#include "native/builtins.h"

#include <fmt/core.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "interpreter.h"
#include "native_module.h"

namespace bnl {

namespace {

using nlohmann::json;

Value json_to_value(const json& j) {
    if (j.is_null())    return Value{};
    if (j.is_boolean()) return Value{j.get<bool>()};
    if (j.is_number())  return Value{j.get<double>()};
    if (j.is_string())  return Value{j.get<std::string>()};
    if (j.is_array()) {
        auto list = std::make_shared<std::vector<Value>>();
        list->reserve(j.size());
        for (const auto& el : j) list->push_back(json_to_value(el));
        return Value{list};
    }
    if (j.is_object()) {
        auto map = std::make_shared<std::unordered_map<std::string, Value>>();
        map->reserve(j.size());
        for (auto it = j.begin(); it != j.end(); ++it) {
            (*map)[it.key()] = json_to_value(it.value());
        }
        return Value{map};
    }
    return Value{};
}

void format_number(double n, std::string& out) {
    if (std::isnan(n) || std::isinf(n)) { out += "null"; return; }
    if (n == std::floor(n) && std::abs(n) < 1e16) {
        out += fmt::format("{:.0f}", n);
    } else {
        out += fmt::format("{}", n);
    }
}

void escape_string(const std::string& s, std::string& out) {
    out += '"';
    for (char c : s) {
        unsigned char uc = static_cast<unsigned char>(c);
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\t': out += "\\t";  break;
            case '\r': out += "\\r";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if (uc < 0x20) {
                    out += fmt::format("\\u{:04x}", static_cast<unsigned>(uc));
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

void value_to_json(const Value& v, std::string& out) {
    if (v.is_null())   { out += "null"; return; }
    if (v.is_bool())   { out += v.as_bool() ? "true" : "false"; return; }
    if (v.is_number()) { format_number(v.as_number(), out); return; }
    if (v.is_string()) { escape_string(v.as_string(), out); return; }
    if (v.is_list()) {
        out += '[';
        bool first = true;
        for (const auto& el : *v.as_list()) {
            if (!first) out += ',';
            value_to_json(el, out);
            first = false;
        }
        out += ']';
        return;
    }
    if (v.is_map()) {
        out += '{';
        bool first = true;
        for (const auto& [k, val] : *v.as_map()) {
            if (!first) out += ',';
            escape_string(k, out);
            out += ':';
            value_to_json(val, out);
            first = false;
        }
        out += '}';
        return;
    }
    // Functions, modules and other non-JSON types degrade to null.
    out += "null";
}

}  // namespace

void register_json(Interpreter& interp) {
    // parse(text) — returns the decoded value, or null and a stderr message on
    // syntax error. Non-throwing so user code can `if (got == null) {...}`.
    auto parse_fn = std::make_shared<NativeFunction>(
        "parse", 1,
        [](Interpreter&, std::vector<Value> args) -> Value {
            if (!args[0].is_string())
                throw std::runtime_error("json.parse(text): text must be a string");
            try {
                auto j = json::parse(args[0].as_string());
                return json_to_value(j);
            } catch (const json::parse_error& e) {
                fmt::print(stderr, "json.parse: {}\n", e.what());
                return Value{};
            }
        });

    // stringify(value) — compact serializer. Walks the bnl Value tree directly
    // so number formatting matches print() (integer doubles emit without ".0").
    auto stringify_fn = std::make_shared<NativeFunction>(
        "stringify", 1,
        [](Interpreter&, std::vector<Value> args) -> Value {
            std::string out;
            value_to_json(args[0], out);
            return Value{std::move(out)};
        });

    auto m = NativeModule("json")
        .add_value("parse",     Value{parse_fn})
        .add_value("stringify", Value{stringify_fn})
        // Bilingual aliases share the underlying Callable.
        .add_value("বিশ্লেষণ", Value{parse_fn})
        .add_value("রূপান্তর", Value{stringify_fn})
        .build();

    interp.register_native_module("json", m);
}

}  // namespace bnl
