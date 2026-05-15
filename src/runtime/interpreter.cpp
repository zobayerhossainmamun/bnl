#include "bnl/interpreter.h"

#include <fmt/core.h>

#include <atomic>
#include <mutex>
#include <utility>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <csignal>
#endif

#include "runtime/environment.h"
#include "frontend/lexer.h"
#include "runtime/future.h"
#include "runtime/internal.h"
#include "runtime/module_loader.h"
#include "frontend/parser.h"
#include "stdlib/registry.h"

namespace bnl {

namespace {

// Process-wide interrupt latch. Set by signal handlers (Ctrl-C / SIGTERM),
// polled by Interpreter::check_interrupt at hot points (loops, calls). The
// interpreter clears it when it raises the corresponding RuntimeError.
std::atomic<bool> g_interrupted{false};

// The interpreter currently driving uv_run, if any. The signal handler uses
// it to call uv_stop so an idle uv_run wakes up promptly. Multiple nested
// interpreters are not a thing here — we just track the latest active one.
std::atomic<Interpreter*> g_active_interp{nullptr};

class ActiveScope {
public:
    explicit ActiveScope(Interpreter* now)
        : prev_(g_active_interp.exchange(now)) {}
    ~ActiveScope() { g_active_interp.store(prev_); }
private:
    Interpreter* prev_;
};

#ifdef _WIN32
BOOL WINAPI ctrl_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        g_interrupted.store(true, std::memory_order_release);
        if (auto* i = g_active_interp.load()) i->notify_stop();
        return TRUE;  // we handled it; don't terminate the process
    }
    return FALSE;
}
#else
extern "C" void posix_signal_handler(int /*sig*/) {
    // Async-signal-safe: only an atomic store and notify_stop (which is
    // itself just an atomic store + a single write to an eventfd).
    g_interrupted.store(true, std::memory_order_release);
    if (auto* i = g_active_interp.load()) i->notify_stop();
}
#endif

}  // namespace

// ============================================================================
// Interpreter — lifecycle, control, and native module registry.
// (Visitor implementations live in exprs.cpp / stmts.cpp; built-in registration
// in builtins.cpp; private helpers and UserFunction in interpreter/internal.h.)
// ============================================================================

Interpreter::Interpreter()
    : globals_(std::make_shared<Environment>()),
      environment_(globals_),
      modules_(std::make_unique<ModuleLoader>(*this)) {
    uv_loop_init(&loop_);
    // Async handle whose only job is to wake the loop when notify_stop() is
    // called (typically from a Ctrl-C signal handler on another thread). The
    // callback is a no-op; the wake itself is what matters. uv_unref so this
    // handle never holds the loop alive on its own — natural termination
    // (script finishes, no other handles) still works.
    uv_async_init(&loop_, &stop_async_, [](uv_async_t*) {});
    uv_unref(reinterpret_cast<uv_handle_t*>(&stop_async_));

    // Prepare handle drains the Future microtask queue at the start of every
    // loop iteration. unref'd so it doesn't keep the loop alive on its own —
    // natural termination still works when the queue is empty.
    uv_prepare_init(&loop_, &microtask_prep_);
    microtask_prep_.data = this;
    uv_prepare_start(&microtask_prep_, [](uv_prepare_t* h) {
        static_cast<Interpreter*>(h->data)->drain_microtasks();
    });
    uv_unref(reinterpret_cast<uv_handle_t*>(&microtask_prep_));

    register_builtins();
    register_future(*this);
    register_sys   (*this);
    register_io    (*this);
    register_timers(*this);
    register_regex (*this);
    register_crypto(*this);
    register_net   (*this);
    register_http  (*this);
    register_tls   (*this);
    register_json  (*this);
    register_exec  (*this);
    register_dns   (*this);
    register_sqlite(*this);
    register_pg    (*this);
    register_mongo (*this);
    register_math  (*this);
    register_random(*this);
    register_time  (*this);
    register_zlib  (*this);
}

Interpreter::~Interpreter() {
    // Force-close any handles that survived (e.g. user crashed mid-loop).
    uv_walk(&loop_, [](uv_handle_t* h, void*) {
        if (!uv_is_closing(h)) uv_close(h, nullptr);
    }, nullptr);
    uv_run(&loop_, UV_RUN_DEFAULT);  // drain close callbacks
    uv_loop_close(&loop_);
}

