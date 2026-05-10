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
        if (auto* i = g_active_interp.load()) uv_stop(i->loop());
        return TRUE;  // we handled it; don't terminate the process
    }
    return FALSE;
}
#else
extern "C" void posix_signal_handler(int /*sig*/) {
    // Async-signal-safe: only an atomic store and uv_stop (= one int write).
    g_interrupted.store(true, std::memory_order_release);
    if (auto* i = g_active_interp.load()) uv_stop(i->loop());
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
    register_builtins();
    register_sys   (*this);
    register_io    (*this);
    register_timers(*this);
    register_regex (*this);
    register_crypto(*this);
    register_net   (*this);
    register_http  (*this);
    register_tls   (*this);
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
    // Take ownership of the AST. UserFunctions (and any closures over them)
    // hold raw pointers into it; the AST must live at least as long as the
    // Interpreter so REPL / async-callback paths don't dangle.
    kept_programs_.push_back(std::move(program));
    auto& prog = kept_programs_.back();

    // RuntimeError / ModuleError propagate to the caller — main.cpp has the
    // source + path and renders a clang-style diagnostic. run_source catches
    // them itself for embedders that don't want exceptions in their host.
    for (const auto& s : prog) execute(*s);

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
