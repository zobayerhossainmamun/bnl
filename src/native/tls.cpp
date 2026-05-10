#include "native/builtins.h"

#include <fmt/core.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#include <openssl/x509.h>
#include <uv.h>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#endif

#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "bnl/interpreter.h"
#include "bnl/native_module.h"

namespace bnl {

namespace {

// ---------- shared SSL_CTX --------------------------------------------------

#ifdef _WIN32
// vcpkg's OpenSSL on Windows has no useful default trust dir, so pull root
// certs from the Windows ROOT store on first init. Without this, every TLS
// verify against a public site would fail.
void load_windows_root_certs(SSL_CTX* ctx) {
    HCERTSTORE store = CertOpenSystemStoreA(0, "ROOT");
    if (!store) return;
    X509_STORE* x509 = SSL_CTX_get_cert_store(ctx);
    PCCERT_CONTEXT pctx = nullptr;
    while ((pctx = CertEnumCertificatesInStore(store, pctx)) != nullptr) {
        const unsigned char* enc = pctx->pbCertEncoded;
        X509* x = d2i_X509(nullptr, &enc, pctx->cbCertEncoded);
        if (x) {
            X509_STORE_add_cert(x509, x);
            X509_free(x);
        }
    }
    CertCloseStore(store, 0);
}
#endif

SSL_CTX* shared_client_ctx() {
    static SSL_CTX* ctx = []() -> SSL_CTX* {
        SSL_CTX* c = SSL_CTX_new(TLS_client_method());
        if (!c) return nullptr;
        SSL_CTX_set_min_proto_version(c, TLS1_2_VERSION);
        SSL_CTX_set_default_verify_paths(c);
#ifdef _WIN32
        load_windows_root_certs(c);
#endif
        return c;
    }();
    if (!ctx) throw std::runtime_error("tls: SSL_CTX_new failed");
    return ctx;
}

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
        fmt::print(stderr, "uncaught error in tls callback at {}:{} (near '{}'): {}\n",
                   e.token.line, e.token.column, e.token.lexeme, e.what());
        interp.mark_loop_failed();
    } catch (const std::exception& e) {
        fmt::print(stderr, "uncaught error in tls callback: {}\n", e.what());
        interp.mark_loop_failed();
    }
}

std::string ssl_err() {
    unsigned long e = ERR_get_error();
    if (e == 0) return "(no openssl error)";
    char buf[256];
    ERR_error_string_n(e, buf, sizeof(buf));
    return buf;
}

// ---------- connection state -----------------------------------------------

struct TlsConn {
    uv_tcp_t              handle{};
    Interpreter*          interp = nullptr;
    SSL*                  ssl = nullptr;
    BIO*                  internal_bio = nullptr;     // SSL_set_bio takes ownership
    BIO*                  network_bio  = nullptr;     // we own and shuttle bytes
    std::string           hostname;
    CallablePtr           on_handshake;               // fn(err, conn) — fired once
    CallablePtr           on_data;
    CallablePtr           on_end;
    bool                  handshake_done = false;
    bool                  closing = false;
    bool                  verify_peer = true;         // false to skip cert check post-handshake
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};

void on_close(uv_handle_t* h) {
    auto* c = static_cast<TlsConn*>(h->data);
    if (c->alive) *c->alive = false;
    if (c->ssl) SSL_free(c->ssl);                 // also frees internal_bio
    if (c->network_bio) BIO_free(c->network_bio); // we own this side of the pair
    delete c;
}

void close_once(TlsConn* c) {
    if (c->closing) return;
    c->closing = true;
    auto* h = reinterpret_cast<uv_handle_t*>(&c->handle);
    if (!uv_is_closing(h)) uv_close(h, on_close);
}

// ---------- write to network -----------------------------------------------

struct WriteReq {
    uv_write_t   req{};
    std::string  buf;
    Interpreter* interp = nullptr;
    CallablePtr  on_done;       // fired when this particular write flushes
};

void on_write_done(uv_write_t* req, int /*status*/) {
    std::unique_ptr<WriteReq> w(static_cast<WriteReq*>(req->data));
    if (w->on_done && w->interp)
        invoke_callback(*w->interp, w->on_done, {});
}