void Interpreter::register_native_module(const std::string& name, ModulePtr m) {
    native_modules_[name] = std::move(m);
}

ModulePtr Interpreter::native_module(const std::string& name) const {
    auto it = native_modules_.find(name);
    return it != native_modules_.end() ? it->second : nullptr;
}

void Interpreter::mark_loop_failed() {
    loop_failed_ = true;
    uv_stop(&loop_);
}

void Interpreter::enqueue_microtask(std::function<void()> fn) {
    // While there are pending microtasks, ref the prepare handle so the
    // libuv loop stays alive long enough to drain them. Without this, a
    // request that completes inside a libuv callback can settle a Future
    // (enqueuing the .next continuation as a microtask), then exit the
    // loop before that continuation ever fires — symptom: async code goes
    // silent after an I/O callback while callback-style code still works.
    if (microtasks_.empty()) {
        uv_ref(reinterpret_cast<uv_handle_t*>(&microtask_prep_));
    }
    microtasks_.push_back(std::move(fn));
}

void Interpreter::drain_microtasks() {
    // Pop one at a time so microtasks enqueued during a drain run in the
    // same pass (standard Promise semantics). Any exception from a microtask
    // is logged to stderr and the queue keeps draining — the alternative
    // (letting one bad continuation kill the whole loop) is worse.
    while (!microtasks_.empty()) {
        auto fn = std::move(microtasks_.front());
        microtasks_.pop_front();
        try {
            fn();
        } catch (const std::exception& e) {
            fmt::print(stderr, "unhandled error in microtask: {}\n", e.what());
            loop_failed_ = true;
        } catch (...) {
            fmt::print(stderr, "unhandled error in microtask: <unknown>\n");
            loop_failed_ = true;
        }
    }
    // Queue empty — release the loop so it can exit naturally if no other
    // referenced handles remain.
    uv_unref(reinterpret_cast<uv_handle_t*>(&microtask_prep_));
}

// ----------------------------------------------------------------------------
// Async function execution.
//
// run_async_body kicks off the stepper with:
//   on_complete = resolve outer with null (implicit return)
//   on_break    = nullptr (not in an async loop)
//   on_continue = nullptr
//
// async_step walks a list of statements. Pure-sync stmts (no wait inside)
// run via execute() in a tight loop. Any statement containing a wait —
// either at the top level (`var x = wait e;` / `wait e;`) or nested inside
// a block / if / while / for-of — delegates to async_step_one and unwinds
// the C++ stack; a microtask continuation re-enters async_step at idx+1.
//
// Signals: `return v` resolves out. `throw v` (or a rejected wait, or a
// runtime error) rejects out. `break` / `continue` are routed to the
// nearest async loop's callbacks; outside a loop they reject out with a
// diagnostic. `on_complete` only fires when a block finishes naturally.
// ----------------------------------------------------------------------------

FuturePtr Interpreter::run_async_body(const std::vector<StmtPtr>* body,
                                      std::shared_ptr<Environment> env) {
    auto out = std::make_shared<Future>();
    FuturePtr out_capture = out;
    // Top-level throw fallthrough: reject the outer Future.
    ThrowCb top_throw = [this, out_capture](Value e) {
        out_capture->reject(*this, std::move(e));
    };
    async_step(body, 0, std::move(env), out,
        /*on_complete=*/[this, out_capture]() {
            out_capture->resolve(*this, Value{});
        },
        /*on_break=*/   nullptr,
        /*on_continue=*/nullptr,
        /*on_throw=*/   std::move(top_throw));
    return out;
}

