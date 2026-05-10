#include "native/builtins.h"

#include <fmt/core.h>
#include <uv.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "bnl/interpreter.h"
#include "bnl/native_module.h"

namespace bnl {

namespace {

// ---------- helpers ---------------------------------------------------------

CallablePtr to_callback(const Value& v, const char* where) {
    if (!v.is_callable())
        throw std::runtime_error(fmt::format("{}: callback must be a function, got {}",
                                             where, v.type_name()));
    return v.as_callable();
}

void invoke_callback(Interpreter& interp, const CallablePtr& cb, std::vector<Value> args) {
    if (!cb) return;
    try {
        cb->call(interp, std::move(args));
    } catch (const RuntimeError& e) {
        fmt::print(stderr, "uncaught error in exec callback at {}:{} (near '{}'): {}\n",
                   e.token.line, e.token.column, e.token.lexeme, e.what());
        interp.mark_loop_failed();
    } catch (const std::exception& e) {
        fmt::print(stderr, "uncaught error in exec callback: {}\n", e.what());
        interp.mark_loop_failed();
    }
}

// Translate a bnl list of strings into argv. Caller owns the storage in the
// returned vector<string>; `out_argv` references those strings, terminated by
// nullptr. Both must outlive the uv_spawn call (uv copies internally).
struct Argv {
    std::vector<std::string>  storage;
    std::vector<char*>        argv;          // NULL-terminated
};

Argv build_argv(const std::string& cmd, const Value& args_val) {
    Argv a;
    a.storage.push_back(cmd);
    if (!args_val.is_null()) {
        if (!args_val.is_list())
            throw std::runtime_error("exec: args must be a list of strings");
        for (const auto& v : *args_val.as_list()) {
            if (!v.is_string())
                throw std::runtime_error("exec: each arg must be a string");
            a.storage.push_back(v.as_string());
        }
    }
    for (auto& s : a.storage) a.argv.push_back(s.data());
    a.argv.push_back(nullptr);
    return a;
}

struct EnvList {
    std::vector<std::string>  storage;       // "NAME=VALUE"
    std::vector<char*>        envp;          // NULL-terminated, or empty if inherit
};

EnvList build_env(const Value& env_val) {
    EnvList e;
    if (env_val.is_null()) return e;          // inherit parent env
    if (!env_val.is_map())
        throw std::runtime_error("exec: env must be a map of {name: value}");
    for (const auto& [k, v] : *env_val.as_map()) {
        if (!v.is_string())
            throw std::runtime_error("exec: env values must be strings");
        e.storage.push_back(k + "=" + v.as_string());
    }
    for (auto& s : e.storage) e.envp.push_back(s.data());
    e.envp.push_back(nullptr);
    return e;
}

// ---------- async spawn (process module) -----------------------------------

struct Proc {
    uv_process_t          process{};
    uv_pipe_t             stdin_pipe{};
    uv_pipe_t             stdout_pipe{};
    uv_pipe_t             stderr_pipe{};
    Interpreter*          interp = nullptr;
    CallablePtr           on_exit;            // fn(code, signal)
    CallablePtr           on_stdout;          // fn(chunk)
    CallablePtr           on_stderr;          // fn(chunk)
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    bool                  exited = false;
    int                   pending_closes = 0; // process + 3 pipes = 4
};

// One generic close callback shared by process + all 3 pipes. The Proc is
// freed only after every handle has reported back.
void on_handle_closed(uv_handle_t* h) {
    auto* p = static_cast<Proc*>(h->data);
    if (--p->pending_closes <= 0) {
        if (p->alive) *p->alive = false;
        delete p;
    }
}

void close_proc_handles(Proc* p) {
    auto try_close = [&](uv_handle_t* h) {
        if (!uv_is_closing(h)) {
            ++p->pending_closes;
            uv_close(h, on_handle_closed);
        }
    };
    try_close(reinterpret_cast<uv_handle_t*>(&p->process));
    try_close(reinterpret_cast<uv_handle_t*>(&p->stdin_pipe));
    try_close(reinterpret_cast<uv_handle_t*>(&p->stdout_pipe));
    try_close(reinterpret_cast<uv_handle_t*>(&p->stderr_pipe));
    if (p->pending_closes == 0) {
        // Nothing to close — free directly. (Shouldn't happen in practice
        // since uv_spawn just initialized everything.)
        if (p->alive) *p->alive = false;
        delete p;
    }
}

void on_proc_exit(uv_process_t* req, int64_t exit_status, int term_signal) {
    auto* p = static_cast<Proc*>(req->data);
    p->exited = true;
    if (p->on_exit) {
        invoke_callback(*p->interp, p->on_exit,
                        { Value{static_cast<double>(exit_status)},
                          Value{static_cast<double>(term_signal)} });
    }
    close_proc_handles(p);
}

void alloc_buf(uv_handle_t*, std::size_t suggested, uv_buf_t* buf) {
    buf->base = static_cast<char*>(std::malloc(suggested));
    buf->len  = static_cast<decltype(buf->len)>(suggested);
}

void on_pipe_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    auto* p = static_cast<Proc*>(stream->data);
    bool is_stdout = (stream == reinterpret_cast<uv_stream_t*>(&p->stdout_pipe));

