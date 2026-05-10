#include "stdlib/registry.h"

#include <fmt/core.h>
#include <uv.h>

#ifndef _WIN32
#  include <arpa/inet.h>
#  include <netinet/in.h>
#endif

#include <cstdint>
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

// ---------------------------------------------------------------------------
// Lifecycle model (read once before reasoning about anything below):
//
//  ConnState / ListenerState are heap-allocated and owned by their libuv
//  handle via `handle.data`. Each has an `alive` shared_ptr<bool> that lambdas
//  capture by VALUE — they never deref `state` without checking `*alive` first.
//  on_*_close flips alive to false and deletes the state, so any stale lambda
//  invocation after the connection is gone short-circuits safely.
//
//  Per-call request structs (WriteReq, ConnectReq, ResolveReq) are heap-
//  allocated, stashed in req.data, and deleted in their completion callback.
// ---------------------------------------------------------------------------

struct ConnState {
    Interpreter*          interp = nullptr;
    uv_tcp_t              handle{};
    CallablePtr           on_data;
    CallablePtr           on_end;
    std::shared_ptr<bool> alive;
    bool                  closing = false;
};

struct ListenerState {
    Interpreter*          interp = nullptr;
    uv_tcp_t              handle{};
    CallablePtr           on_connection;
    std::shared_ptr<bool> alive;
    bool                  closing = false;
};

struct WriteReq {
    uv_write_t            req{};
    std::string           data;     // backs the uv_buf_t until on_write_done
    CallablePtr           on_done;  // optional; null = fire-and-forget
    Interpreter*          interp = nullptr;
    std::shared_ptr<bool> alive;    // conn alive flag — skip on_done if closed
};

struct ConnectReq {
    uv_connect_t          req{};
    CallablePtr           on_done;
    ConnState*            state = nullptr;
    Interpreter*          interp = nullptr;
};

struct ResolveReq {
    uv_getaddrinfo_t      req{};
    CallablePtr           cb;
    Interpreter*          interp = nullptr;
};

// ---------- callback safety net --------------------------------------------

void invoke_cb(Interpreter& interp, const CallablePtr& cb, std::vector<Value> args) {
    if (!cb) return;
    try {
        cb->call(interp, std::move(args));
    } catch (ThrowSignal& sig) {
        fmt::print(stderr, "uncaught throw in net callback: {}\n",
                   sig.value.to_display());
        interp.mark_loop_failed();
    } catch (const RuntimeError& e) {
        fmt::print(stderr, "uncaught error in net callback at {}:{} (near '{}'): {}\n",
                   e.token.line, e.token.column, e.token.lexeme, e.what());
        interp.mark_loop_failed();
    } catch (const std::exception& e) {
        fmt::print(stderr, "uncaught error in net callback: {}\n", e.what());
        interp.mark_loop_failed();
    }
}

// ---------- libuv read buffer alloc / close --------------------------------

void alloc_buf(uv_handle_t*, std::size_t suggested, uv_buf_t* buf) {
    buf->base = static_cast<char*>(std::malloc(suggested));
    buf->len  = static_cast<decltype(uv_buf_t::len)>(suggested);
}

void on_conn_close(uv_handle_t* h) {
    auto* state = static_cast<ConnState*>(h->data);
    if (!state) return;
    if (state->alive) *state->alive = false;
    delete state;
}

void on_listener_close(uv_handle_t* h) {
    auto* lst = static_cast<ListenerState*>(h->data);
    if (!lst) return;
    if (lst->alive) *lst->alive = false;
    delete lst;
}

void close_conn_once(ConnState* state) {
    if (state->closing) return;
    state->closing = true;
    auto* h = reinterpret_cast<uv_handle_t*>(&state->handle);
    if (!uv_is_closing(h)) {
        uv_read_stop(reinterpret_cast<uv_stream_t*>(&state->handle));
        uv_close(h, on_conn_close);
    }
}

void close_listener_once(ListenerState* lst) {
    if (lst->closing) return;
    lst->closing = true;
    auto* h = reinterpret_cast<uv_handle_t*>(&lst->handle);
    if (!uv_is_closing(h)) uv_close(h, on_listener_close);
}