void Interpreter::async_step(const std::vector<StmtPtr>* body,
                              std::size_t                  idx,
                              std::shared_ptr<Environment> env,
                              FuturePtr                    out,
                              StepCb                       on_complete,
                              StepCb                       on_break,
                              StepCb                       on_continue,
                              ThrowCb                      on_throw) {
    auto saved_env = environment_;
    environment_   = env;

    while (idx < body->size()) {
        Stmt& s = *(*body)[idx];

        if (interp_detail::stmt_contains_wait(s)) {
            environment_ = saved_env;
            auto next = [this, body, idx, env, out, on_complete, on_break, on_continue, on_throw]() {
                this->async_step(body, idx + 1, env, out, on_complete, on_break, on_continue, on_throw);
            };
            async_step_one(s, env, out, next, on_break, on_continue, on_throw);
            return;
        }

        try {
            execute(s);
        } catch (ReturnSignal& r) {
            environment_ = saved_env;
            out->resolve(*this, std::move(r.value));
            return;
        } catch (BreakSignal&) {
            environment_ = saved_env;
            if (on_break) on_break();
            else          on_throw(Value{std::string("'break' outside a loop")});
            return;
        } catch (ContinueSignal&) {
            environment_ = saved_env;
            if (on_continue) on_continue();
            else             on_throw(Value{std::string("'continue' outside a loop")});
            return;
        } catch (ThrowSignal& t) {
            environment_ = saved_env;
            on_throw(std::move(t.value));
            return;
        } catch (const RuntimeError& re) {
            environment_ = saved_env;
            on_throw(Value{std::string(re.what())});
            return;
        } catch (const std::exception& ex) {
            environment_ = saved_env;
            on_throw(Value{std::string(ex.what())});
            return;
        }
        ++idx;
    }

    environment_ = saved_env;
    on_complete();
}

void Interpreter::async_step_one(Stmt&                        s,
                                  std::shared_ptr<Environment> env,
                                  FuturePtr                    out,
                                  StepCb                       on_done,
                                  StepCb                       on_break,
                                  StepCb                       on_continue,
                                  ThrowCb                      on_throw) {
    auto saved_env = environment_;
    environment_   = env;

    // ---- Top-level wait -----------------------------------------------------
    WaitExpr*   wait_e = nullptr;
    std::string bind_name;
    if (auto* vs = dynamic_cast<VarStmt*>(&s)) {
        if (vs->initializer) {
            if (auto* w = dynamic_cast<WaitExpr*>(vs->initializer.get())) {
                wait_e    = w;
                bind_name = std::string(vs->name.lexeme);
            }
        }
    } else if (auto* es = dynamic_cast<ExpressionStmt*>(&s)) {
        if (auto* w = dynamic_cast<WaitExpr*>(es->expr.get())) {
            wait_e = w;
        }
    }
    if (wait_e) {
        Value fut_val;
        try {
            fut_val = evaluate(*wait_e->operand);
        } catch (ThrowSignal& t) {
            environment_ = saved_env; on_throw(std::move(t.value)); return;
        } catch (const RuntimeError& re) {
            environment_ = saved_env; on_throw(Value{std::string(re.what())}); return;
        } catch (const std::exception& ex) {
            environment_ = saved_env; on_throw(Value{std::string(ex.what())}); return;
        }

        FuturePtr fut;
        if (fut_val.is_future()) {
            fut = fut_val.as_future();
        } else {
            fut = std::make_shared<Future>();
            fut->resolve(*this, std::move(fut_val));
        }

        auto on_ok = std::make_shared<NativeFunction>("__step_ok", 1,
            [env, on_done, bind_name](Interpreter& /*i*/,
                                      std::vector<Value> args) -> Value {
                Value v = args.empty() ? Value{} : std::move(args[0]);
                if (!bind_name.empty()) env->define(bind_name, std::move(v));
                on_done();
                return Value{};
            });
        auto on_err = std::make_shared<NativeFunction>("__step_err", 1,
            [on_throw](Interpreter& /*i*/, std::vector<Value> args) -> Value {
                on_throw(args.empty() ? Value{} : std::move(args[0]));
                return Value{};
            });
        environment_ = saved_env;
        fut->add_next(*this, on_ok, on_err);
        return;
    }

    // ---- BlockStmt — fresh scope, step through inner list -------------------
    if (auto* bs = dynamic_cast<BlockStmt*>(&s)) {
        auto block_env = std::make_shared<Environment>(env);
        environment_ = saved_env;
        async_step(&bs->statements, 0, block_env, out, on_done, on_break, on_continue, on_throw);
        return;
    }

    // ---- IfStmt with wait somewhere inside ----------------------------------
    if (auto* is = dynamic_cast<IfStmt*>(&s)) {
        Value cond_val;
        try {
            cond_val = evaluate(*is->cond);
        } catch (ThrowSignal& t)            { environment_ = saved_env; on_throw(std::move(t.value)); return; }
          catch (const RuntimeError& re)    { environment_ = saved_env; on_throw(Value{std::string(re.what())}); return; }
          catch (const std::exception& ex)  { environment_ = saved_env; on_throw(Value{std::string(ex.what())}); return; }

        Stmt* chosen = cond_val.truthy() ? is->then_branch.get()
                                         : is->else_branch.get();
        environment_ = saved_env;
        if (!chosen) { on_done(); return; }
        async_step_one(*chosen, env, out, on_done, on_break, on_continue, on_throw);
        return;
    }

    // ---- WhileStmt with wait inside -----------------------------------------
    if (auto* ws = dynamic_cast<WhileStmt*>(&s)) {
        environment_ = saved_env;
        async_run_while(*ws, env, out, on_done, on_throw);
        return;
    }

    // ---- ForOfStmt with wait inside -----------------------------------------
    if (auto* fos = dynamic_cast<ForOfStmt*>(&s)) {
        environment_ = saved_env;
        async_run_forof(*fos, env, out, on_done, on_throw);
        return;
    }

    // ---- TryStmt with wait inside -------------------------------------------
    if (auto* ts = dynamic_cast<TryStmt*>(&s)) {
        environment_ = saved_env;
        async_run_try(*ts, env, out, on_done, on_break, on_continue, on_throw);
        return;
    }

    // Anything else (ForStmt with wait, SwitchStmt with wait, etc.) — not yet
    // supported. Route the diagnostic through on_throw so the caller's catch
    // (if any) can intercept it; outside try, it lands on the outer Future.
    environment_ = saved_env;
    on_throw(Value{std::string(
        "`wait` is not supported in this position. "
        "Use it at statement top level, or inside a block, if/else, "
        "while, for-of, or try/catch.")});
}