    if (nread > 0) {
        std::string chunk(buf->base, static_cast<std::size_t>(nread));
        std::free(buf->base);
        const auto& cb = is_stdout ? p->on_stdout : p->on_stderr;
        if (cb) invoke_callback(*p->interp, cb, { Value{std::move(chunk)} });
        return;
    }
    if (buf->base) std::free(buf->base);
    if (nread < 0) {
        // EOF (or error). Stop reading; the process exit cb handles cleanup.
        uv_read_stop(stream);
    }
}

struct WriteReq {
    uv_write_t  req{};
    std::string buf;
};

void on_write_done(uv_write_t* req, int /*status*/) {
    delete static_cast<WriteReq*>(req->data);
}

ModulePtr build_stdin_module(Proc* p) {
    auto alive = p->alive;
    auto write_fn = [p, alive](Interpreter&, std::vector<Value> args) -> Value {
        if (!*alive) throw std::runtime_error("process.stdin.write: process is gone");
        if (!args[0].is_string())
            throw std::runtime_error("process.stdin.write: argument must be a string");
        auto* w = new WriteReq{};
        w->buf       = args[0].as_string();
        w->req.data  = w;
        uv_buf_t b   = uv_buf_init(w->buf.data(), static_cast<unsigned int>(w->buf.size()));
        int rc = uv_write(&w->req, reinterpret_cast<uv_stream_t*>(&p->stdin_pipe),
                          &b, 1, on_write_done);
        if (rc < 0) {
            delete w;
            throw std::runtime_error(fmt::format("process.stdin.write: {}", uv_strerror(rc)));
        }
        return Value{};
    };
    auto close_fn = [p, alive](Interpreter&, std::vector<Value>) -> Value {
        if (!*alive) return Value{};
        auto* h = reinterpret_cast<uv_handle_t*>(&p->stdin_pipe);
        if (!uv_is_closing(h)) {
            ++p->pending_closes;
            uv_close(h, on_handle_closed);
        }
        return Value{};
    };
    return NativeModule("process_stdin")
        .add_function("write", 1, write_fn)
        .add_function("close", 0, close_fn)
        .build();
}

ModulePtr build_pipe_reader_module(Proc* p, bool is_stdout) {
    auto alive = p->alive;
    auto on_data_fn = [p, alive, is_stdout](Interpreter&, std::vector<Value> args) -> Value {
        if (!*alive) throw std::runtime_error("process.on_data: process is gone");
        auto cb = to_callback(args[0], is_stdout ? "process.stdout.on_data"
                                                  : "process.stderr.on_data");
        if (is_stdout) p->on_stdout = std::move(cb);
        else           p->on_stderr = std::move(cb);
        return Value{};
    };
    return NativeModule(is_stdout ? "process_stdout" : "process_stderr")
        .add_function("on_data", 1, on_data_fn)
        .build();
}

ModulePtr build_process_module(Proc* p) {
    auto alive = p->alive;

    auto kill_fn = [p, alive](Interpreter&, std::vector<Value> args) -> Value {
        if (!*alive) return Value{};
        int sig = 15;     // SIGTERM
        if (!args.empty() && args[0].is_number()) sig = static_cast<int>(args[0].as_number());
        int rc = uv_process_kill(&p->process, sig);
        if (rc < 0)
            throw std::runtime_error(fmt::format("process.kill: {}", uv_strerror(rc)));
        return Value{};
    };

    return NativeModule("process")
        .add_value   ("pid",    Value{static_cast<double>(p->process.pid)})
        .add_value   ("stdin",  Value{build_stdin_module(p)})
        .add_value   ("stdout", Value{build_pipe_reader_module(p, true)})
        .add_value   ("stderr", Value{build_pipe_reader_module(p, false)})
        .add_function("kill",  -1, kill_fn)
        .build();
}

// ---------- sync run -------------------------------------------------------
//
// Runs a child to completion and returns {code, signal, stdout, stderr}.
// Uses a fresh uv_loop_t per call so the interpreter's main loop is
// untouched — important if exec.run is called from inside a server handler.

struct SyncProc {
    uv_process_t  process{};
    uv_pipe_t     stdin_pipe{};
    uv_pipe_t     stdout_pipe{};
    uv_pipe_t     stderr_pipe{};
    std::string   stdout_buf;
    std::string   stderr_buf;
    int64_t       exit_status = -1;
    int           term_signal = 0;
};

void sync_on_exit(uv_process_t* req, int64_t exit_status, int term_signal) {
    auto* sp = static_cast<SyncProc*>(req->data);
    sp->exit_status = exit_status;
    sp->term_signal = term_signal;
    uv_close(reinterpret_cast<uv_handle_t*>(req), nullptr);
}

void sync_on_pipe_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    auto* sp = static_cast<SyncProc*>(stream->data);
    bool is_stdout = (stream == reinterpret_cast<uv_stream_t*>(&sp->stdout_pipe));
    if (nread > 0) {
        (is_stdout ? sp->stdout_buf : sp->stderr_buf)
            .append(buf->base, static_cast<std::size_t>(nread));
        std::free(buf->base);
        return;
    }
    if (buf->base) std::free(buf->base);
    if (nread < 0) {
        uv_read_stop(stream);
        uv_close(reinterpret_cast<uv_handle_t*>(stream), nullptr);
    }
}

}  // namespace

