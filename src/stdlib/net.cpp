#include "stdlib/registry.h"

#include <fmt/core.h>
#include <uv.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "bnl/interpreter.h"
#include "bnl/native_module.h"
#include "runtime/environment.h"

namespace bnl {

namespace {

// ---------- helpers ---------------------------------------------------------

void invoke_callback(Interpreter& interp, const CallablePtr& cb, std::vector<Value> args) {
    if (!cb) return;
    try {
        cb->call(interp, std::move(args));
    } catch (const RuntimeError& e) {
        fmt::print(stderr, "uncaught error in net callback at {}:{} (near '{}'): {}\n",
                   e.token.line, e.token.column, e.token.lexeme, e.what());
        interp.mark_loop_failed();
    } catch (const std::exception& e) {
        fmt::print(stderr, "uncaught error in net callback: {}\n", e.what());
        interp.mark_loop_failed();
    }
}

CallablePtr to_callback(const Value& v, const char* where) {
    if (!v.is_callable())
        throw std::runtime_error(fmt::format("{}: callback must be a function, got {}",
                                             where, v.type_name()));
    return v.as_callable();
}

// ---------- connection state ------------------------------------------------

struct Connection {
    uv_tcp_t              handle{};
    Interpreter*          interp = nullptr;
    CallablePtr           on_data;
    CallablePtr           on_end;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    bool                  closing = false;
};

void on_close_conn(uv_handle_t* h) {
    auto* c = static_cast<Connection*>(h->data);
    if (c->alive) *c->alive = false;
    delete c;
}

void close_conn_once(Connection* c) {
    if (c->closing) return;
    c->closing = true;
    auto* h = reinterpret_cast<uv_handle_t*>(&c->handle);
    if (!uv_is_closing(h)) uv_close(h, on_close_conn);
}

void alloc_buffer(uv_handle_t*, std::size_t suggested, uv_buf_t* buf) {
    buf->base = static_cast<char*>(std::malloc(suggested));
    buf->len  = static_cast<decltype(buf->len)>(suggested);
}

void on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    auto* c = static_cast<Connection*>(stream->data);

    if (nread > 0) {
        std::string chunk(buf->base, static_cast<std::size_t>(nread));
        std::free(buf->base);
        if (c->on_data) {
            invoke_callback(*c->interp, c->on_data, { Value{std::move(chunk)} });
        }
        return;
    }

    if (buf->base) std::free(buf->base);