void Interpreter::async_run_while(WhileStmt& w,
                                   std::shared_ptr<Environment> env,
                                   FuturePtr                    out,
                                   StepCb                       on_done,
                                   ThrowCb                      on_throw) {
    auto loop_iter = std::make_shared<std::function<void()>>();
    WhileStmt* w_ptr = &w;
    *loop_iter = [this, w_ptr, env, out, on_done, on_throw, loop_iter]() {
        Value cond_val;
        try {
            auto saved = this->environment_;
            this->environment_ = env;
            cond_val = this->evaluate(*w_ptr->cond);
            this->environment_ = saved;
        } catch (ThrowSignal& t)           { on_throw(std::move(t.value)); return; }
          catch (const RuntimeError& re)   { on_throw(Value{std::string(re.what())}); return; }
          catch (const std::exception& ex) { on_throw(Value{std::string(ex.what())}); return; }

        if (!cond_val.truthy()) {
            on_done();
            return;
        }

        auto next_iter = [loop_iter]() { (*loop_iter)(); };
        this->async_step_one(*w_ptr->body, env, out,
            /*on_done=*/    next_iter,
            /*on_break=*/   on_done,
            /*on_continue=*/next_iter,
            /*on_throw=*/   on_throw);
    };
    (*loop_iter)();
}

