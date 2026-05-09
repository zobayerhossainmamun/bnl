#include "native/builtins.h"

#include <fmt/core.h>
#include <llhttp.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "bnl/interpreter.h"
#include "bnl/native_module.h"
#include "environment.h"

namespace bnl {

namespace {

struct Parsed {
    std::string                                       method;
    std::string                                       url;
    std::string                                       body;
    std::vector<std::pair<std::string, std::string>>  headers;
    std::string                                       current_header_name;
    bool                                              complete = false;
};

int on_url(llhttp_t* p, const char* at, std::size_t len) {
    static_cast<Parsed*>(p->data)->url.append(at, len);
    return 0;
}

int on_header_field(llhttp_t* p, const char* at, std::size_t len) {
    auto* parsed = static_cast<Parsed*>(p->data);
    // If the previous header value just finished, push the pair we built up.
    if (!parsed->current_header_name.empty() && !parsed->headers.empty()
        && parsed->headers.back().first == parsed->current_header_name
        && parsed->current_header_name == parsed->headers.back().first) {
        parsed->current_header_name.clear();
    }
    parsed->current_header_name.append(at, len);
    return 0;
}

int on_header_value(llhttp_t* p, const char* at, std::size_t len) {
    auto* parsed = static_cast<Parsed*>(p->data);
    if (!parsed->current_header_name.empty() &&
        (parsed->headers.empty() || parsed->headers.back().first != parsed->current_header_name)) {
        parsed->headers.emplace_back(parsed->current_header_name, std::string{});
    }
    if (!parsed->headers.empty())
        parsed->headers.back().second.append(at, len);
    return 0;
}

int on_header_value_complete(llhttp_t* p) {
    static_cast<Parsed*>(p->data)->current_header_name.clear();
    return 0;
}

int on_body(llhttp_t* p, const char* at, std::size_t len) {
    static_cast<Parsed*>(p->data)->body.append(at, len);
    return 0;
}

int on_message_complete(llhttp_t* p) {
    static_cast<Parsed*>(p->data)->complete = true;
    return 0;
}

std::string ascii_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

ModulePtr build_request_module(Parsed&& parsed) {
    // Capture headers by shared_ptr so the .header() closure outlives this call.
    auto headers = std::make_shared<std::vector<std::pair<std::string, std::string>>>(
        std::move(parsed.headers));

    return NativeModule("http_request")
        .add_value("method",       Value{std::move(parsed.method)})
        .add_value("url",          Value{std::move(parsed.url)})
        .add_value("body",         Value{std::move(parsed.body)})
        .add_value("header_count", Value{static_cast<double>(headers->size())})
        .add_value("complete",     Value{parsed.complete})
        .add_function("header", 1,
            [headers](Interpreter&, std::vector<Value> args) -> Value {
                if (!args[0].is_string())
                    throw std::runtime_error("request.header(name): name must be a string");
                auto needle = ascii_lower(args[0].as_string());
                for (const auto& [k, v] : *headers) {
                    if (ascii_lower(k) == needle) return Value{v};
                }
                return Value{};
            })
        .build();
}

}  // namespace

void register_httpp(Interpreter& interp) {
    auto m = NativeModule("httpp")
        // httpp.parse_request(buf) -> Module | null
        // Returns null if the buffer doesn't contain a complete request yet,
        // throws on parse error.
        .add_function("parse_request", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                if (!args[0].is_string())
                    throw std::runtime_error("httpp.parse_request: input must be a string");

                const std::string& buf = args[0].as_string();

                llhttp_settings_t settings;
                llhttp_settings_init(&settings);
                settings.on_url                   = on_url;
                settings.on_header_field          = on_header_field;
                settings.on_header_value          = on_header_value;
                settings.on_header_value_complete = on_header_value_complete;
                settings.on_body                  = on_body;
                settings.on_message_complete      = on_message_complete;

                llhttp_t parser;
                llhttp_init(&parser, HTTP_REQUEST, &settings);
                Parsed parsed;
                parser.data = &parsed;

                auto err = llhttp_execute(&parser, buf.data(), buf.size());
                if (err != HPE_OK && err != HPE_PAUSED_UPGRADE) {
                    throw std::runtime_error(fmt::format(
                        "httpp.parse_request: {}: {}",
                        llhttp_errno_name(err),
                        llhttp_get_error_reason(&parser)));
                }

                if (!parsed.complete) return Value{};

                parsed.method = llhttp_method_name(static_cast<llhttp_method_t>(parser.method));
                return Value{build_request_module(std::move(parsed))};
            })
        .build();

    interp.register_native_module("httpp", m);
}

}  // namespace bnl