    if (nread < 0) {
        // EOF or error — invoke on_end and close.
        if (c->on_end) invoke_callback(*c->interp, c->on_end, {});
        close_conn_once(c);
    }
}

// uv_write request lifetime: write_req owns its data buffer + optional bnl
// callback that fires when the bytes have actually been flushed to the kernel.
// Without the callback callers can pile up writes faster than the network
// drains; the callback is what enables backpressure for streaming uploads.
struct WriteReq {
    uv_write_t   req{};
    std::string  buf;
    Interpreter* interp = nullptr;
    CallablePtr  on_done;
};

void on_write_done(uv_write_t* req, int /*status*/) {
    std::unique_ptr<WriteReq> w(static_cast<WriteReq*>(req->data));
    if (w->on_done && w->interp)
        invoke_callback(*w->interp, w->on_done, {});
}

ModulePtr build_connection_module(Connection* c) {
    auto alive = c->alive;

    // connection.write(data [, on_done])
    //   on_done() fires when the kernel has accepted the bytes, so callers can
    //   gate the next read+write on it. Omitting on_done is fire-and-forget.
    auto write_fn = [c, alive](Interpreter& interp, std::vector<Value> args) -> Value {
        if (!*alive)
            throw std::runtime_error("connection.write: connection is closed");
        if (args.empty() || args.size() > 2)
            throw std::runtime_error("connection.write: expects 1 or 2 arguments");
        if (!args[0].is_string())
            throw std::runtime_error("connection.write: data must be a string");
        auto* w = new WriteReq{};
        w->buf    = args[0].as_string();
        w->interp = &interp;
        if (args.size() == 2 && !args[1].is_null())
            w->on_done = to_callback(args[1], "connection.write");
        w->req.data = w;
        uv_buf_t buf = uv_buf_init(w->buf.data(), static_cast<unsigned int>(w->buf.size()));
        int rc = uv_write(&w->req, reinterpret_cast<uv_stream_t*>(&c->handle), &buf, 1, on_write_done);
        if (rc < 0) {
            delete w;
            throw std::runtime_error(fmt::format("connection.write: {}", uv_strerror(rc)));
        }
        return Value{};
    };

    auto close_fn = [c, alive](Interpreter&, std::vector<Value>) -> Value {
        if (!*alive) return Value{};
        close_conn_once(c);
        return Value{};
    };

    auto on_data_fn = [c, alive](Interpreter&, std::vector<Value> args) -> Value {
        if (!*alive)
            throw std::runtime_error("connection.on_data: connection is closed");
        c->on_data = to_callback(args[0], "connection.on_data");
        return Value{};
    };

    auto on_end_fn = [c, alive](Interpreter&, std::vector<Value> args) -> Value {
        if (!*alive)
            throw std::runtime_error("connection.on_end: connection is closed");
        c->on_end = to_callback(args[0], "connection.on_end");
        return Value{};
    };

    return NativeModule("tcp_connection")
        .add_function("write",  -1, write_fn)
        .add_function("close",   0, close_fn)
        .add_function("on_data", 1, on_data_fn)
        .add_function("on_end",  1, on_end_fn)
        .build();
}

// ---------- server ----------------------------------------------------------

struct Server {
    uv_tcp_t              handle{};
    Interpreter*          interp = nullptr;
    CallablePtr           on_connection;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    bool                  closing = false;
};

void on_close_server(uv_handle_t* h) {
    auto* s = static_cast<Server*>(h->data);
    if (s->alive) *s->alive = false;
    delete s;
}

void close_server_once(Server* s) {
    if (s->closing) return;
    s->closing = true;
    auto* h = reinterpret_cast<uv_handle_t*>(&s->handle);
    if (!uv_is_closing(h)) uv_close(h, on_close_server);
}

void on_new_connection(uv_stream_t* server_stream, int status) {
    auto* server = static_cast<Server*>(server_stream->data);
    if (status < 0) {
        fmt::print(stderr, "net: accept failed: {}\n", uv_strerror(status));
        return;
    }

    auto* c = new Connection{};
    c->interp = server->interp;
    uv_tcp_init(server->interp->loop(), &c->handle);
    c->handle.data = c;

    if (uv_accept(server_stream, reinterpret_cast<uv_stream_t*>(&c->handle)) != 0) {
        close_conn_once(c);
        return;
    }

    uv_read_start(reinterpret_cast<uv_stream_t*>(&c->handle), alloc_buffer, on_read);

    auto conn_module = build_connection_module(c);
    if (server->on_connection) {
        invoke_callback(*server->interp, server->on_connection, { Value{conn_module} });
    }
}

ModulePtr build_server_module(Server* s, int actual_port) {
    auto alive = s->alive;
    auto close_fn = [s, alive](Interpreter&, std::vector<Value>) -> Value {
        if (!*alive) return Value{};
        close_server_once(s);
        return Value{};
    };
    return NativeModule("tcp_server")
        .add_value   ("port", Value{static_cast<double>(actual_port)})
        .add_function("close", 0, close_fn)
        .build();
}

// ---------- DNS resolve -----------------------------------------------------

struct ResolveReq {
    uv_getaddrinfo_t  req{};
    Interpreter*      interp = nullptr;
    CallablePtr       on_done;     // fn(err, ip_string)
};

void on_resolved(uv_getaddrinfo_t* req, int status, addrinfo* res) {
    std::unique_ptr<ResolveReq> rr(static_cast<ResolveReq*>(req->data));

    if (status < 0) {
        std::string err = uv_strerror(status);
        if (res) uv_freeaddrinfo(res);
        invoke_callback(*rr->interp, rr->on_done, { Value{err}, Value{} });
        return;
    }

    // Walk the addrinfo list and pick the first IPv4 address. Falls back to the
    // first result of any family if no v4 entry exists.
    char ip[INET6_ADDRSTRLEN] = {0};
    bool got = false;
    for (addrinfo* a = res; a != nullptr; a = a->ai_next) {
        if (a->ai_family == AF_INET) {
            uv_ip4_name(reinterpret_cast<sockaddr_in*>(a->ai_addr), ip, sizeof(ip));
            got = true;
            break;
        }
    }
    if (!got && res) {
        if (res->ai_family == AF_INET6)
            uv_ip6_name(reinterpret_cast<sockaddr_in6*>(res->ai_addr), ip, sizeof(ip));
        else if (res->ai_family == AF_INET)
            uv_ip4_name(reinterpret_cast<sockaddr_in*>(res->ai_addr), ip, sizeof(ip));
        got = ip[0] != 0;
    }
    if (res) uv_freeaddrinfo(res);

    if (!got) {
        invoke_callback(*rr->interp, rr->on_done,
                        { Value{std::string("net.resolve: no usable address")}, Value{} });
        return;
    }
    invoke_callback(*rr->interp, rr->on_done, { Value{}, Value{std::string(ip)} });
}

// ---------- connect (client) ------------------------------------------------

struct ConnectReq {
    uv_connect_t  req{};
    Connection*   conn   = nullptr;
    CallablePtr   on_done;     // fn(err, conn)
};

void on_connected(uv_connect_t* req, int status) {
    std::unique_ptr<ConnectReq> cr(static_cast<ConnectReq*>(req->data));
    auto* c = cr->conn;

    if (status < 0) {
        // Failed to connect — destroy the connection, deliver the error.
        std::string err = uv_strerror(status);
        close_conn_once(c);
        invoke_callback(*c->interp, cr->on_done, { Value{err}, Value{} });
        return;
    }

    uv_read_start(reinterpret_cast<uv_stream_t*>(&c->handle), alloc_buffer, on_read);
    auto conn_module = build_connection_module(c);
    invoke_callback(*c->interp, cr->on_done, { Value{}, Value{conn_module} });
}

}  // namespace