void Interpreter::async_run_forof(ForOfStmt& fos,
                                   std::shared_ptr<Environment> env,
                                   FuturePtr                    out,
                                   StepCb                       on_done,
                                   ThrowCb                      on_throw) {
    Value iter_val;
    try {
        auto saved = environment_;
        environment_ = env;
        iter_val = evaluate(*fos.iterable);
        environment_ = saved;
    } catch (ThrowSignal& t)           { on_throw(std::move(t.value)); return; }
      catch (const RuntimeError& re)   { on_throw(Value{std::string(re.what())}); return; }
      catch (const std::exception& ex) { on_throw(Value{std::string(ex.what())}); return; }

    if (!iter_val.is_list()) {
        on_throw(Value{std::string("for-of: iterable must be a list")});
        return;
    }
    ListPtr     list      = iter_val.as_list();
    std::string var_name(fos.var.lexeme);
    Stmt*       body_stmt = fos.body.get();

    auto run_iter = std::make_shared<std::function<void(std::size_t)>>();
    *run_iter = [this, list, var_name, body_stmt, env, out, on_done, on_throw, run_iter]
                (std::size_t i) {
        if (i >= list->size()) {
            on_done();
            return;
        }
        auto iter_env = std::make_shared<Environment>(env);
        iter_env->define(var_name, (*list)[i]);

        auto next = [run_iter, i]() { (*run_iter)(i + 1); };
        this->async_step_one(*body_stmt, iter_env, out,
            /*on_done=*/    next,
            /*on_break=*/   on_done,
            /*on_continue=*/next,
            /*on_throw=*/   on_throw);
    };
    (*run_iter)(0);
}

void Interpreter::async_run_try(TryStmt& ts,
                                 std::shared_ptr<Environment> env,
                                 FuturePtr                    out,
                                 StepCb                       on_done,
                                 StepCb                       on_break,
                                 StepCb                       on_continue,
                                 ThrowCb                      on_throw) {
    // `finally` containing `wait` is not yet supported — the value-tracking
    // semantics through finally (run on every exit path, override pending
    // return/throw if finally itself returns/throws) need more work. Plain
    // `try { wait ... } catch (e) {...}` without finally is fully supported.
    if (ts.has_finally) {
        on_throw(Value{std::string(
            "`finally` is not yet supported when `wait` appears inside try/catch. "
            "Use try/catch without finally.")});
        return;
    }
    if (!ts.has_catch) {
        // try { } finally — but finally not supported above. Treat as no-op
        // wrapping (run try, propagate everything).
        async_step(&ts.try_block, 0, env, out,
                   on_done, on_break, on_continue, on_throw);
        return;
    }

    // Snapshot the catch shape so the on_throw closure can run the catch
    // block when the try body rejects.
    std::string catch_var(ts.catch_var.lexeme);
    const std::vector<StmtPtr>* catch_block = &ts.catch_block;

    ThrowCb try_throw = [this, env, out, catch_var, catch_block,
                         on_done, on_break, on_continue, on_throw](Value thrown) {
        // Bind the thrown value in a fresh scope and run the catch block.
        // Inside the catch, normal throw routes back to the outer on_throw
        // (catches don't re-catch themselves).
        auto catch_env = std::make_shared<Environment>(env);
        catch_env->define(catch_var, std::move(thrown));
        this->async_step(catch_block, 0, catch_env, out,
                         on_done, on_break, on_continue, on_throw);
    };

    async_step(&ts.try_block, 0, env, out,
               on_done, on_break, on_continue, std::move(try_throw));
}

void Interpreter::notify_stop() {
    // Both calls are documented thread-safe by libuv. uv_stop alone only sets
    // a flag — the loop won't see it until it wakes from its current I/O
    // wait. uv_async_send is the cross-thread wake primitive that triggers
    // the wake immediately (PostQueuedCompletionStatus on Windows IOCP /
    // an eventfd write on Linux).
    uv_stop(&loop_);
    uv_async_send(&stop_async_);
}

void Interpreter::install_signal_handlers() {
    static std::once_flag once;
    std::call_once(once, [] {
#ifdef _WIN32
        // Returning TRUE from ctrl_handler suppresses default termination.
        SetConsoleCtrlHandler(ctrl_handler, TRUE);
#else
        struct sigaction sa{};
        sa.sa_handler = posix_signal_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        sigaction(SIGINT,  &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);
#endif
    });
}

void Interpreter::check_call_depth(const Token& site) {
    if (call_depth_ >= max_call_depth_) {
        throw RuntimeError(site,
            fmt::format("max call depth ({}) exceeded — likely infinite recursion",
                        max_call_depth_));
    }
}

void Interpreter::check_interrupt(const Token& site) {
    // Acquire pairs with the signal handler's release store.
    if (g_interrupted.load(std::memory_order_acquire)) {
        // Latch consumed: clear so subsequent runs don't auto-fail. Preserves
        // the property that Ctrl-C interrupts ONE script run.
        g_interrupted.store(false, std::memory_order_release);
        throw RuntimeError(site, "interrupted (Ctrl-C / SIGTERM)");
    }
}

