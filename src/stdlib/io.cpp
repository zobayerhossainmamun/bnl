#include "stdlib/registry.h"

#include <fmt/core.h>
#include <uv.h>

#include <sys/stat.h>      // S_IFMT, S_IFREG, S_IFDIR

#include <cstdint>
#include <cstdlib>
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

// ---------- helpers --------------------------------------------------------

const std::string& to_path_string(const Value& v) {
    if (!v.is_string())
        throw std::runtime_error(fmt::format("io: path must be a string, got {}", v.type_name()));
    return v.as_string();
}

CallablePtr to_callback(const Value& v, const char* where) {
    if (!v.is_callable())
        throw std::runtime_error(fmt::format("{}: callback must be a function, got {}",
                                             where, v.type_name()));
    return v.as_callable();
}

[[noreturn]] void throw_uv(int rc, const char* op, const std::string& path) {
    throw std::runtime_error(fmt::format("io.{}('{}'): {}", op, path, uv_strerror(rc)));
}

// RAII close so partial reads/writes don't leak fds when an exception fires.
struct UvFile {
    uv_loop_t* loop;
    uv_file    fd;
    UvFile(uv_loop_t* l, uv_file f) : loop(l), fd(f) {}
    ~UvFile() {
        if (fd >= 0) {
            uv_fs_t req;
            uv_fs_close(loop, &req, fd, nullptr);
            uv_fs_req_cleanup(&req);
        }
    }
    UvFile(const UvFile&) = delete;
    UvFile& operator=(const UvFile&) = delete;
};

void invoke_callback(Interpreter& interp, CallablePtr cb, std::vector<Value> args) {
    try {
        cb->call(interp, std::move(args));
    } catch (const RuntimeError& e) {
        fmt::print(stderr, "uncaught error in async io callback at {}:{} (near '{}'): {}\n",
                   e.token.line, e.token.column, e.token.lexeme, e.what());
        interp.mark_loop_failed();
    } catch (const std::exception& e) {
        fmt::print(stderr, "uncaught error in async io callback: {}\n", e.what());
        interp.mark_loop_failed();
    }
}

// ---------- sync ops over uv_fs_* ------------------------------------------

std::string sync_read_file(uv_loop_t* loop, const std::string& path) {
    uv_fs_t open_req;
    int fd = uv_fs_open(loop, &open_req, path.c_str(), UV_FS_O_RDONLY, 0, nullptr);
    uv_fs_req_cleanup(&open_req);
    if (fd < 0) throw_uv(fd, "read_file", path);
    UvFile keep(loop, fd);

    uv_fs_t stat_req;
    int rc = uv_fs_fstat(loop, &stat_req, fd, nullptr);
    auto size = static_cast<std::size_t>(stat_req.statbuf.st_size);
    uv_fs_req_cleanup(&stat_req);
    if (rc < 0) throw_uv(rc, "read_file", path);

    std::string out(size, '\0');
    std::size_t total = 0;
    while (total < size) {
        uv_buf_t b = uv_buf_init(out.data() + total,
                                 static_cast<unsigned int>(size - total));
        uv_fs_t read_req;
        int n = uv_fs_read(loop, &read_req, fd, &b, 1,
                           static_cast<int64_t>(total), nullptr);
        uv_fs_req_cleanup(&read_req);
        if (n < 0) throw_uv(n, "read_file", path);
        if (n == 0) break;       // truncated mid-read — return what we got
        total += static_cast<std::size_t>(n);
    }
    out.resize(total);
    return out;
}

void sync_write_file(uv_loop_t* loop, const std::string& path,
                     const std::string& content, bool append) {
    int flags = UV_FS_O_WRONLY | UV_FS_O_CREAT;
    flags    |= append ? UV_FS_O_APPEND : UV_FS_O_TRUNC;

    uv_fs_t open_req;
    int fd = uv_fs_open(loop, &open_req, path.c_str(), flags, 0644, nullptr);
    uv_fs_req_cleanup(&open_req);
    const char* op = append ? "append_file" : "write_file";
    if (fd < 0) throw_uv(fd, op, path);
    UvFile keep(loop, fd);

    std::size_t written = 0;
    while (written < content.size()) {
        uv_buf_t b = uv_buf_init(const_cast<char*>(content.data() + written),
                                 static_cast<unsigned int>(content.size() - written));
        uv_fs_t write_req;
        int n = uv_fs_write(loop, &write_req, fd, &b, 1, -1, nullptr);
        uv_fs_req_cleanup(&write_req);
        if (n < 0) throw_uv(n, op, path);
        if (n == 0) break;
        written += static_cast<std::size_t>(n);
    }
}

