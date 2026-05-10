#include "stdlib/registry.h"

#include <fmt/core.h>
#include <uv.h>

#ifndef _WIN32
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#endif

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "bnl/interpreter.h"
#include "bnl/native_module.h"

namespace bnl {

namespace {

// Per-call state, owned by the libuv request via req.data and freed in the
// completion callback.
struct LookupReq {
    uv_getaddrinfo_t      req{};
    CallablePtr           cb;
    Interpreter*          interp = nullptr;
    bool                  all      = false;  // false → first result, true → list
};

struct ReverseReq {
    uv_getnameinfo_t      req{};
    CallablePtr           cb;
    Interpreter*          interp = nullptr;
};

void invoke_cb(Interpreter& interp, const CallablePtr& cb, std::vector<Value> args) {
    if (!cb) return;
    try {
        cb->call(interp, std::move(args));
    } catch (ThrowSignal& sig) {
        fmt::print(stderr, "uncaught throw in dns callback: {}\n",
                   sig.value.to_display());
        interp.mark_loop_failed();
    } catch (const RuntimeError& e) {
        fmt::print(stderr, "uncaught error in dns callback at {}:{} (near '{}'): {}\n",
                   e.token.line, e.token.column, e.token.lexeme, e.what());
        interp.mark_loop_failed();
    } catch (const std::exception& e) {
        fmt::print(stderr, "uncaught error in dns callback: {}\n", e.what());
        interp.mark_loop_failed();
    }
}

// Build {address, family} for one addrinfo entry. Returns nullopt if the
// family isn't IPv4 / IPv6 (rare — usually filtered by hints.ai_family).
Value addr_to_value(const addrinfo* ai) {
    char        buf[INET6_ADDRSTRLEN] = {};
    int         family                = 0;
    if (ai->ai_family == AF_INET) {
        uv_ip4_name(reinterpret_cast<const sockaddr_in*>(ai->ai_addr),
                    buf, sizeof(buf));
        family = 4;
    } else if (ai->ai_family == AF_INET6) {
        uv_ip6_name(reinterpret_cast<const sockaddr_in6*>(ai->ai_addr),
                    buf, sizeof(buf));
        family = 6;
    } else {
        return Value{};
    }
    auto m = std::make_shared<std::unordered_map<std::string, Value>>();
    (*m)["address"] = Value{std::string(buf)};
    (*m)["family"]  = Value{static_cast<double>(family)};
    return Value{m};
}

void on_lookup(uv_getaddrinfo_t* req, int status, addrinfo* res) {
    auto* lr = static_cast<LookupReq*>(req->data);

    if (status < 0 || !res) {
        // Single-result form: (err, null, null).  All-results: (err, null).
        if (lr->all)
            invoke_cb(*lr->interp, lr->cb,
                      { Value{std::string(uv_strerror(status))}, Value{} });
        else
            invoke_cb(*lr->interp, lr->cb,
                      { Value{std::string(uv_strerror(status))}, Value{}, Value{} });
        if (res) uv_freeaddrinfo(res);
        delete lr;
        return;
    }

    if (lr->all) {
        auto out = std::make_shared<std::vector<Value>>();
        for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
            Value v = addr_to_value(ai);
            if (!v.is_null()) out->push_back(std::move(v));
        }
        invoke_cb(*lr->interp, lr->cb, { Value{}, Value{out} });
    } else {
        // First IPv4 / IPv6 entry only.
        for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
            if (ai->ai_family == AF_INET || ai->ai_family == AF_INET6) {
                Value v = addr_to_value(ai);
                auto  m = v.as_map();
                invoke_cb(*lr->interp, lr->cb,
                          { Value{}, (*m)["address"], (*m)["family"] });
                uv_freeaddrinfo(res);
                delete lr;
                return;
            }
        }
        invoke_cb(*lr->interp, lr->cb,
                  { Value{std::string("no addresses returned")}, Value{}, Value{} });
    }

    uv_freeaddrinfo(res);
    delete lr;
}

void on_reverse(uv_getnameinfo_t* req, int status,
                const char* hostname, const char* /*service*/) {
    auto* rr = static_cast<ReverseReq*>(req->data);
    if (status < 0) {
        invoke_cb(*rr->interp, rr->cb,
                  { Value{std::string(uv_strerror(status))}, Value{} });
    } else {
        invoke_cb(*rr->interp, rr->cb,
                  { Value{}, Value{std::string(hostname ? hostname : "")} });
    }
    delete rr;
}

// ---------- argument helpers -----------------------------------------------

const std::string& require_string(const Value& v, const char* where) {
    if (!v.is_string()) throw std::runtime_error(std::string(where) + ": expected string");
    return v.as_string();
}

CallablePtr require_callable(const Value& v, const char* where) {
    if (!v.is_callable()) throw std::runtime_error(std::string(where) + ": expected function");
    return v.as_callable();
}

// (host, cb) or (host, family, cb). Returns the family int (0 = any).
struct LookupArgs {
    std::string host;
    int         family;
    CallablePtr cb;
};