bool Interpreter::run(std::vector<StmtPtr> program,
                      const std::filesystem::path& entry_path) {
    // Mark this interpreter as the signal-handler target for the duration of
    // the run. RAII so it is restored even if execute() / uv_run throws.
    ActiveScope scope(this);

    current_file_ = entry_path;
    if (entry_path_.empty()) entry_path_ = entry_path;
    // Take ownership of the AST. UserFunctions (and any closures over them)
    // hold raw pointers into it; the AST must live at least as long as the
    // Interpreter so REPL / async-callback paths don't dangle.
    kept_programs_.push_back(std::move(program));
    auto& prog = kept_programs_.back();

    // RuntimeError / ModuleError propagate to the caller — main.cpp has the
    // source + path and renders a clang-style diagnostic. run_source catches
    // them itself for embedders that don't want exceptions in their host.
    for (const auto& s : prog) execute(*s);

    // Drain microtasks queued by purely synchronous Future chains
    // (e.g. `Future.of(7).next(fn)` at top level) before handing off
    // to libuv. The prepare handle drains again at the start of each
    // subsequent loop iteration.
    drain_microtasks();

    // Drain the event loop. Async callbacks (timers, fs, net, ...) run here.
    // If any of them sets loop_failed_, the program exits non-zero.
    uv_run(&loop_, UV_RUN_DEFAULT);
    return !loop_failed_;
}

bool Interpreter::run_source(const std::string&            source,
                             const std::filesystem::path&  path) {
    Lexer lexer(source);
    auto  tokens = lexer.tokenize();
    if (lexer.has_errors()) {
        for (const auto& d : lexer.diagnostics()) {
            fmt::print(stderr, "lex error at {}:{}: {}\n", d.line, d.column, d.message);
        }
        return false;
    }

    Parser parser(std::move(tokens));
    auto   program = parser.parse();
    if (parser.has_errors()) {
        for (const auto& d : parser.diagnostics()) {
            fmt::print(stderr, "parse error at {}:{}: {}\n", d.line, d.column, d.message);
        }
        return false;
    }

    try {
        return run(std::move(program), path);
    } catch (const RuntimeError& e) {
        fmt::print(stderr, "runtime error at {}:{} (near '{}'): {}\n",
                   e.token.line, e.token.column, e.token.lexeme, e.what());
        return false;
    } catch (const ModuleError& e) {
        fmt::print(stderr, "module error at {}:{} (near '{}'): {}\n",
                   e.token.line, e.token.column, e.token.lexeme, e.what());
        return false;
    } catch (ThrowSignal& sig) {
        // An uncaught `throw <value>;` would otherwise escape past main() into
        // std::terminate, which on Windows pops the MSVC runtime crash dialog.
        fmt::print(stderr, "uncaught throw at {}:{}: {}\n",
                   sig.token.line, sig.token.column, sig.value.to_display());
        return false;
    }
}

void Interpreter::run_module(Module& m) {
    auto previous_file = current_file_;
    current_file_      = m.path();
    try {
        execute_block(m.program(), m.exports());
    } catch (...) {
        current_file_ = previous_file;
        throw;
    }
    current_file_ = previous_file;
}

Value Interpreter::evaluate(Expr& e) {
    e.accept(*this);
    return std::move(result_);
}

void Interpreter::execute(Stmt& s) { s.accept(*this); }

Value Interpreter::evaluate_in(Expr& e, std::shared_ptr<Environment> env) {
    auto saved = environment_;
    environment_ = std::move(env);
    try {
        Value v = evaluate(e);
        environment_ = saved;
        return v;
    } catch (...) {
        environment_ = saved;
        throw;
    }
}

void Interpreter::execute_block(const std::vector<StmtPtr>& stmts,
                                std::shared_ptr<Environment> env) {
    auto previous = environment_;
    environment_ = std::move(env);
    try {
        for (const auto& s : stmts) execute(*s);
    } catch (...) {
        environment_ = previous;
        throw;
    }
    environment_ = previous;
}

}  // namespace bnl
