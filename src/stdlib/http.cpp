#include "stdlib/registry.h"

#include <fmt/core.h>
#include <llhttp.h>

#include <cstddef>
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

// State accumulated through llhttp's per-token callbacks. One instance per
// parse_* call — the parser is one-shot, no internal buffering.
struct ParseState {
    std::string                                          url;          // request only
    std::string                                          status_text;  // response only
    std::vector<std::pair<std::string, std::string>>     headers;
    std::string                                          field_buf;
    std::string                                          value_buf;
    enum LastSeen { None, Field, Value }                 last_seen = None;
    std::string                                          body;
    bool                                                 message_complete = false;
};

int cb_url(llhttp_t* p, const char* at, std::size_t len) {
    static_cast<ParseState*>(p->data)->url.append(at, len);
    return 0;
}
int cb_status(llhttp_t* p, const char* at, std::size_t len) {
    static_cast<ParseState*>(p->data)->status_text.append(at, len);
    return 0;
}
int cb_header_field(llhttp_t* p, const char* at, std::size_t len) {
    auto* s = static_cast<ParseState*>(p->data);
    if (s->last_seen == ParseState::Value) {
        // Field after value → previous pair is finished, push it.
        s->headers.emplace_back(std::move(s->field_buf), std::move(s->value_buf));
        s->field_buf.clear();
        s->value_buf.clear();
    }
    s->field_buf.append(at, len);
    s->last_seen = ParseState::Field;
    return 0;
}
int cb_header_value(llhttp_t* p, const char* at, std::size_t len) {
    auto* s = static_cast<ParseState*>(p->data);
    s->value_buf.append(at, len);
    s->last_seen = ParseState::Value;
    return 0;
}
int cb_headers_complete(llhttp_t* p) {
    auto* s = static_cast<ParseState*>(p->data);
    if (s->last_seen == ParseState::Value) {
        s->headers.emplace_back(std::move(s->field_buf), std::move(s->value_buf));
        s->field_buf.clear();
        s->value_buf.clear();
        s->last_seen = ParseState::None;
    }
    return 0;
}
int cb_body(llhttp_t* p, const char* at, std::size_t len) {
    static_cast<ParseState*>(p->data)->body.append(at, len);
    return 0;
}
int cb_message_complete(llhttp_t* p) {
    static_cast<ParseState*>(p->data)->message_complete = true;
    // Pause so llhttp_get_error_pos tells us exactly where this message ended
    // in the input buffer — the caller uses that as `consumed` to drop the
    // bytes belonging to this message and keep any pipelined bytes that follow.
    return HPE_PAUSED;
}

void install_callbacks(llhttp_settings_t& s) {
    llhttp_settings_init(&s);
    s.on_url              = cb_url;
    s.on_status           = cb_status;
    s.on_header_field     = cb_header_field;
    s.on_header_value     = cb_header_value;
    s.on_headers_complete = cb_headers_complete;
    s.on_body             = cb_body;
    s.on_message_complete = cb_message_complete;
}

std::string ascii_lower(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// Pack ParseState + parser metadata into the {method, url, ...} map returned
// to bnl. `consumed` lets the caller slice the buffer for keep-alive / pipelined
// connections; `complete` tells them whether the message ended inside `buf`.
Value build_result(int type, llhttp_t& parser, ParseState& state,
                   std::size_t consumed) {
    auto out = std::make_shared<std::unordered_map<std::string, Value>>();

    if (type == HTTP_REQUEST) {
        const char* m = llhttp_method_name(static_cast<llhttp_method_t>(parser.method));
        (*out)["method"] = Value{std::string(m ? m : "")};
        (*out)["url"]    = Value{state.url};
    } else {
        (*out)["status"] = Value{static_cast<double>(parser.status_code)};
        (*out)["reason"] = Value{state.status_text};
    }

    (*out)["version"] = Value{fmt::format("{}.{}", parser.http_major, parser.http_minor)};

    auto hmap = std::make_shared<std::unordered_map<std::string, Value>>();
    for (auto& [k, v] : state.headers) (*hmap)[ascii_lower(k)] = Value{v};
    (*out)["headers"]  = Value{hmap};

    (*out)["body"]     = Value{state.body};
    (*out)["complete"] = Value{state.message_complete};
    (*out)["consumed"] = Value{static_cast<double>(consumed)};
    return Value{out};
}

Value parse_one(int type, const std::string& buf, bool finalize) {
    llhttp_t          parser;
    llhttp_settings_t settings;
    install_callbacks(settings);
    llhttp_init(&parser, static_cast<llhttp_type_t>(type), &settings);

    ParseState state;
    parser.data = &state;

    enum llhttp_errno err = llhttp_execute(&parser, buf.data(), buf.size());

    std::size_t consumed = buf.size();
    if (err == HPE_PAUSED) {
        // We paused inside on_message_complete — the position is right after
        // the consumed message bytes.
        const char* pos = llhttp_get_error_pos(&parser);
        consumed = static_cast<std::size_t>(pos - buf.data());
    } else if (err != HPE_OK) {
        throw std::runtime_error(fmt::format("http.parse: {}: {}",
            llhttp_errno_name(err), llhttp_get_error_reason(&parser)));
    }

    // For HTTP/1.0 responses without Content-Length the body is delimited by
    // EOF — the caller signals "no more bytes coming" by passing finalize=true,
    // and we ask llhttp to wrap up the message.
    if (finalize && !state.message_complete) {
        enum llhttp_errno fe = llhttp_finish(&parser);
        if (fe == HPE_OK || fe == HPE_PAUSED) {
            consumed = buf.size();
        } else {
            throw std::runtime_error(fmt::format("http.parse (finalize): {}: {}",
                llhttp_errno_name(fe), llhttp_get_error_reason(&parser)));
        }
    }

    return build_result(type, parser, state, consumed);
}

const std::string& require_string(const Value& v, const char* where) {
    if (!v.is_string())
        throw std::runtime_error(std::string(where) + ": expected string");
    return v.as_string();
}

bool optional_bool(const std::vector<Value>& args, std::size_t i, bool def) {
    if (args.size() <= i || args[i].is_null()) return def;
    if (!args[i].is_bool())
        throw std::runtime_error("http.parse_response: is_final must be a bool");
    return args[i].as_bool();
}

}  // namespace

void register_http(Interpreter& interp) {
    auto m = NativeModule("http")

        // http.parse_request(buf) — parse one HTTP request from `buf`. Returns
        // {method, url, version, headers, body, complete, consumed}.
        // headers is a map with lowercased names.
        // complete=false means the buffer doesn't contain a full message yet
        // (the caller should accumulate more bytes and try again).
        // consumed is how many bytes of `buf` belonged to the parsed message —
        // any tail past that is the start of the next pipelined request.
        .add_function("parse_request", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                return parse_one(HTTP_REQUEST,
                                 require_string(args[0], "http.parse_request: buf"),
                                 /*finalize=*/false);
            })

        // http.parse_response(buf, is_final?) — same shape as parse_request,
        // but returns {status, reason, ...} instead of {method, url, ...}.
        // Pass is_final=true for the last chunk of an HTTP/1.0 response whose
        // body is delimited by connection EOF (no Content-Length).
        .add_function("parse_response", -1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                if (args.empty() || args.size() > 2)
                    throw std::runtime_error(
                        "http.parse_response(buf, is_final?): wrong arity");
                return parse_one(HTTP_RESPONSE,
                                 require_string(args[0], "http.parse_response: buf"),
                                 optional_bool(args, 1, false));
            })

        .build();

    interp.register_native_module("http", m);
}

}  // namespace bnl