void register_net(Interpreter& interp) {
    auto m = NativeModule("net")
        // net.tcp_listen(port, on_connection) -> server module
        // Listens on 127.0.0.1:port. Pass 0 to let the OS choose; check
        // server.port to see what was bound.
        .add_function("tcp_listen", 2,
            [&interp](Interpreter&, std::vector<Value> args) -> Value {
                if (!args[0].is_number())
                    throw std::runtime_error("net.tcp_listen: port must be a number");
                int port = static_cast<int>(args[0].as_number());
                auto cb  = to_callback(args[1], "net.tcp_listen");

                auto* s = new Server{};
                s->interp = &interp;
                s->on_connection = cb;
                uv_tcp_init(interp.loop(), &s->handle);
                s->handle.data = s;

                sockaddr_in addr{};
                if (uv_ip4_addr("127.0.0.1", port, &addr) != 0) {
                    close_server_once(s);
                    throw std::runtime_error("net.tcp_listen: invalid address");
                }
                int rc = uv_tcp_bind(&s->handle, reinterpret_cast<const sockaddr*>(&addr), 0);
                if (rc < 0) {
                    close_server_once(s);
                    throw std::runtime_error(fmt::format("net.tcp_listen bind: {}", uv_strerror(rc)));
                }
                rc = uv_listen(reinterpret_cast<uv_stream_t*>(&s->handle), 128, on_new_connection);
                if (rc < 0) {
                    close_server_once(s);
                    throw std::runtime_error(fmt::format("net.tcp_listen: {}", uv_strerror(rc)));
                }

                // Read back the actual port (relevant when port=0 was passed).
                sockaddr_storage bound{};
                int blen = sizeof(bound);
                uv_tcp_getsockname(&s->handle, reinterpret_cast<sockaddr*>(&bound), &blen);
                int actual_port = ntohs(reinterpret_cast<sockaddr_in*>(&bound)->sin_port);

                return Value{build_server_module(s, actual_port)};
            })

        // net.tcp_connect(host, port, on_done) where on_done(err, conn).
        .add_function("tcp_connect", 3,
            [&interp](Interpreter&, std::vector<Value> args) -> Value {
                if (!args[0].is_string())
                    throw std::runtime_error("net.tcp_connect: host must be a string");
                if (!args[1].is_number())
                    throw std::runtime_error("net.tcp_connect: port must be a number");
                auto cb = to_callback(args[2], "net.tcp_connect");

                auto* c = new Connection{};
                c->interp = &interp;
                uv_tcp_init(interp.loop(), &c->handle);
                c->handle.data = c;

                sockaddr_in addr{};
                if (uv_ip4_addr(args[0].as_string().c_str(),
                                static_cast<int>(args[1].as_number()), &addr) != 0) {
                    close_conn_once(c);
                    throw std::runtime_error("net.tcp_connect: invalid address");
                }

                auto* cr = new ConnectReq{};
                cr->conn    = c;
                cr->on_done = cb;
                cr->req.data = cr;
                int rc = uv_tcp_connect(&cr->req, &c->handle,
                                        reinterpret_cast<const sockaddr*>(&addr),
                                        on_connected);
                if (rc < 0) {
                    delete cr;
                    close_conn_once(c);
                    throw std::runtime_error(fmt::format("net.tcp_connect: {}", uv_strerror(rc)));
                }
                return Value{};
            })

        // net.resolve(host, on_done) where on_done(err, ip_string).
        // Resolves a hostname to an IP address using libuv's threadpool.
        .add_function("resolve", 2,
            [&interp](Interpreter&, std::vector<Value> args) -> Value {
                if (!args[0].is_string())
                    throw std::runtime_error("net.resolve: host must be a string");
                auto cb = to_callback(args[1], "net.resolve");

                auto* rr = new ResolveReq{};
                rr->interp  = &interp;
                rr->on_done = cb;
                rr->req.data = rr;

                addrinfo hints{};
                hints.ai_family   = AF_UNSPEC;
                hints.ai_socktype = SOCK_STREAM;

                int rc = uv_getaddrinfo(interp.loop(), &rr->req, on_resolved,
                                        args[0].as_string().c_str(), nullptr, &hints);
                if (rc < 0) {
                    delete rr;
                    throw std::runtime_error(fmt::format("net.resolve: {}", uv_strerror(rc)));
                }
                return Value{};
            })

        .build();

    interp.register_native_module("net", m);
}

}  // namespace bnl