// Recursive mkdir. uv_fs_mkdir only does one level; if parent is missing
// (UV_ENOENT) we recurse on the parent and retry.
void sync_mkdir_p(uv_loop_t* loop, std::string path) {
    if (path.empty()) return;
    while (path.size() > 1 && (path.back() == '/' || path.back() == '\\'))
        path.pop_back();

    uv_fs_t req;
    int rc = uv_fs_mkdir(loop, &req, path.c_str(), 0755, nullptr);
    uv_fs_req_cleanup(&req);
    if (rc == 0 || rc == UV_EEXIST) return;
    if (rc != UV_ENOENT) throw_uv(rc, "mkdir", path);

    auto pos = path.find_last_of("/\\");
    if (pos == std::string::npos || pos == 0) throw_uv(rc, "mkdir", path);
    sync_mkdir_p(loop, path.substr(0, pos));

    uv_fs_t retry;
    int rc2 = uv_fs_mkdir(loop, &retry, path.c_str(), 0755, nullptr);
    uv_fs_req_cleanup(&retry);
    if (rc2 != 0 && rc2 != UV_EEXIST) throw_uv(rc2, "mkdir", path);
}

// Recursive remove. uv has no rm -rf, so we drive it with stat + scandir.
void sync_remove(uv_loop_t* loop, const std::string& path) {
    uv_fs_t stat_req;
    int rc = uv_fs_lstat(loop, &stat_req, path.c_str(), nullptr);
    if (rc == UV_ENOENT) {
        uv_fs_req_cleanup(&stat_req);
        return;       // idempotent: removing what isn't there is a no-op
    }
    bool is_dir = (rc == 0) && (stat_req.statbuf.st_mode & S_IFMT) == S_IFDIR;
    uv_fs_req_cleanup(&stat_req);
    if (rc < 0) throw_uv(rc, "remove", path);

    if (!is_dir) {
        uv_fs_t unlink_req;
        int urc = uv_fs_unlink(loop, &unlink_req, path.c_str(), nullptr);
        uv_fs_req_cleanup(&unlink_req);
        if (urc < 0) throw_uv(urc, "remove", path);
        return;
    }

    // Directory — recurse into children first.
    uv_fs_t scan_req;
    int srn = uv_fs_scandir(loop, &scan_req, path.c_str(), 0, nullptr);
    if (srn < 0) {
        uv_fs_req_cleanup(&scan_req);
        throw_uv(srn, "remove", path);
    }
    uv_dirent_t ent;
    while (uv_fs_scandir_next(&scan_req, &ent) != UV_EOF) {
        std::string child = path;
        if (!child.empty() && child.back() != '/' && child.back() != '\\')
            child.push_back('/');
        child += ent.name;
        sync_remove(loop, child);
    }
    uv_fs_req_cleanup(&scan_req);

    uv_fs_t rmdir_req;
    int rrc = uv_fs_rmdir(loop, &rmdir_req, path.c_str(), nullptr);
    uv_fs_req_cleanup(&rmdir_req);
    if (rrc < 0) throw_uv(rrc, "remove", path);
}

// ---------- async wrappers via threadpool ----------------------------------
//
// uv_fs_* sync calls (callback=NULL) block the calling thread but are safe to
// invoke off the loop thread. Wrapping them in uv_queue_work moves the block
// to a worker, then dispatches the result back via the after_work callback.

struct AsyncRead {
    std::string  path;
    std::string  result;
    std::string  error;
    CallablePtr  callback;
    Interpreter* interp;
};

void on_read_work(uv_work_t* req) {
    auto* w = static_cast<AsyncRead*>(req->data);
    try {
        w->result = sync_read_file(w->interp->loop(), w->path);
    } catch (const std::exception& e) {
        w->error = e.what();
    }
}