// Drain the network BIO into one or more uv_write calls. Returns false on
// uv_write error. If on_done is supplied, it's attached to the last queued
// write so the caller learns when the bytes have actually flushed (used for
// streaming-upload backpressure). One SSL_write may cause multiple uv_writes;
// uv_write completions fire in order, so attaching to the last is correct.
bool flush_network(TlsConn* c, Interpreter* interp = nullptr,
                   CallablePtr on_done = nullptr) {
    WriteReq* last = nullptr;
    while (true) {
        char chunk[4096];
        int n = BIO_read(c->network_bio, chunk, sizeof(chunk));
        if (n <= 0) break;
        auto* w = new WriteReq{};
        w->buf.assign(chunk, n);
        w->req.data = w;
        uv_buf_t b = uv_buf_init(w->buf.data(), static_cast<unsigned int>(w->buf.size()));
        int rc = uv_write(&w->req, reinterpret_cast<uv_stream_t*>(&c->handle),
                          &b, 1, on_write_done);
        if (rc < 0) {
            delete w;
            return false;
        }
        last = w;
    }
    if (on_done) {
        if (last) {
            last->interp  = interp;
            last->on_done = std::move(on_done);
        } else if (interp) {
            // Nothing was pending in the BIO — fire immediately so callers
            // don't deadlock waiting for a flush that never had data.
            invoke_callback(*interp, on_done, {});
        }
    }
    return true;
}

// ---------- pump ------------------------------------------------------------
// Runs after every event that might unblock the SSL state machine: TCP
// connected, network bytes received, user write. Drives both directions.

ModulePtr build_connection_module(TlsConn* c);

void pump(TlsConn* c) {
    if (c->closing) return;

    if (!c->handshake_done) {
        int rc = SSL_do_handshake(c->ssl);
        // Always drain whatever the handshake wanted to send.
        if (!flush_network(c)) {
            if (c->on_handshake) {
                auto cb = c->on_handshake;
                c->on_handshake = nullptr;
                invoke_callback(*c->interp, cb,
                                { Value{std::string("tls: uv_write failed during handshake")}, Value{} });
            }
            close_once(c);
            return;
        }
        if (rc == 1) {
            c->handshake_done = true;
            if (c->verify_peer) {
                long vr = SSL_get_verify_result(c->ssl);
                if (vr != X509_V_OK) {
                    std::string err = "tls: certificate verify failed: ";
                    err += X509_verify_cert_error_string(vr);
                    if (c->on_handshake) {
                        auto cb = c->on_handshake;
                        c->on_handshake = nullptr;
                        invoke_callback(*c->interp, cb, { Value{err}, Value{} });
                    }
                    close_once(c);
                    return;
                }
            }
            auto mod = build_connection_module(c);
            if (c->on_handshake) {
                auto cb = c->on_handshake;
                c->on_handshake = nullptr;
                invoke_callback(*c->interp, cb, { Value{}, Value{mod} });
            }
            // Fall through — caller may have installed on_data and there might
            // already be bytes in the SSL read queue.
        } else {
            int err = SSL_get_error(c->ssl, rc);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) return;
            std::string msg = "tls handshake: " + ssl_err();
            if (c->on_handshake) {
                auto cb = c->on_handshake;
                c->on_handshake = nullptr;
                invoke_callback(*c->interp, cb, { Value{msg}, Value{} });
            }
            close_once(c);
            return;
        }
    }

    // Post-handshake: drain decrypted bytes and dispatch.
    while (true) {
        char buf[8192];
        int rc = SSL_read(c->ssl, buf, sizeof(buf));
        if (rc > 0) {
            if (c->on_data) {
                std::string chunk(buf, rc);
                invoke_callback(*c->interp, c->on_data, { Value{std::move(chunk)} });
            }
            continue;
        }
        int err = SSL_get_error(c->ssl, rc);
        if (err == SSL_ERROR_WANT_READ) break;
        if (err == SSL_ERROR_ZERO_RETURN) {
            // Peer sent close_notify — clean shutdown.
            if (c->on_end) invoke_callback(*c->interp, c->on_end, {});
            close_once(c);
            return;
        }
        // Anything else is an error.
        fmt::print(stderr, "tls read: {}\n", ssl_err());
        close_once(c);
        return;
    }
    flush_network(c);
}

void alloc_buffer(uv_handle_t*, std::size_t suggested, uv_buf_t* buf) {
    buf->base = static_cast<char*>(std::malloc(suggested));
    buf->len  = static_cast<decltype(buf->len)>(suggested);
}

void on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    auto* c = static_cast<TlsConn*>(stream->data);
    if (nread > 0) {
        // Push encrypted bytes into the network BIO so SSL can decrypt them.
        int written = 0;
        while (written < nread) {
            int rc = BIO_write(c->network_bio, buf->base + written,
                               static_cast<int>(nread - written));
            if (rc <= 0) break;
            written += rc;
        }
        std::free(buf->base);
        pump(c);
        return;
    }
    if (buf->base) std::free(buf->base);
    if (nread < 0) {
        if (!c->handshake_done && c->on_handshake) {
            auto cb = c->on_handshake;
            c->on_handshake = nullptr;
            invoke_callback(*c->interp, cb,
                            { Value{std::string("tls: connection closed during handshake")}, Value{} });
        } else if (c->on_end) {
            invoke_callback(*c->interp, c->on_end, {});
        }
        close_once(c);
    }
}