// ---------- libuv read / write callbacks -----------------------------------

void on_conn_read(uv_stream_t* s, ssize_t nread, const uv_buf_t* buf) {
    auto* state = static_cast<ConnState*>(s->data);

    if (nread > 0) {
        std::string chunk(buf->base, static_cast<std::size_t>(nread));
        if (buf->base) std::free(buf->base);
        invoke_cb(*state->interp, state->on_data, { Value{std::move(chunk)} });
        return;
    }

    if (buf->base) std::free(buf->base);
    if (nread == 0) return;  // would-block; libuv may call us with nothing read

    // EOF or error — both deliver on_end and tear the connection down.
    invoke_cb(*state->interp, state->on_end, {});
    close_conn_once(state);
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

// ---------- conn module exposed to bnl -------------------------------------

ModulePtr build_conn_module(ConnState* state) {
    Interpreter* interp = state->interp;
    auto         alive  = state->alive;

    return NativeModule("conn")

        // conn.write(data, on_done?) — sends `data` (raw byte string).
        // on_done(err) fires when the write reaches the kernel; err is null
        // on success and an error message on failure.
        .add_function("write", -1,
            [state, alive, interp](Interpreter&, std::vector<Value> args) -> Value {
                if (args.empty() || args.size() > 2)
                    throw std::runtime_error("conn.write(data, on_done?): wrong arity");
                if (!*alive || state->closing)
                    throw std::runtime_error("conn.write: connection is closed");
                if (!args[0].is_string())
                    throw std::runtime_error("conn.write: data must be a string");
                if (args[0].as_string().size() > 0x7fffffffu)
                    throw std::runtime_error("conn.write: data too large (>2 GiB)");

                CallablePtr on_done;
                if (args.size() == 2 && !args[1].is_null()) {
                    if (!args[1].is_callable())
                        throw std::runtime_error("conn.write: on_done must be a function");
                    on_done = args[1].as_callable();
                }

                auto* wr = new WriteReq{};
                wr->data    = args[0].as_string();
                wr->on_done = std::move(on_done);
                wr->interp  = interp;
                wr->alive   = alive;
                wr->req.data = wr;

                uv_buf_t buf = uv_buf_init(wr->data.data(),
                                           static_cast<unsigned int>(wr->data.size()));
                int rc = uv_write(&wr->req,
                                  reinterpret_cast<uv_stream_t*>(&state->handle),
                                  &buf, 1, on_write_done);
                if (rc < 0) {
                    delete wr;
                    throw std::runtime_error(
                        std::string("conn.write: ") + uv_strerror(rc));
                }
                return Value{};
            })

        // conn.on_data(fn) — install / replace the read handler. fn(chunk).
        .add_function("on_data", 1,
            [state, alive](Interpreter&, std::vector<Value> args) -> Value {
                if (!*alive) return Value{};
                if (!args[0].is_callable())
                    throw std::runtime_error("conn.on_data: callback must be a function");
                state->on_data = args[0].as_callable();
                return Value{};
            })

        // conn.on_end(fn) — install / replace the EOF / error handler. fn().
        .add_function("on_end", 1,
            [state, alive](Interpreter&, std::vector<Value> args) -> Value {
                if (!*alive) return Value{};
                if (!args[0].is_callable())
                    throw std::runtime_error("conn.on_end: callback must be a function");
                state->on_end = args[0].as_callable();
                return Value{};
            })

        // conn.close() — tear the connection down. Idempotent.
        .add_function("close", 0,
            [state, alive](Interpreter&, std::vector<Value>) -> Value {
                if (*alive) close_conn_once(state);
                return Value{};
            })

        .build();
}

// ---------- libuv connection / connect / resolve callbacks -----------------

void on_new_connection(uv_stream_t* server, int status) {
    auto* lst = static_cast<ListenerState*>(server->data);
    if (status < 0) return;

    auto* state   = new ConnState{};
    state->interp = lst->interp;
    state->alive  = std::make_shared<bool>(true);
    uv_tcp_init(lst->interp->loop(), &state->handle);
    state->handle.data = state;

    if (uv_accept(server, reinterpret_cast<uv_stream_t*>(&state->handle)) != 0) {
        uv_close(reinterpret_cast<uv_handle_t*>(&state->handle), on_conn_close);
        return;
    }

    ModulePtr conn_module = build_conn_module(state);
    invoke_cb(*lst->interp, lst->on_connection, { Value{conn_module} });

    // Handler may have closed the conn synchronously — don't start reading
    // on a dead handle.
    if (state->closing) return;
    uv_read_start(reinterpret_cast<uv_stream_t*>(&state->handle),
                  alloc_buf, on_conn_read);
}

void on_connect_done(uv_connect_t* req, int status) {
    auto* cr = static_cast<ConnectReq*>(req->data);
    if (status < 0) {
        Value err{std::string(uv_strerror(status))};
        uv_close(reinterpret_cast<uv_handle_t*>(&cr->state->handle), on_conn_close);
        invoke_cb(*cr->interp, cr->on_done, { std::move(err), Value{} });
        delete cr;
        return;
    }
    ModulePtr conn_module = build_conn_module(cr->state);
    invoke_cb(*cr->interp, cr->on_done, { Value{}, Value{conn_module} });
    if (!cr->state->closing) {
        uv_read_start(reinterpret_cast<uv_stream_t*>(&cr->state->handle),
                      alloc_buf, on_conn_read);
    }
    delete cr;
}

void on_resolve(uv_getaddrinfo_t* req, int status, addrinfo* res) {
    auto* rr = static_cast<ResolveReq*>(req->data);
    if (status < 0 || !res) {
        invoke_cb(*rr->interp, rr->cb,
            { Value{std::string(uv_strerror(status))}, Value{} });
    } else {
        char ip[INET_ADDRSTRLEN] = {};
        auto* addr = reinterpret_cast<sockaddr_in*>(res->ai_addr);
        uv_ip4_name(addr, ip, sizeof(ip));
        invoke_cb(*rr->interp, rr->cb, { Value{}, Value{std::string(ip)} });
    }
    if (res) uv_freeaddrinfo(res);
    delete rr;
}

}  // namespace

