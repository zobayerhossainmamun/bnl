#include "stdlib/registry.h"

#include <fmt/core.h>
#include <uv.h>

#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "bnl/interpreter.h"
#include "bnl/native_module.h"

namespace bnl {

namespace {

// ===========================================================================
// Child process I/O via uv_spawn + 3 paired pipes (stdin / stdout / stderr).
//
// Lifetime model:
//   Each spawn allocates one ProcessState on the heap. It owns the
//   uv_process_t and three uv_pipe_t handles, plus a back-pointer in each
//   PipeStream so libuv read callbacks can find their parent. `pending_closes`
//   starts at 4 (process + 3 pipes); each on_close decrements it and the
//   state self-destructs when it hits 0. Lambdas in the returned process
//   module hold a `shared_ptr<bool> alive` and check it before deref —
//   same pattern as net.cpp.
// ===========================================================================

struct ProcessState;

struct PipeStream {
    ProcessState*  parent  = nullptr;
    uv_pipe_t      pipe{};
    CallablePtr    on_data;
    CallablePtr    on_end;
    bool           ended   = false;   // guards double-firing on_end
};

struct ProcessState {
    Interpreter*           interp = nullptr;
    uv_process_t           process{};
    PipeStream             stdin_s;
    PipeStream             stdout_s;
    PipeStream             stderr_s;
    CallablePtr            on_exit;
    std::shared_ptr<bool>  alive;
    int                    pending_closes = 0;  // bumped after successful spawn
    bool                   on_exit_fired  = false;
};

struct WriteReq {
    uv_write_t             req{};
    std::string            data;
    CallablePtr            on_done;
    Interpreter*           interp = nullptr;
    std::shared_ptr<bool>  alive;
};

// ---------- callback safety net --------------------------------------------

void invoke_cb(Interpreter& interp, const CallablePtr& cb, std::vector<Value> args) {
    if (!cb) return;
    try {
        cb->call(interp, std::move(args));
    } catch (ThrowSignal& sig) {
        fmt::print(stderr, "uncaught throw in exec callback: {}\n",
                   sig.value.to_display());
        interp.mark_loop_failed();
    } catch (const RuntimeError& e) {
        fmt::print(stderr, "uncaught error in exec callback at {}:{} (near '{}'): {}\n",
                   e.token.line, e.token.column, e.token.lexeme, e.what());
        interp.mark_loop_failed();
    } catch (const std::exception& e) {
        fmt::print(stderr, "uncaught error in exec callback: {}\n", e.what());
        interp.mark_loop_failed();
    }
}

void alloc_buf(uv_handle_t*, std::size_t suggested, uv_buf_t* buf) {
    buf->base = static_cast<char*>(std::malloc(suggested));
    buf->len  = static_cast<decltype(uv_buf_t::len)>(suggested);
}

// ---------- close paths ----------------------------------------------------

void check_state_dead(ProcessState* state) {
    if (state->pending_closes == 0) {
        if (state->alive) *state->alive = false;
        delete state;
    }
}

void on_pipe_close(uv_handle_t* h) {
    auto* p = static_cast<PipeStream*>(h->data);
    p->parent->pending_closes--;
    check_state_dead(p->parent);
}

void on_process_close(uv_handle_t* h) {
    auto* state = static_cast<ProcessState*>(h->data);
    state->pending_closes--;
    check_state_dead(state);
}

void close_pipe_once(PipeStream& p) {
    auto* h = reinterpret_cast<uv_handle_t*>(&p.pipe);
    if (!uv_is_closing(h)) {
        uv_read_stop(reinterpret_cast<uv_stream_t*>(&p.pipe));
        uv_close(h, on_pipe_close);
    }
}

// ---------- pipe read / write callbacks ------------------------------------

void on_pipe_read(uv_stream_t* s, ssize_t nread, const uv_buf_t* buf) {
    auto* p = static_cast<PipeStream*>(s->data);
    if (nread > 0) {
        std::string chunk(buf->base, static_cast<std::size_t>(nread));
        if (buf->base) std::free(buf->base);
        invoke_cb(*p->parent->interp, p->on_data, { Value{std::move(chunk)} });
        return;
    }
    if (buf->base) std::free(buf->base);
    if (nread == 0) return;
    if (!p->ended) {
        p->ended = true;
        invoke_cb(*p->parent->interp, p->on_end, {});
    }
    close_pipe_once(*p);
}

void on_write_done(uv_write_t* req, int status) {
    auto* wr = static_cast<WriteReq*>(req->data);
    if (wr->on_done && wr->alive && *wr->alive) {
        Value err = (status < 0)
            ? Value{std::string(uv_strerror(status))}
            : Value{};
        invoke_cb(*wr->interp, wr->on_done, { std::move(err) });
    }
    delete wr;
}

void on_proc_exit(uv_process_t* req, int64_t exit_status, int term_signal) {
    auto* state = static_cast<ProcessState*>(req->data);
    if (!state->on_exit_fired) {
        state->on_exit_fired = true;
        invoke_cb(*state->interp, state->on_exit, {
            Value{static_cast<double>(exit_status)},
            Value{static_cast<double>(term_signal)}
        });
    }
    auto* ph = reinterpret_cast<uv_handle_t*>(&state->process);
    if (!uv_is_closing(ph)) uv_close(ph, on_process_close);
    // Pipes typically EOF on their own when the child exits, but close them
    // explicitly here in case the user never installed read handlers.
    close_pipe_once(state->stdin_s);
    close_pipe_once(state->stdout_s);
    close_pipe_once(state->stderr_s);
}

// ---------- per-stream sub-modules -----------------------------------------

ModulePtr build_stdin_module(PipeStream* p, std::shared_ptr<bool> alive,
                              Interpreter* interp) {
    return NativeModule("stdin")
        .add_function("write", -1,
            [p, alive, interp](Interpreter&, std::vector<Value> args) -> Value {
                if (args.empty() || args.size() > 2)
                    throw std::runtime_error("stdin.write(data, on_done?): wrong arity");
                if (!*alive)
                    throw std::runtime_error("stdin.write: process is gone");
                auto* h = reinterpret_cast<uv_handle_t*>(&p->pipe);
                if (uv_is_closing(h))
                    throw std::runtime_error("stdin.write: stdin is closed");
                if (!args[0].is_string())
                    throw std::runtime_error("stdin.write: data must be a string");

                CallablePtr on_done;
                if (args.size() == 2 && !args[1].is_null()) {
                    if (!args[1].is_callable())
                        throw std::runtime_error("stdin.write: on_done must be a function");
                    on_done = args[1].as_callable();
                }

                auto* wr     = new WriteReq{};
                wr->data     = args[0].as_string();
                wr->on_done  = std::move(on_done);
                wr->interp   = interp;
                wr->alive    = alive;
                wr->req.data = wr;

                uv_buf_t buf = uv_buf_init(wr->data.data(),
                                           static_cast<unsigned int>(wr->data.size()));
                int rc = uv_write(&wr->req,
                                  reinterpret_cast<uv_stream_t*>(&p->pipe),
                                  &buf, 1, on_write_done);
                if (rc < 0) {
                    delete wr;
                    throw std::runtime_error(std::string("stdin.write: ") + uv_strerror(rc));
                }
                return Value{};
            })

        // Closing stdin sends EOF to the child.
        .add_function("close", 0,
            [p, alive](Interpreter&, std::vector<Value>) -> Value {
                if (*alive) close_pipe_once(*p);
                return Value{};
            })
        .build();
}

ModulePtr build_readable_module(PipeStream* p, std::shared_ptr<bool> alive,
                                 const char* name) {
    return NativeModule(name)
        .add_function("on_data", 1,
            [p, alive](Interpreter&, std::vector<Value> args) -> Value {
                if (!*alive) return Value{};
                if (!args[0].is_callable())
                    throw std::runtime_error("on_data: callback must be a function");
                p->on_data = args[0].as_callable();
                return Value{};
            })
        .add_function("on_end", 1,
            [p, alive](Interpreter&, std::vector<Value> args) -> Value {
                if (!*alive) return Value{};
                if (!args[0].is_callable())
                    throw std::runtime_error("on_end: callback must be a function");
                p->on_end = args[0].as_callable();
                return Value{};
            })
        .build();
}

ModulePtr build_process_module(ProcessState* state) {
    auto         alive  = state->alive;
    Interpreter* interp = state->interp;

    return NativeModule("process")
        .add_value("pid",    Value{static_cast<double>(state->process.pid)})
        .add_value("stdin",  Value{build_stdin_module   (&state->stdin_s,  alive, interp)})
        .add_value("stdout", Value{build_readable_module(&state->stdout_s, alive, "stdout")})
        .add_value("stderr", Value{build_readable_module(&state->stderr_s, alive, "stderr")})

        // kill(sig?) — sig defaults to SIGTERM (15). On Windows uv translates
        // SIGTERM to TerminateProcess.
        .add_function("kill", -1,
            [state, alive](Interpreter&, std::vector<Value> args) -> Value {
                if (!*alive) return Value{};
                int sig = SIGTERM;
                if (args.size() == 1 && args[0].is_number())
                    sig = static_cast<int>(args[0].as_number());
                uv_process_kill(&state->process, sig);
                return Value{};
            })
        .build();
}

}  // namespace

// Registered as "_exec" so the public "exec" name belongs to lib/exec.bnl,
// which re-exports spawn and adds higher-level helpers (run, run_with_input).
// Users wanting the bare primitive can `import "_exec"` directly.
void register_exec(Interpreter& interp) {
    auto m = NativeModule("_exec")

        // exec.spawn(cmd, args, opts, on_exit) — start a child process.
        // cmd:     program path or name (Windows uses PATH lookup)
        // args:    list of strings, NOT including argv[0] (we add it)
        // opts:    map (or null). Recognised keys:
        //              cwd: string — child's working directory
        //              env: map of string → string — replaces inherited env
        // on_exit: function(code, signal) — fires when the child exits
        //
        // Returns a process module:
        //   .pid                  — child PID (number)
        //   .stdin.write(d, cb?)  — push bytes to child's stdin
        //   .stdin.close()        — send EOF
        //   .stdout.on_data(fn)   — receive child stdout chunks
        //   .stdout.on_end(fn)    — fires when child closes stdout
        //   .stderr.on_data(fn)   — same for stderr
        //   .stderr.on_end(fn)
        //   .kill(sig?)           — default SIGTERM
        .add_function("spawn", 4,
            [&interp](Interpreter&, std::vector<Value> args) -> Value {
                if (!args[0].is_string())
                    throw std::runtime_error("exec.spawn: cmd must be a string");
                if (!args[1].is_list())
                    throw std::runtime_error("exec.spawn: args must be a list");
                if (!args[2].is_map() && !args[2].is_null())
                    throw std::runtime_error("exec.spawn: opts must be a map or null");
                if (!args[3].is_callable())
                    throw std::runtime_error("exec.spawn: on_exit must be a function");

                const std::string& cmd       = args[0].as_string();
                const auto&        user_args = *args[1].as_list();
                CallablePtr        on_exit   = args[3].as_callable();

                // argv = [cmd, args..., NULL]. Strings owned by argv_storage,
                // pointers in argv. Both must outlive uv_spawn.
                std::vector<std::string> argv_storage;
                argv_storage.reserve(user_args.size() + 1);
                argv_storage.push_back(cmd);
                for (const auto& a : user_args) {
                    if (!a.is_string())
                        throw std::runtime_error("exec.spawn: args must all be strings");
                    argv_storage.push_back(a.as_string());
                }
                std::vector<char*> argv;
                argv.reserve(argv_storage.size() + 1);
                for (auto& s : argv_storage) argv.push_back(const_cast<char*>(s.c_str()));
                argv.push_back(nullptr);

                // Optional cwd / env from opts map.
                std::string              cwd_str;
                std::vector<std::string> env_storage;
                std::vector<char*>       envp;
                const char*              cwd_cstr = nullptr;
                char**                   envp_arr = nullptr;
                if (args[2].is_map()) {
                    auto opts = args[2].as_map();
                    if (auto it = opts->find("cwd"); it != opts->end()) {
                        if (!it->second.is_string())
                            throw std::runtime_error("exec.spawn: opts.cwd must be a string");
                        cwd_str  = it->second.as_string();
                        cwd_cstr = cwd_str.c_str();
                    }
                    if (auto it = opts->find("env"); it != opts->end()) {
                        if (!it->second.is_map())
                            throw std::runtime_error("exec.spawn: opts.env must be a map");
                        for (const auto& [k, v] : *it->second.as_map()) {
                            if (!v.is_string())
                                throw std::runtime_error("exec.spawn: env values must be strings");
                            env_storage.push_back(k + "=" + v.as_string());
                        }
                        for (auto& s : env_storage) envp.push_back(const_cast<char*>(s.c_str()));
                        envp.push_back(nullptr);
                        envp_arr = envp.data();
                    }
                }

                auto* state = new ProcessState{};
                state->interp  = &interp;
                state->alive   = std::make_shared<bool>(true);
                state->on_exit = std::move(on_exit);
                state->process.data    = state;
                state->stdin_s.parent  = state;
                state->stdout_s.parent = state;
                state->stderr_s.parent = state;

                uv_pipe_init(interp.loop(), &state->stdin_s.pipe,  0);
                uv_pipe_init(interp.loop(), &state->stdout_s.pipe, 0);
                uv_pipe_init(interp.loop(), &state->stderr_s.pipe, 0);
                state->stdin_s.pipe.data  = &state->stdin_s;
                state->stdout_s.pipe.data = &state->stdout_s;
                state->stderr_s.pipe.data = &state->stderr_s;

                uv_stdio_container_t stdio[3] = {};
                // Child reads from stdin, writes to stdout / stderr —
                // flags describe the CHILD end of each pipe.
                stdio[0].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_READABLE_PIPE);
                stdio[0].data.stream = reinterpret_cast<uv_stream_t*>(&state->stdin_s.pipe);
                stdio[1].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
                stdio[1].data.stream = reinterpret_cast<uv_stream_t*>(&state->stdout_s.pipe);
                stdio[2].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
                stdio[2].data.stream = reinterpret_cast<uv_stream_t*>(&state->stderr_s.pipe);

                uv_process_options_t options = {};
                options.exit_cb     = on_proc_exit;
                options.file        = argv_storage[0].c_str();
                options.args        = argv.data();
                options.cwd         = cwd_cstr;
                options.env         = envp_arr;
                options.stdio_count = 3;
                options.stdio       = stdio;
                options.flags       = 0;

                state->pending_closes = 4;  // process + 3 pipes
                int rc = uv_spawn(interp.loop(), &state->process, &options);
                if (rc < 0) {
                    // uv_spawn always initializes the process handle, even on
                    // failure — close it (and the pipes) so the loop can
                    // garbage-collect the state.
                    auto* ph = reinterpret_cast<uv_handle_t*>(&state->process);
                    if (!uv_is_closing(ph)) uv_close(ph, on_process_close);
                    close_pipe_once(state->stdin_s);
                    close_pipe_once(state->stdout_s);
                    close_pipe_once(state->stderr_s);
                    throw std::runtime_error(std::string("exec.spawn: ") + uv_strerror(rc));
                }

                uv_read_start(reinterpret_cast<uv_stream_t*>(&state->stdout_s.pipe),
                              alloc_buf, on_pipe_read);
                uv_read_start(reinterpret_cast<uv_stream_t*>(&state->stderr_s.pipe),
                              alloc_buf, on_pipe_read);

                return Value{build_process_module(state)};
            })

        .build();

    interp.register_native_module("_exec", m);
}

}  // namespace bnl