// ---------- connection module ----------------------------------------------

ModulePtr build_connection_module(TlsConn* c) {
    auto alive = c->alive;

    // tls.write(data [, on_done]) — same backpressure contract as net.write:
    // on_done() fires once the encrypted bytes have actually been kernel-flushed.
    auto write_fn = [c, alive](Interpreter& interp, std::vector<Value> args) -> Value {
        if (!*alive) throw std::runtime_error("tls.write: connection is closed");
        if (args.empty() || args.size() > 2)
            throw std::runtime_error("tls.write: expects 1 or 2 arguments");
        if (!args[0].is_string())
            throw std::runtime_error("tls.write: data must be a string");
        CallablePtr on_done;
        if (args.size() == 2 && !args[1].is_null())
            on_done = to_callback(args[1], "tls.write");

        const std::string& s = args[0].as_string();
        int rc = SSL_write(c->ssl, s.data(), static_cast<int>(s.size()));
        if (rc <= 0) {
            (void)SSL_get_error(c->ssl, rc);
            throw std::runtime_error("tls.write: SSL_write failed: " + ssl_err());
        }
        if (!flush_network(c, &interp, std::move(on_done)))
            throw std::runtime_error("tls.write: uv_write failed");
        return Value{};
    };

    auto close_fn = [c, alive](Interpreter&, std::vector<Value>) -> Value {
        if (!*alive) return Value{};
        SSL_shutdown(c->ssl);
        flush_network(c);
        close_once(c);
        return Value{};
    };

    auto on_data_fn = [c, alive](Interpreter&, std::vector<Value> args) -> Value {
        if (!*alive) throw std::runtime_error("tls.on_data: connection is closed");
        c->on_data = to_callback(args[0], "tls.on_data");
        return Value{};
    };

    auto on_end_fn = [c, alive](Interpreter&, std::vector<Value> args) -> Value {
        if (!*alive) throw std::runtime_error("tls.on_end: connection is closed");
        c->on_end = to_callback(args[0], "tls.on_end");
        return Value{};
    };

    return NativeModule("tls_connection")
        .add_function("write",  -1, write_fn)
        .add_function("close",   0, close_fn)
        .add_function("on_data", 1, on_data_fn)
        .add_function("on_end",  1, on_end_fn)
        .build();
}

// ---------- TCP connect, then handshake ------------------------------------

struct ConnectReq {
    uv_connect_t  req{};
    TlsConn*      conn = nullptr;
};

void on_tcp_connected(uv_connect_t* req, int status) {
    std::unique_ptr<ConnectReq> cr(static_cast<ConnectReq*>(req->data));
    auto* c = cr->conn;

    if (status < 0) {
        std::string err = std::string("tls connect: ") + uv_strerror(status);
        if (c->on_handshake) {
            auto cb = c->on_handshake;
            c->on_handshake = nullptr;
            invoke_callback(*c->interp, cb, { Value{err}, Value{} });
        }
        close_once(c);
        return;
    }

    int rc = uv_read_start(reinterpret_cast<uv_stream_t*>(&c->handle),
                           alloc_buffer, on_read);
    if (rc < 0) {
        if (c->on_handshake) {
            auto cb = c->on_handshake;
            c->on_handshake = nullptr;
            invoke_callback(*c->interp, cb,
                            { Value{std::string("tls: uv_read_start failed")}, Value{} });
        }
        close_once(c);
        return;
    }
    SSL_set_connect_state(c->ssl);
    pump(c);
}

// ---------- server: tls.listen --------------------------------------------

struct TlsServer {
    uv_tcp_t              handle{};
    Interpreter*          interp = nullptr;
    SSL_CTX*              ctx = nullptr;     // owned; freed in on_close_server
    CallablePtr           on_connection;     // user fn(conn)
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    bool                  closing = false;
};

void on_close_server(uv_handle_t* h) {
    auto* s = static_cast<TlsServer*>(h->data);
    if (s->alive) *s->alive = false;
    if (s->ctx) SSL_CTX_free(s->ctx);
    delete s;
}

void close_server_once(TlsServer* s) {
    if (s->closing) return;
    s->closing = true;
    auto* h = reinterpret_cast<uv_handle_t*>(&s->handle);
    if (!uv_is_closing(h)) uv_close(h, on_close_server);
}