void register_net(Interpreter& interp) {
    auto m = NativeModule("net")

        // net.tcp_listen(port, on_connection) — bind 0.0.0.0:port and listen.
        // Returns a listener module {port, close}. `port` is the actual bound
        // port (useful when the caller passes 0 for an ephemeral port).
        // on_connection(conn) fires once per accepted connection.
        .add_function("tcp_listen", 2,
            [&interp](Interpreter&, std::vector<Value> args) -> Value {
                if (!args[0].is_number())
                    throw std::runtime_error("net.tcp_listen: port must be a number");
                if (!args[1].is_callable())
                    throw std::runtime_error("net.tcp_listen: on_connection must be a function");

                int         port    = static_cast<int>(args[0].as_number());
                CallablePtr on_conn = args[1].as_callable();

                auto* lst = new ListenerState{};
                lst->interp        = &interp;
                lst->on_connection = std::move(on_conn);
                lst->alive         = std::make_shared<bool>(true);
                uv_tcp_init(interp.loop(), &lst->handle);
                lst->handle.data = lst;

                sockaddr_in addr{};
                uv_ip4_addr("0.0.0.0", port, &addr);

                int rc = uv_tcp_bind(&lst->handle,
                                     reinterpret_cast<const sockaddr*>(&addr), 0);
                if (rc < 0) {
                    uv_close(reinterpret_cast<uv_handle_t*>(&lst->handle),
                             on_listener_close);
                    throw std::runtime_error(
                        std::string("net.tcp_listen: bind: ") + uv_strerror(rc));
                }

                rc = uv_listen(reinterpret_cast<uv_stream_t*>(&lst->handle),
                               128, on_new_connection);
                if (rc < 0) {
                    uv_close(reinterpret_cast<uv_handle_t*>(&lst->handle),
                             on_listener_close);
                    throw std::runtime_error(
                        std::string("net.tcp_listen: listen: ") + uv_strerror(rc));
                }

                // Read back the bound port so the caller knows what they got
                // when they passed 0.
                struct sockaddr_storage actual;
                int alen = sizeof(actual);
                uv_tcp_getsockname(&lst->handle,
                                   reinterpret_cast<sockaddr*>(&actual), &alen);
                int actual_port = ntohs(reinterpret_cast<sockaddr_in*>(&actual)->sin_port);

                ListenerState* lst_p = lst;
                auto           alive = lst->alive;
                auto listener = NativeModule("tcp_listener")
                    .add_value("port", Value{static_cast<double>(actual_port)})
                    .add_function("close", 0,
                        [lst_p, alive](Interpreter&, std::vector<Value>) -> Value {
                            if (*alive) close_listener_once(lst_p);
                            return Value{};
                        })
                    .build();
                return Value{listener};
            })

        // net.tcp_connect(ip, port, on_done) — open a TCP client to an IPv4
        // address. Resolve hostnames separately via net.resolve.
        // on_done(err, conn) fires once: err is null on success and an error
        // string on failure.
        .add_function("tcp_connect", 3,
            [&interp](Interpreter&, std::vector<Value> args) -> Value {
                if (!args[0].is_string())
                    throw std::runtime_error("net.tcp_connect: ip must be a string");
                if (!args[1].is_number())
                    throw std::runtime_error("net.tcp_connect: port must be a number");
                if (!args[2].is_callable())
                    throw std::runtime_error("net.tcp_connect: on_done must be a function");

                const std::string& ip      = args[0].as_string();
                int                port    = static_cast<int>(args[1].as_number());
                CallablePtr        on_done = args[2].as_callable();

                sockaddr_in addr{};
                if (uv_ip4_addr(ip.c_str(), port, &addr) != 0)
                    throw std::runtime_error(
                        "net.tcp_connect: invalid IPv4 address: " + ip);

                auto* state   = new ConnState{};
                state->interp = &interp;
                state->alive  = std::make_shared<bool>(true);
                uv_tcp_init(interp.loop(), &state->handle);
                state->handle.data = state;

                auto* cr     = new ConnectReq{};
                cr->on_done  = std::move(on_done);
                cr->state    = state;
                cr->interp   = &interp;
                cr->req.data = cr;

                int rc = uv_tcp_connect(&cr->req, &state->handle,
                                        reinterpret_cast<const sockaddr*>(&addr),
                                        on_connect_done);
                if (rc < 0) {
                    delete cr;
                    uv_close(reinterpret_cast<uv_handle_t*>(&state->handle),
                             on_conn_close);
                    throw std::runtime_error(
                        std::string("net.tcp_connect: ") + uv_strerror(rc));
                }
                return Value{};
            })

        // net.resolve(host, cb) — IPv4-only DNS via uv_getaddrinfo.
        // cb(err, ip): err is null on success, ip is the first A record as a
        // dotted-quad string. IP literals pass through unchanged.
        .add_function("resolve", 2,
            [&interp](Interpreter&, std::vector<Value> args) -> Value {
                if (!args[0].is_string())
                    throw std::runtime_error("net.resolve: host must be a string");
                if (!args[1].is_callable())
                    throw std::runtime_error("net.resolve: callback must be a function");

                std::string host = args[0].as_string();
                CallablePtr cb   = args[1].as_callable();

                auto* rr      = new ResolveReq{};
                rr->cb        = std::move(cb);
                rr->interp    = &interp;
                rr->req.data  = rr;

                addrinfo hints{};
                hints.ai_family   = AF_INET;
                hints.ai_socktype = SOCK_STREAM;

                int rc = uv_getaddrinfo(interp.loop(), &rr->req, on_resolve,
                                        host.c_str(), nullptr, &hints);
                if (rc < 0) {
                    delete rr;
                    throw std::runtime_error(
                        std::string("net.resolve: ") + uv_strerror(rc));
                }
                return Value{};
            })

        .build();

    interp.register_native_module("net", m);
}

}  // namespace bnl