LookupArgs parse_lookup_args(const std::vector<Value>& args, const char* fn_name) {
    if (args.size() < 2 || args.size() > 3)
        throw std::runtime_error(std::string("dns.") + fn_name +
            "(host, family?, cb): wrong arity");
    LookupArgs out;
    out.host = require_string(args[0], (std::string("dns.") + fn_name + ": host").c_str());
    if (args.size() == 2) {
        out.family = 0;
        out.cb     = require_callable(args[1],
            (std::string("dns.") + fn_name + ": cb").c_str());
    } else {
        if (!args[1].is_number())
            throw std::runtime_error(std::string("dns.") + fn_name +
                ": family must be a number (0, 4, or 6)");
        out.family = static_cast<int>(args[1].as_number());
        if (out.family != 0 && out.family != 4 && out.family != 6)
            throw std::runtime_error(std::string("dns.") + fn_name +
                ": family must be 0 (any), 4, or 6");
        out.cb = require_callable(args[2],
            (std::string("dns.") + fn_name + ": cb").c_str());
    }
    return out;
}

int family_to_ai(int family) {
    if (family == 4) return AF_INET;
    if (family == 6) return AF_INET6;
    return AF_UNSPEC;
}

}  // namespace

void register_dns(Interpreter& interp) {
    auto m = NativeModule("_dns")

        // dns.lookup(host, family?, cb) — resolve host, fire cb(err, ip, family)
        // with the FIRST matching address. family: 0 = any (default), 4 = IPv4,
        // 6 = IPv6. IP literals pass through unchanged.
        .add_function("lookup", -1,
            [&interp](Interpreter&, std::vector<Value> args) -> Value {
                auto la = parse_lookup_args(args, "lookup");

                auto* lr     = new LookupReq{};
                lr->cb       = std::move(la.cb);
                lr->interp   = &interp;
                lr->all      = false;
                lr->req.data = lr;

                addrinfo hints{};
                hints.ai_family   = family_to_ai(la.family);
                hints.ai_socktype = SOCK_STREAM;

                int rc = uv_getaddrinfo(interp.loop(), &lr->req, on_lookup,
                                        la.host.c_str(), nullptr, &hints);
                if (rc < 0) {
                    delete lr;
                    throw std::runtime_error(std::string("dns.lookup: ") + uv_strerror(rc));
                }
                return Value{};
            })

        // dns.lookup_all(host, family?, cb) — fire cb(err, [{address, family}, …])
        // with EVERY matching address. Useful for client-side load balancing.
        .add_function("lookup_all", -1,
            [&interp](Interpreter&, std::vector<Value> args) -> Value {
                auto la = parse_lookup_args(args, "lookup_all");

                auto* lr     = new LookupReq{};
                lr->cb       = std::move(la.cb);
                lr->interp   = &interp;
                lr->all      = true;
                lr->req.data = lr;

                addrinfo hints{};
                hints.ai_family   = family_to_ai(la.family);
                hints.ai_socktype = SOCK_STREAM;

                int rc = uv_getaddrinfo(interp.loop(), &lr->req, on_lookup,
                                        la.host.c_str(), nullptr, &hints);
                if (rc < 0) {
                    delete lr;
                    throw std::runtime_error(std::string("dns.lookup_all: ") + uv_strerror(rc));
                }
                return Value{};
            })

        // dns.reverse(ip, cb) — reverse lookup. cb(err, hostname). When the OS
        // returns no PTR record, hostname falls back to the IP literal (default
        // getnameinfo behaviour, no NI_NAMEREQD).
        .add_function("reverse", 2,
            [&interp](Interpreter&, std::vector<Value> args) -> Value {
                const std::string& ip_str = require_string(args[0], "dns.reverse: ip");
                CallablePtr        cb     = require_callable(args[1], "dns.reverse: cb");

                sockaddr_storage addr{};
                bool             ok_v4 = uv_ip4_addr(ip_str.c_str(), 0,
                                            reinterpret_cast<sockaddr_in*>(&addr)) == 0;
                bool             ok_v6 = !ok_v4 && uv_ip6_addr(ip_str.c_str(), 0,
                                            reinterpret_cast<sockaddr_in6*>(&addr)) == 0;
                if (!ok_v4 && !ok_v6)
                    throw std::runtime_error("dns.reverse: not a valid IPv4 or IPv6 address: " + ip_str);

                auto* rr     = new ReverseReq{};
                rr->cb       = std::move(cb);
                rr->interp   = &interp;
                rr->req.data = rr;

                int rc = uv_getnameinfo(interp.loop(), &rr->req, on_reverse,
                                        reinterpret_cast<sockaddr*>(&addr), 0);
                if (rc < 0) {
                    delete rr;
                    throw std::runtime_error(std::string("dns.reverse: ") + uv_strerror(rc));
                }
                return Value{};
            })

        .build();

    interp.register_native_module("_dns", m);
}

}  // namespace bnl