void on_new_tls_connection(uv_stream_t* server_stream, int status) {
    auto* server = static_cast<TlsServer*>(server_stream->data);
    if (status < 0) {
        fmt::print(stderr, "tls.listen: accept failed: {}\n", uv_strerror(status));
        return;
    }

    auto* c = new TlsConn{};
    c->interp = server->interp;
    uv_tcp_init(server->interp->loop(), &c->handle);
    c->handle.data = c;

    if (uv_accept(server_stream, reinterpret_cast<uv_stream_t*>(&c->handle)) != 0) {
        close_once(c);
        return;
    }

    c->ssl = SSL_new(server->ctx);
    if (!c->ssl) {
        fmt::print(stderr, "tls.listen: SSL_new: {}\n", ssl_err());
        close_once(c);
        return;
    }
    if (BIO_new_bio_pair(&c->internal_bio, 0, &c->network_bio, 0) != 1) {
        fmt::print(stderr, "tls.listen: BIO_new_bio_pair failed\n");
        close_once(c);
        return;
    }
    SSL_set_bio(c->ssl, c->internal_bio, c->internal_bio);
    SSL_set_accept_state(c->ssl);

    // Wrap the user's on_connection so pump's on_handshake(err, conn) signature
    // works for both client and server. On server-side handshake error, log and
    // drop; on success, hand the conn to the user.
    auto user_cb = server->on_connection;
    c->on_handshake = std::make_shared<NativeFunction>(
        "tls_server_handshake_done", 2,
        [user_cb](Interpreter& i, std::vector<Value> args) -> Value {
            if (args[0].is_string()) {
                fmt::print(stderr, "tls.listen: handshake failed: {}\n", args[0].as_string());
                return Value{};
            }
            invoke_callback(i, user_cb, { std::move(args[1]) });
            return Value{};
        });

    int rc = uv_read_start(reinterpret_cast<uv_stream_t*>(&c->handle),
                           alloc_buffer, on_read);
    if (rc < 0) {
        close_once(c);
        return;
    }
    pump(c);
}

}  // namespace

