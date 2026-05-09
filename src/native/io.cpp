#include "native/builtins.h"

#include <fmt/core.h>
#include <uv.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "interpreter.h"
#include "native_module.h"

namespace bnl {

namespace {

std::filesystem::path to_path(const Value& v) {
    if (!v.is_string())
        throw std::runtime_error(fmt::format("io: path must be a string, got {}", v.type_name()));
    return std::filesystem::path(v.as_string());
}

CallablePtr to_callback(const Value& v, const char* where) {
    if (!v.is_callable())
        throw std::runtime_error(fmt::format("{}: callback must be a function, got {}",
                                             where, v.type_name()));
    return v.as_callable();
}

// ---------- async file I/O via uv_queue_work --------------------------------
//
// Each async op carries a heap-allocated work struct. The worker callback runs
// on libuv's thread pool and does the blocking syscall; the after_work
// callback runs on the main loop thread and invokes the bnl callback with
// Node-style (err, data) arguments.

struct AsyncRead {
    std::string  path;
    std::string  result;
    std::string  error;     // empty == ok
    CallablePtr  callback;
    Interpreter* interp;
};

struct AsyncWrite {
    std::string  path;
    std::string  content;
    std::string  error;
    CallablePtr  callback;
    Interpreter* interp;
};

void on_read_work(uv_work_t* req) {
    auto* w = static_cast<AsyncRead*>(req->data);
    try {
        std::ifstream in(std::filesystem::path(w->path), std::ios::binary);
        if (!in) { w->error = "cannot open " + w->path; return; }
        std::ostringstream buf; buf << in.rdbuf();
        w->result = buf.str();
    } catch (const std::exception& e) {
        w->error = e.what();
    }
}

void invoke_callback(Interpreter& interp, CallablePtr cb, std::vector<Value> args) {
    try {
        cb->call(interp, std::move(args));
    } catch (const RuntimeError& e) {
        fmt::print(stderr, "uncaught error in async callback at {}:{} (near '{}'): {}\n",
                   e.token.line, e.token.column, e.token.lexeme, e.what());
        interp.mark_loop_failed();
    } catch (const std::exception& e) {
        fmt::print(stderr, "uncaught error in async callback: {}\n", e.what());
        interp.mark_loop_failed();
    }
}

void on_read_done(uv_work_t* req, int /*status*/) {
    std::unique_ptr<AsyncRead>  w  (static_cast<AsyncRead*>(req->data));
    std::unique_ptr<uv_work_t>  rq (req);
    std::vector<Value> args;
    if (!w->error.empty()) {
        args.push_back(Value{w->error});
        args.push_back(Value{});
    } else {
        args.push_back(Value{});
        args.push_back(Value{std::move(w->result)});
    }
    invoke_callback(*w->interp, w->callback, std::move(args));
}

void on_write_work(uv_work_t* req) {
    auto* w = static_cast<AsyncWrite*>(req->data);
    try {
        std::ofstream out(std::filesystem::path(w->path), std::ios::binary | std::ios::trunc);
        if (!out) { w->error = "cannot open " + w->path; return; }
        out << w->content;
        if (!out) w->error = "write failed for " + w->path;
    } catch (const std::exception& e) {
        w->error = e.what();
    }
}

void on_write_done(uv_work_t* req, int /*status*/) {
    std::unique_ptr<AsyncWrite> w  (static_cast<AsyncWrite*>(req->data));
    std::unique_ptr<uv_work_t>  rq (req);
    std::vector<Value> args;
    args.push_back(w->error.empty() ? Value{} : Value{w->error});
    invoke_callback(*w->interp, w->callback, std::move(args));
}

}  // namespace

void register_io(Interpreter& interp) {
    auto m = NativeModule("io")
        // io.read_file(path) -> string (entire contents as bytes)
        .add_function("read_file", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                auto p = to_path(args[0]);
                std::ifstream in(p, std::ios::binary);
                if (!in)
                    throw std::runtime_error(fmt::format("io.read_file: cannot open '{}'", p.string()));
                std::ostringstream buf; buf << in.rdbuf();
                return Value{buf.str()};
            })

        // io.write_file(path, content) -> null  (overwrites)
        .add_function("write_file", 2,
            [](Interpreter&, std::vector<Value> args) -> Value {
                auto p = to_path(args[0]);
                if (!args[1].is_string())
                    throw std::runtime_error("io.write_file: content must be a string");
                std::ofstream out(p, std::ios::binary | std::ios::trunc);
                if (!out)
                    throw std::runtime_error(fmt::format("io.write_file: cannot open '{}'", p.string()));
                out << args[1].as_string();
                return Value{};
            })

        // io.exists(path) -> bool
        .add_function("exists", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                std::error_code ec;
                return Value{std::filesystem::exists(to_path(args[0]), ec)};
            })

        // io.is_file(path) -> bool
        .add_function("is_file", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                std::error_code ec;
                return Value{std::filesystem::is_regular_file(to_path(args[0]), ec)};
            })

        // io.is_dir(path) -> bool
        .add_function("is_dir", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                std::error_code ec;
                return Value{std::filesystem::is_directory(to_path(args[0]), ec)};
            })

        // io.mkdir(path) -> null  (creates intermediate dirs as needed)
        .add_function("mkdir", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                auto p = to_path(args[0]);
                std::error_code ec;
                std::filesystem::create_directories(p, ec);
                if (ec)
                    throw std::runtime_error(fmt::format("io.mkdir('{}'): {}", p.string(), ec.message()));
                return Value{};
            })

        // io.remove(path) -> null  (removes file or directory tree)
        .add_function("remove", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                auto p = to_path(args[0]);
                std::error_code ec;
                std::filesystem::remove_all(p, ec);
                if (ec)
                    throw std::runtime_error(fmt::format("io.remove('{}'): {}", p.string(), ec.message()));
                return Value{};
            })

        // io.read_file_async(path, fn) -> null. fn(err, data).
        .add_function("read_file_async", 2,
            [&interp](Interpreter&, std::vector<Value> args) -> Value {
                auto path = to_path(args[0]).string();
                auto cb   = to_callback(args[1], "io.read_file_async");
                auto* req = new uv_work_t{};
                req->data = new AsyncRead{std::move(path), {}, {}, std::move(cb), &interp};
                uv_queue_work(interp.loop(), req, on_read_work, on_read_done);
                return Value{};
            })

        // io.write_file_async(path, content, fn) -> null. fn(err).
        .add_function("write_file_async", 3,
            [&interp](Interpreter&, std::vector<Value> args) -> Value {
                auto path = to_path(args[0]).string();
                if (!args[1].is_string())
                    throw std::runtime_error("io.write_file_async: content must be a string");
                auto cb   = to_callback(args[2], "io.write_file_async");
                auto* req = new uv_work_t{};
                req->data = new AsyncWrite{
                    std::move(path), args[1].as_string(), {}, std::move(cb), &interp};
                uv_queue_work(interp.loop(), req, on_write_work, on_write_done);
                return Value{};
            })

        .build();

    interp.register_native_module("io", m);
}

}  // namespace bnl