void on_read_done(uv_work_t* req, int /*status*/) {
    std::unique_ptr<AsyncRead> w (static_cast<AsyncRead*>(req->data));
    std::unique_ptr<uv_work_t> rq(req);
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

struct AsyncWrite {
    std::string  path;
    std::string  content;
    bool         append = false;
    std::string  error;
    CallablePtr  callback;
    Interpreter* interp;
};

// ---------- pull-based file read stream ------------------------------------
//
// io.open_read(path) returns a module with read(size, cb) and close().
// Each read submits one uv_fs_read on the threadpool. Caller drives the
// pacing — exactly what request.upload needs to align reads with conn.write
// callbacks for backpressure.

struct ReadStream {
    uv_loop_t*            loop = nullptr;
    Interpreter*          interp = nullptr;
    uv_file               fd = -1;
    int64_t               offset = 0;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};

struct AsyncReadChunk {
    ReadStream*  stream = nullptr;
    std::size_t  request_size = 0;
    std::string  buf;          // resized to actual bytes read
    std::string  error;
    bool         eof = false;
    CallablePtr  callback;
    Interpreter* interp = nullptr;
    std::shared_ptr<bool> stream_alive;
};

void on_read_chunk_work(uv_work_t* req) {
    auto* w = static_cast<AsyncReadChunk*>(req->data);
    if (!*w->stream_alive) { w->error = "stream is closed"; return; }
    w->buf.resize(w->request_size);
    uv_buf_t b = uv_buf_init(w->buf.data(),
                             static_cast<unsigned int>(w->request_size));
    uv_fs_t  read_req;
    int n = uv_fs_read(w->stream->loop, &read_req, w->stream->fd,
                       &b, 1, w->stream->offset, nullptr);
    uv_fs_req_cleanup(&read_req);
    if (n < 0) {
        w->error = uv_strerror(n);
        return;
    }
    if (n == 0) {
        w->eof = true;
        w->buf.clear();
        return;
    }
    w->buf.resize(static_cast<std::size_t>(n));
    w->stream->offset += n;
}

void on_read_chunk_done(uv_work_t* req, int /*status*/) {
    std::unique_ptr<AsyncReadChunk> w (static_cast<AsyncReadChunk*>(req->data));
    std::unique_ptr<uv_work_t>      rq(req);
    std::vector<Value> args;
    if (!w->error.empty()) {
        args.push_back(Value{w->error});
        args.push_back(Value{});
    } else if (w->eof) {
        args.push_back(Value{});      // err
        args.push_back(Value{});      // chunk null = EOF
    } else {
        args.push_back(Value{});
        args.push_back(Value{std::move(w->buf)});
    }
    invoke_callback(*w->interp, w->callback, std::move(args));
}

// ---------- pull-based file write stream (mirror of ReadStream) -----------

struct WriteStream {
    uv_loop_t*            loop = nullptr;
    Interpreter*          interp = nullptr;
    uv_file               fd = -1;
    int64_t               offset = 0;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};

struct AsyncWriteChunk {
    WriteStream* stream = nullptr;
    std::string  data;
    std::string  error;
    CallablePtr  callback;
    Interpreter* interp = nullptr;
    std::shared_ptr<bool> stream_alive;
};

void on_write_chunk_work(uv_work_t* req) {
    auto* w = static_cast<AsyncWriteChunk*>(req->data);
    if (!*w->stream_alive) { w->error = "stream is closed"; return; }
    std::size_t written = 0;
    while (written < w->data.size()) {
        uv_buf_t b = uv_buf_init(const_cast<char*>(w->data.data() + written),
                                 static_cast<unsigned int>(w->data.size() - written));
        uv_fs_t  write_req;
        int n = uv_fs_write(w->stream->loop, &write_req, w->stream->fd,
                            &b, 1, w->stream->offset, nullptr);
        uv_fs_req_cleanup(&write_req);
        if (n < 0) { w->error = uv_strerror(n); return; }
        if (n == 0) break;
        written += static_cast<std::size_t>(n);
        w->stream->offset += n;
    }
}

void on_write_chunk_done(uv_work_t* req, int /*status*/) {
    std::unique_ptr<AsyncWriteChunk> w (static_cast<AsyncWriteChunk*>(req->data));
    std::unique_ptr<uv_work_t>       rq(req);
    std::vector<Value> args;
    args.push_back(w->error.empty() ? Value{} : Value{w->error});
    invoke_callback(*w->interp, w->callback, std::move(args));
}

ModulePtr build_write_stream_module(WriteStream* s) {
    auto alive = s->alive;
    auto loop  = s->loop;

    auto write_fn = [s, alive](Interpreter& interp, std::vector<Value> args) -> Value {
        if (!*alive) throw std::runtime_error("write_stream.write: stream is closed");
        if (args.size() != 2)
            throw std::runtime_error("write_stream.write(data, cb): expects 2 arguments");
        if (!args[0].is_string())
            throw std::runtime_error("write_stream.write: data must be a string");
        auto cb = to_callback(args[1], "write_stream.write");

        auto* w = new AsyncWriteChunk{};
        w->stream       = s;
        w->data         = args[0].as_string();
        w->callback     = std::move(cb);
        w->interp       = &interp;
        w->stream_alive = s->alive;

        auto* req = new uv_work_t{};
        req->data = w;
        uv_queue_work(s->loop, req, on_write_chunk_work, on_write_chunk_done);
        return Value{};
    };

    auto close_fn = [s, alive, loop](Interpreter&, std::vector<Value>) -> Value {
        if (!*alive) return Value{};
        *alive = false;
        if (s->fd >= 0) {
            uv_fs_t req;
            uv_fs_close(loop, &req, s->fd, nullptr);
            uv_fs_req_cleanup(&req);
            s->fd = -1;
        }
        delete s;
        return Value{};
    };

    return NativeModule("write_stream")
        .add_function("write", 2, write_fn)
        .add_function("close", 0, close_fn)
        .build();
}

ModulePtr build_read_stream_module(ReadStream* s) {
    auto alive = s->alive;
    auto loop  = s->loop;

    auto read_fn = [s, alive](Interpreter& interp, std::vector<Value> args) -> Value {
        if (!*alive) throw std::runtime_error("read_stream.read: stream is closed");
        if (args.size() != 2)
            throw std::runtime_error("read_stream.read(size, cb): expects 2 arguments");
        if (!args[0].is_number())
            throw std::runtime_error("read_stream.read: size must be a number");
        auto cb = to_callback(args[1], "read_stream.read");

        auto* w = new AsyncReadChunk{};
        w->stream       = s;
        w->request_size = static_cast<std::size_t>(args[0].as_number());
        w->callback     = std::move(cb);
        w->interp       = &interp;
        w->stream_alive = s->alive;

        auto* req = new uv_work_t{};
        req->data = w;
        uv_queue_work(s->loop, req, on_read_chunk_work, on_read_chunk_done);
        return Value{};
    };

    auto close_fn = [s, alive, loop](Interpreter&, std::vector<Value>) -> Value {
        if (!*alive) return Value{};
        *alive = false;
        if (s->fd >= 0) {
            uv_fs_t req;
            uv_fs_close(loop, &req, s->fd, nullptr);
            uv_fs_req_cleanup(&req);
            s->fd = -1;
        }
        delete s;
        return Value{};
    };

    return NativeModule("read_stream")
        .add_function("read",  2, read_fn)
        .add_function("close", 0, close_fn)
        .build();
}

void on_write_work(uv_work_t* req) {
    auto* w = static_cast<AsyncWrite*>(req->data);
    try {
        sync_write_file(w->interp->loop(), w->path, w->content, w->append);
    } catch (const std::exception& e) {
        w->error = e.what();
    }
}

void on_write_done(uv_work_t* req, int /*status*/) {
    std::unique_ptr<AsyncWrite> w (static_cast<AsyncWrite*>(req->data));
    std::unique_ptr<uv_work_t>  rq(req);
    std::vector<Value> args;
    args.push_back(w->error.empty() ? Value{} : Value{w->error});
    invoke_callback(*w->interp, w->callback, std::move(args));
}

}  // namespace