void register_exec(Interpreter& interp) {
    auto m = NativeModule("exec")

        // exec.spawn(cmd, args, opts, on_exit) where on_exit(code, signal).
        // args is a list of strings (or null for none). opts is a map with
        // optional cwd (string) and env (map). on_exit may be null. Returns a
        // process module: .pid, .stdin{write,close}, .stdout.on_data,
        // .stderr.on_data, .kill(signal).
        .add_function("spawn", 4,
            [&interp](Interpreter&, std::vector<Value> args) -> Value {
                if (!args[0].is_string())
                    throw std::runtime_error("exec.spawn: cmd must be a string");
                const std::string cmd = args[0].as_string();

                Argv argv = build_argv(cmd, args[1]);

                std::string cwd_str;
                EnvList     env;
                if (!args[2].is_null()) {
                    if (!args[2].is_map())
                        throw std::runtime_error("exec.spawn: opts must be a map");
                    const auto& opts = args[2].as_map();
                    if (auto it = opts->find("cwd"); it != opts->end()) {
                        if (!it->second.is_string())
                            throw std::runtime_error("exec.spawn: opts.cwd must be a string");
                        cwd_str = it->second.as_string();
                    }
                    if (auto it = opts->find("env"); it != opts->end())
                        env = build_env(it->second);
                }
                CallablePtr on_exit_cb;
                if (!args[3].is_null()) on_exit_cb = to_callback(args[3], "exec.spawn");

                auto* p = new Proc{};
                p->interp  = &interp;
                p->on_exit = std::move(on_exit_cb);
                p->process.data     = p;
                p->stdin_pipe.data  = p;
                p->stdout_pipe.data = p;
                p->stderr_pipe.data = p;

                uv_pipe_init(interp.loop(), &p->stdin_pipe,  0);
                uv_pipe_init(interp.loop(), &p->stdout_pipe, 0);
                uv_pipe_init(interp.loop(), &p->stderr_pipe, 0);

                uv_stdio_container_t stdio[3];
                stdio[0].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_READABLE_PIPE);
                stdio[0].data.stream = reinterpret_cast<uv_stream_t*>(&p->stdin_pipe);
                stdio[1].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
                stdio[1].data.stream = reinterpret_cast<uv_stream_t*>(&p->stdout_pipe);
                stdio[2].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
                stdio[2].data.stream = reinterpret_cast<uv_stream_t*>(&p->stderr_pipe);

                uv_process_options_t opts{};
                opts.exit_cb     = on_proc_exit;
                opts.file        = cmd.c_str();
                opts.args        = argv.argv.data();
                opts.cwd         = cwd_str.empty() ? nullptr : cwd_str.c_str();
                opts.env         = env.envp.empty() ? nullptr : env.envp.data();
                opts.stdio_count = 3;
                opts.stdio       = stdio;

                int rc = uv_spawn(interp.loop(), &p->process, &opts);
                if (rc < 0) {
                    close_proc_handles(p);
                    throw std::runtime_error(fmt::format("exec.spawn '{}': {}",
                                                         cmd, uv_strerror(rc)));
                }

                uv_read_start(reinterpret_cast<uv_stream_t*>(&p->stdout_pipe),
                              alloc_buf, on_pipe_read);
                uv_read_start(reinterpret_cast<uv_stream_t*>(&p->stderr_pipe),
                              alloc_buf, on_pipe_read);

                return Value{build_process_module(p)};
            })

        // exec.run(cmd, args [, opts]) -> {code, signal, stdout, stderr}.
        // Synchronous: blocks until the child exits and pipes drain. Uses a
        // private uv_loop so it doesn't disrupt the main event loop.
        .add_function("run", -1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                if (args.size() < 2 || args.size() > 3)
                    throw std::runtime_error("exec.run: expects 2 or 3 arguments");
                if (!args[0].is_string())
                    throw std::runtime_error("exec.run: cmd must be a string");
                const std::string cmd = args[0].as_string();
                Argv argv = build_argv(cmd, args[1]);

                std::string cwd_str;
                EnvList     env;
                if (args.size() == 3 && !args[2].is_null()) {
                    if (!args[2].is_map())
                        throw std::runtime_error("exec.run: opts must be a map");
                    const auto& opts = args[2].as_map();
                    if (auto it = opts->find("cwd"); it != opts->end()) {
                        if (!it->second.is_string())
                            throw std::runtime_error("exec.run: opts.cwd must be a string");
                        cwd_str = it->second.as_string();
                    }
                    if (auto it = opts->find("env"); it != opts->end())
                        env = build_env(it->second);
                }

                uv_loop_t loop;
                uv_loop_init(&loop);

                SyncProc sp;
                sp.process.data     = &sp;
                sp.stdin_pipe.data  = &sp;
                sp.stdout_pipe.data = &sp;
                sp.stderr_pipe.data = &sp;

                uv_pipe_init(&loop, &sp.stdin_pipe,  0);
                uv_pipe_init(&loop, &sp.stdout_pipe, 0);
                uv_pipe_init(&loop, &sp.stderr_pipe, 0);

                uv_stdio_container_t stdio[3];
                stdio[0].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_READABLE_PIPE);
                stdio[0].data.stream = reinterpret_cast<uv_stream_t*>(&sp.stdin_pipe);
                stdio[1].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
                stdio[1].data.stream = reinterpret_cast<uv_stream_t*>(&sp.stdout_pipe);
                stdio[2].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
                stdio[2].data.stream = reinterpret_cast<uv_stream_t*>(&sp.stderr_pipe);

                uv_process_options_t opts{};
                opts.exit_cb     = sync_on_exit;
                opts.file        = cmd.c_str();
                opts.args        = argv.argv.data();
                opts.cwd         = cwd_str.empty() ? nullptr : cwd_str.c_str();
                opts.env         = env.envp.empty() ? nullptr : env.envp.data();
                opts.stdio_count = 3;
                opts.stdio       = stdio;

                int rc = uv_spawn(&loop, &sp.process, &opts);
                if (rc < 0) {
                    // Close handles and tear down the loop before throwing.
                    auto close_handle = [](uv_handle_t* h) {
                        if (!uv_is_closing(h)) uv_close(h, nullptr);
                    };
                    close_handle(reinterpret_cast<uv_handle_t*>(&sp.process));
                    close_handle(reinterpret_cast<uv_handle_t*>(&sp.stdin_pipe));
                    close_handle(reinterpret_cast<uv_handle_t*>(&sp.stdout_pipe));
                    close_handle(reinterpret_cast<uv_handle_t*>(&sp.stderr_pipe));
                    uv_run(&loop, UV_RUN_DEFAULT);
                    uv_loop_close(&loop);
                    throw std::runtime_error(fmt::format("exec.run '{}': {}",
                                                         cmd, uv_strerror(rc)));
                }

                // Close stdin so the child sees EOF immediately. We don't
                // need to write to it from sync mode.
                uv_close(reinterpret_cast<uv_handle_t*>(&sp.stdin_pipe), nullptr);

                uv_read_start(reinterpret_cast<uv_stream_t*>(&sp.stdout_pipe),
                              alloc_buf, sync_on_pipe_read);
                uv_read_start(reinterpret_cast<uv_stream_t*>(&sp.stderr_pipe),
                              alloc_buf, sync_on_pipe_read);

                uv_run(&loop, UV_RUN_DEFAULT);
                uv_loop_close(&loop);

                auto out = std::make_shared<std::unordered_map<std::string, Value>>();
                (*out)["code"]   = Value{static_cast<double>(sp.exit_status)};
                (*out)["signal"] = Value{static_cast<double>(sp.term_signal)};
                (*out)["stdout"] = Value{std::move(sp.stdout_buf)};
                (*out)["stderr"] = Value{std::move(sp.stderr_buf)};
                return Value{out};
            })
        .build();

    interp.register_native_module("exec", m);
}

}  // namespace bnl