void register_tls(Interpreter& interp) {
    auto m = NativeModule("tls")
        // tls.connect(hostname, ip, port, on_done [, opts]) where on_done(err, conn).
        // hostname is used for SNI and certificate verification.
        // ip is the dotted-quad to dial — call net.resolve(hostname, ...) first.
        // opts (optional map): {verify: bool}. verify defaults to true; pass
        // false to skip cert verification (test/dev only).
        .add_function("connect", -1,
            [&interp](Interpreter&, std::vector<Value> args) -> Value {
                if (args.size() < 4 || args.size() > 5)
                    throw std::runtime_error("tls.connect: expects 4 or 5 arguments");
                if (!args[0].is_string()) throw std::runtime_error("tls.connect: hostname must be a string");
                if (!args[1].is_string()) throw std::runtime_error("tls.connect: ip must be a string");
                if (!args[2].is_number()) throw std::runtime_error("tls.connect: port must be a number");
                auto cb = to_callback(args[3], "tls.connect");

                bool verify = true;
                if (args.size() == 5) {
                    if (!args[4].is_map())
                        throw std::runtime_error("tls.connect: opts must be a map");
                    const auto& opts = args[4].as_map();
                    auto it = opts->find("verify");
                    if (it != opts->end() && it->second.is_bool()) verify = it->second.as_bool();
                }

                SSL_CTX* ctx = shared_client_ctx();

                auto* c = new TlsConn{};
                c->interp = &interp;
                c->hostname = args[0].as_string();
                c->on_handshake = cb;
                c->verify_peer = verify;
                uv_tcp_init(interp.loop(), &c->handle);
                c->handle.data = c;

                c->ssl = SSL_new(ctx);
                if (!c->ssl) {
                    std::string err = "tls.connect: SSL_new failed: " + ssl_err();
                    delete c;
                    throw std::runtime_error(err);
                }
                if (BIO_new_bio_pair(&c->internal_bio, 0, &c->network_bio, 0) != 1) {
                    SSL_free(c->ssl);
                    delete c;
                    throw std::runtime_error("tls.connect: BIO_new_bio_pair failed");
                }
                SSL_set_bio(c->ssl, c->internal_bio, c->internal_bio);
                SSL_set_tlsext_host_name(c->ssl, c->hostname.c_str());
                if (verify) {
                    SSL_set_verify(c->ssl, SSL_VERIFY_PEER, nullptr);
                    SSL_set1_host(c->ssl, c->hostname.c_str());
                } else {
                    SSL_set_verify(c->ssl, SSL_VERIFY_NONE, nullptr);
                }

                sockaddr_in addr{};
                if (uv_ip4_addr(args[1].as_string().c_str(),
                                static_cast<int>(args[2].as_number()), &addr) != 0) {
                    close_once(c);
                    throw std::runtime_error(
                        "tls.connect: invalid ip — pass a dotted-quad (call net.resolve first)");
                }

                auto* cr = new ConnectReq{};
                cr->conn = c;
                cr->req.data = cr;
                int rc = uv_tcp_connect(&cr->req, &c->handle,
                                        reinterpret_cast<const sockaddr*>(&addr),
                                        on_tcp_connected);
                if (rc < 0) {
                    delete cr;
                    close_once(c);
                    throw std::runtime_error(fmt::format("tls.connect: {}", uv_strerror(rc)));
                }
                return Value{};
            })

        // tls.listen(port, opts, on_connection) where on_connection(conn).
        // opts must be a map with 'cert' and 'key' (PEM file paths). Returns
        // a server module with .port (number) and .close() — same shape as
        // net.tcp_listen, so lib code can swap transports.
        .add_function("listen", 3,
            [&interp](Interpreter&, std::vector<Value> args) -> Value {
                if (!args[0].is_number())
                    throw std::runtime_error("tls.listen: port must be a number");
                if (!args[1].is_map())
                    throw std::runtime_error("tls.listen: opts must be a map with 'cert' and 'key'");
                auto cb = to_callback(args[2], "tls.listen");

                const auto& opts = args[1].as_map();
                auto get_str = [&](const char* k) -> std::string {
                    auto it = opts->find(k);
                    if (it == opts->end() || !it->second.is_string())
                        throw std::runtime_error(
                            fmt::format("tls.listen: opts.{} must be a string", k));
                    return it->second.as_string();
                };
                std::string cert_path = get_str("cert");
                std::string key_path  = get_str("key");

                SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
                if (!ctx) throw std::runtime_error("tls.listen: SSL_CTX_new failed: " + ssl_err());
                SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
                if (SSL_CTX_use_certificate_file(ctx, cert_path.c_str(), SSL_FILETYPE_PEM) <= 0) {
                    std::string err = "tls.listen: load cert '" + cert_path + "': " + ssl_err();
                    SSL_CTX_free(ctx);
                    throw std::runtime_error(err);
                }
                if (SSL_CTX_use_PrivateKey_file(ctx, key_path.c_str(), SSL_FILETYPE_PEM) <= 0) {
                    std::string err = "tls.listen: load key '" + key_path + "': " + ssl_err();
                    SSL_CTX_free(ctx);
                    throw std::runtime_error(err);
                }
                if (SSL_CTX_check_private_key(ctx) != 1) {
                    SSL_CTX_free(ctx);
                    throw std::runtime_error("tls.listen: cert/key mismatch");
                }

                auto* s = new TlsServer{};
                s->interp        = &interp;
                s->ctx           = ctx;
                s->on_connection = cb;
                uv_tcp_init(interp.loop(), &s->handle);
                s->handle.data = s;

                sockaddr_in addr{};
                if (uv_ip4_addr("127.0.0.1", static_cast<int>(args[0].as_number()), &addr) != 0) {
                    close_server_once(s);
                    throw std::runtime_error("tls.listen: invalid address");
                }
                int rc = uv_tcp_bind(&s->handle, reinterpret_cast<const sockaddr*>(&addr), 0);
                if (rc < 0) {
                    close_server_once(s);
                    throw std::runtime_error(fmt::format("tls.listen bind: {}", uv_strerror(rc)));
                }
                rc = uv_listen(reinterpret_cast<uv_stream_t*>(&s->handle),
                               128, on_new_tls_connection);
                if (rc < 0) {
                    close_server_once(s);
                    throw std::runtime_error(fmt::format("tls.listen: {}", uv_strerror(rc)));
                }

                sockaddr_storage bound{};
                int blen = sizeof(bound);
                uv_tcp_getsockname(&s->handle, reinterpret_cast<sockaddr*>(&bound), &blen);
                int actual_port = ntohs(reinterpret_cast<sockaddr_in*>(&bound)->sin_port);

                auto alive = s->alive;
                auto close_fn = [s, alive](Interpreter&, std::vector<Value>) -> Value {
                    if (!*alive) return Value{};
                    close_server_once(s);
                    return Value{};
                };

                return Value{NativeModule("tls_server")
                    .add_value   ("port",  Value{static_cast<double>(actual_port)})
                    .add_function("close", 0, close_fn)
                    .build()};
            })
        .build();

    interp.register_native_module("tls", m);
}

}  // namespace bnl