void register_io(Interpreter& interp) {
    auto m = NativeModule("io")
        // ---------- read / write -------------------------------------------

        // io.read_file(path) -> string (entire contents as bytes)
        .add_function("read_file", 1,
            [&interp](Interpreter&, std::vector<Value> args) -> Value {
                return Value{sync_read_file(interp.loop(), to_path_string(args[0]))};
            })

        // io.write_file(path, content) -> null  (overwrites)
        .add_function("write_file", 2,
            [&interp](Interpreter&, std::vector<Value> args) -> Value {
                if (!args[1].is_string())
                    throw std::runtime_error("io.write_file: content must be a string");
                sync_write_file(interp.loop(), to_path_string(args[0]),
                                args[1].as_string(), false);
                return Value{};
            })

        // io.append_file(path, content) -> null  (creates if missing)
        .add_function("append_file", 2,
            [&interp](Interpreter&, std::vector<Value> args) -> Value {
                if (!args[1].is_string())
                    throw std::runtime_error("io.append_file: content must be a string");
                sync_write_file(interp.loop(), to_path_string(args[0]),
                                args[1].as_string(), true);
                return Value{};
            })

        // ---------- existence / type ---------------------------------------

        .add_function("exists", 1,
            [&interp](Interpreter&, std::vector<Value> args) -> Value {
                uv_fs_t req;
                int rc = uv_fs_stat(interp.loop(), &req,
                                    to_path_string(args[0]).c_str(), nullptr);
                uv_fs_req_cleanup(&req);
                return Value{rc == 0};
            })

        .add_function("is_file", 1,
            [&interp](Interpreter&, std::vector<Value> args) -> Value {
                uv_fs_t req;
                int rc = uv_fs_stat(interp.loop(), &req,
                                    to_path_string(args[0]).c_str(), nullptr);
                bool ok = (rc == 0) && ((req.statbuf.st_mode & S_IFMT) == S_IFREG);
                uv_fs_req_cleanup(&req);
                return Value{ok};
            })

        .add_function("is_dir", 1,
            [&interp](Interpreter&, std::vector<Value> args) -> Value {
                uv_fs_t req;
                int rc = uv_fs_stat(interp.loop(), &req,
                                    to_path_string(args[0]).c_str(), nullptr);
                bool ok = (rc == 0) && ((req.statbuf.st_mode & S_IFMT) == S_IFDIR);
                uv_fs_req_cleanup(&req);
                return Value{ok};
            })

        // io.stat(path) -> {bytes, mtime, is_dir, is_file}. mtime is epoch
        // seconds. The byte count is named "bytes" rather than "size" because
        // the map's intrinsic .size accessor returns the entry count and would
        // shadow a "size" key here.
        .add_function("stat", 1,
            [&interp](Interpreter&, std::vector<Value> args) -> Value {
                const std::string path = to_path_string(args[0]);
                uv_fs_t req;
                int rc = uv_fs_stat(interp.loop(), &req, path.c_str(), nullptr);
                if (rc < 0) {
                    uv_fs_req_cleanup(&req);
                    throw_uv(rc, "stat", path);
                }
                auto out = std::make_shared<std::unordered_map<std::string, Value>>();
                (*out)["bytes"]   = Value{static_cast<double>(req.statbuf.st_size)};
                (*out)["mtime"]   = Value{static_cast<double>(req.statbuf.st_mtim.tv_sec)};
                (*out)["is_dir"]  = Value{(req.statbuf.st_mode & S_IFMT) == S_IFDIR};
                (*out)["is_file"] = Value{(req.statbuf.st_mode & S_IFMT) == S_IFREG};
                uv_fs_req_cleanup(&req);
                return Value{out};
            })

        // ---------- directories --------------------------------------------

        // io.mkdir(path) -> null  (creates intermediate dirs as needed)
        .add_function("mkdir", 1,
            [&interp](Interpreter&, std::vector<Value> args) -> Value {
                sync_mkdir_p(interp.loop(), to_path_string(args[0]));
                return Value{};
            })

        // io.list_dir(path) -> [string, ...]  (entry names, no '.' / '..')
        .add_function("list_dir", 1,
            [&interp](Interpreter&, std::vector<Value> args) -> Value {
                const std::string path = to_path_string(args[0]);
                uv_fs_t req;
                int rc = uv_fs_scandir(interp.loop(), &req, path.c_str(), 0, nullptr);
                if (rc < 0) {
                    uv_fs_req_cleanup(&req);
                    throw_uv(rc, "list_dir", path);
                }
                auto out = std::make_shared<std::vector<Value>>();
                uv_dirent_t ent;
                while (uv_fs_scandir_next(&req, &ent) != UV_EOF) {
                    out->emplace_back(std::string(ent.name));
                }
                uv_fs_req_cleanup(&req);
                return Value{out};
            })

        // io.remove(path) -> null  (file or directory tree; idempotent)
        .add_function("remove", 1,
            [&interp](Interpreter&, std::vector<Value> args) -> Value {
                sync_remove(interp.loop(), to_path_string(args[0]));
                return Value{};
            })

        // ---------- rename / copy ------------------------------------------

        .add_function("rename", 2,
            [&interp](Interpreter&, std::vector<Value> args) -> Value {
                const std::string from = to_path_string(args[0]);
                const std::string to   = to_path_string(args[1]);
                uv_fs_t req;
                int rc = uv_fs_rename(interp.loop(), &req,
                                      from.c_str(), to.c_str(), nullptr);
                uv_fs_req_cleanup(&req);
                if (rc < 0)
                    throw std::runtime_error(fmt::format(
                        "io.rename('{}' -> '{}'): {}", from, to, uv_strerror(rc)));
                return Value{};
            })

        .add_function("copy_file", 2,
            [&interp](Interpreter&, std::vector<Value> args) -> Value {
                const std::string from = to_path_string(args[0]);
                const std::string to   = to_path_string(args[1]);
                uv_fs_t req;
                int rc = uv_fs_copyfile(interp.loop(), &req,
                                        from.c_str(), to.c_str(), 0, nullptr);
                uv_fs_req_cleanup(&req);
                if (rc < 0)
                    throw std::runtime_error(fmt::format(
                        "io.copy_file('{}' -> '{}'): {}", from, to, uv_strerror(rc)));
                return Value{};
            })

        // ---------- async --------------------------------------------------

        // io.read_file_async(path, fn) -> null. fn(err, data).
        .add_function("read_file_async", 2,
            [&interp](Interpreter&, std::vector<Value> args) -> Value {
                auto cb = to_callback(args[1], "io.read_file_async");
                auto* req = new uv_work_t{};
                req->data = new AsyncRead{
                    to_path_string(args[0]), {}, {}, std::move(cb), &interp};
                uv_queue_work(interp.loop(), req, on_read_work, on_read_done);
                return Value{};
            })

        // io.write_file_async(path, content, fn) -> null. fn(err).
        .add_function("write_file_async", 3,
            [&interp](Interpreter&, std::vector<Value> args) -> Value {
                if (!args[1].is_string())
                    throw std::runtime_error("io.write_file_async: content must be a string");
                auto cb = to_callback(args[2], "io.write_file_async");
                auto* req = new uv_work_t{};
                req->data = new AsyncWrite{
                    to_path_string(args[0]), args[1].as_string(), false, {},
                    std::move(cb), &interp};
                uv_queue_work(interp.loop(), req, on_write_work, on_write_done);
                return Value{};
            })

        // io.open_read(path) -> stream module {read(size, cb), close()}.
        // Pull-based: each .read(n, cb) submits one uv_fs_read on the
        // threadpool. Designed for streaming uploads — pair each .read with
        // a conn.write(chunk, on_done) to get natural backpressure.
        .add_function("open_read", 1,
            [&interp](Interpreter&, std::vector<Value> args) -> Value {
                const std::string path = to_path_string(args[0]);
                uv_fs_t open_req;
                int fd = uv_fs_open(interp.loop(), &open_req, path.c_str(),
                                    UV_FS_O_RDONLY, 0, nullptr);
                uv_fs_req_cleanup(&open_req);
                if (fd < 0) throw_uv(fd, "open_read", path);

                auto* s = new ReadStream{};
                s->loop   = interp.loop();
                s->interp = &interp;
                s->fd     = fd;
                return Value{build_read_stream_module(s)};
            })

        // io.open_write(path) -> stream module {write(data, cb), close()}.
        // Truncates if the file exists. Each .write(data, cb) appends the
        // data and fires cb(err) when bytes are durable in the kernel page
        // cache. Pair with read-stream-based downloads for streaming receive.
        .add_function("open_write", 1,
            [&interp](Interpreter&, std::vector<Value> args) -> Value {
                const std::string path = to_path_string(args[0]);
                uv_fs_t open_req;
                int fd = uv_fs_open(interp.loop(), &open_req, path.c_str(),
                                    UV_FS_O_WRONLY | UV_FS_O_CREAT | UV_FS_O_TRUNC,
                                    0644, nullptr);
                uv_fs_req_cleanup(&open_req);
                if (fd < 0) throw_uv(fd, "open_write", path);

                auto* s = new WriteStream{};
                s->loop   = interp.loop();
                s->interp = &interp;
                s->fd     = fd;
                return Value{build_write_stream_module(s)};
            })

        .build();

    interp.register_native_module("io", m);
}

}  // namespace bnl
