#include "stdlib/registry.h"

#include <fmt/core.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
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

// ---------- nlohmann::json -> bnl::Value -----------------------------------

Value json_to_value(const nlohmann::json& j) {
    using nlohmann::json;
    switch (j.type()) {
        case json::value_t::null:            return Value{};
        case json::value_t::boolean:         return Value{j.get<bool>()};
        case json::value_t::number_integer:  return Value{static_cast<double>(j.get<long long>())};
        case json::value_t::number_unsigned: return Value{static_cast<double>(j.get<unsigned long long>())};
        case json::value_t::number_float:    return Value{j.get<double>()};
        case json::value_t::string:          return Value{j.get<std::string>()};
        case json::value_t::array: {
            auto out = std::make_shared<std::vector<Value>>();
            out->reserve(j.size());
            for (const auto& el : j) out->push_back(json_to_value(el));
            return Value{out};
        }
        case json::value_t::object: {
            auto out = std::make_shared<std::unordered_map<std::string, Value>>();
            for (auto it = j.begin(); it != j.end(); ++it) {
                (*out)[it.key()] = json_to_value(it.value());
            }
            return Value{out};
        }
        default:
            // binary / discarded — not produced by parse from text JSON.
            return Value{};
    }
}

// ---------- bnl::Value -> JSON text ----------------------------------------

// Match `print()` for numbers: integer-valued doubles emit without `.0`.
// JSON has no NaN / Infinity, so coerce to null (Node's JSON.stringify
// does the same).
std::string format_number(double n) {
    if (!std::isfinite(n)) return "null";
    double i;
    if (std::modf(n, &i) == 0.0 && std::abs(n) < 1e16) {
        return std::to_string(static_cast<long long>(n));
    }
    return fmt::format("{}", n);
}

std::string escape_string(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (char c : s) {
        unsigned char uc = static_cast<unsigned char>(c);
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (uc < 0x20) {
                    out += fmt::format("\\u{:04x}", static_cast<unsigned>(uc));
                } else {
                    out += c;
                }
        }
    }
    out += '"';
    return out;
}

void append_indent(std::string& out, int indent, int depth) {
    if (indent > 0) {
        out += '\n';
        out.append(static_cast<std::size_t>(indent) * static_cast<std::size_t>(depth), ' ');
    }
}

void stringify_value(const Value& v, std::string& out, int indent, int depth);

void stringify_list(const std::vector<Value>& list, std::string& out,
                    int indent, int depth) {
    if (list.empty()) { out += "[]"; return; }
    out += '[';
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (i > 0) out += ',';
        append_indent(out, indent, depth + 1);
        stringify_value(list[i], out, indent, depth + 1);
    }
    append_indent(out, indent, depth);
    out += ']';
}

void stringify_map(const std::unordered_map<std::string, Value>& map,
                   std::string& out, int indent, int depth) {
    if (map.empty()) { out += "{}"; return; }

    // Sort keys so output is deterministic (and diff-friendly). JSON object
    // member order isn't significant per spec, so sorting is safe.
    std::vector<std::string> keys;
    keys.reserve(map.size());
    for (const auto& [k, _] : map) keys.push_back(k);
    std::sort(keys.begin(), keys.end());

    out += '{';
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (i > 0) out += ',';
        append_indent(out, indent, depth + 1);
        out += escape_string(keys[i]);
        out += indent > 0 ? ": " : ":";
        stringify_value(map.at(keys[i]), out, indent, depth + 1);
    }
    append_indent(out, indent, depth);
    out += '}';
}

void stringify_value(const Value& v, std::string& out, int indent, int depth) {
    if (v.is_null())   { out += "null";                                          return; }
    if (v.is_bool())   { out += v.as_bool() ? "true" : "false";                  return; }
    if (v.is_number()) { out += format_number(v.as_number());                    return; }
    if (v.is_string()) { out += escape_string(v.as_string());                    return; }
    if (v.is_list())   { stringify_list(*v.as_list(), out, indent, depth);       return; }
    if (v.is_map())    { stringify_map (*v.as_map(),  out, indent, depth);       return; }
    throw std::runtime_error(std::string("json.stringify: cannot serialize ") +
                             v.type_name());
}

}  // namespace

void register_json(Interpreter& interp) {
    auto m = NativeModule("json")

        // json.parse(s) — parse a JSON string into a bnl value (null / bool /
        // number / string / list / map). On a malformed input, prints an
        // error to stderr and returns null. Returning null instead of
        // throwing preserves the legacy contract: `if (got == null) {...}`.
        //
        // We catch std::exception (every nlohmann exception derives from it)
        // plus a bare catch — on Windows, catching the nested nlohmann type
        // by its specific name doesn't always match across the vcpkg/MSVC
        // boundary, so the std::exception base is the reliable funnel.
        .add_function("parse", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                if (!args[0].is_string())
                    throw std::runtime_error("json.parse: expected string");
                try {
                    auto j = nlohmann::json::parse(args[0].as_string());
                    return json_to_value(j);
                } catch (const std::exception& e) {
                    fmt::print(stderr, "json.parse: {}\n", e.what());
                    return Value{};
                } catch (...) {
                    fmt::print(stderr, "json.parse: unknown error\n");
                    return Value{};
                }
            })

        // json.stringify(value, indent?) — serialize a bnl value to JSON text.
        // indent is the per-level space count (0 = single-line compact, the
        // default; clamped to [0, 16]). Object keys are sorted for stable
        // output. Integer-valued numbers emit without `.0` (matches print).
        // Throws if value (or anything reachable from it) is a callable /
        // module / instance, since those have no JSON shape.
        .add_function("stringify", -1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                if (args.empty() || args.size() > 2)
                    throw std::runtime_error(
                        "json.stringify(value, indent?): wrong arity");
                int indent = 0;
                if (args.size() == 2 && !args[1].is_null()) {
                    if (!args[1].is_number())
                        throw std::runtime_error(
                            "json.stringify: indent must be a number");
                    indent = static_cast<int>(args[1].as_number());
                    if (indent < 0)  indent = 0;
                    if (indent > 16) indent = 16;
                }
                std::string out;
                stringify_value(args[0], out, indent, 0);
                return Value{std::move(out)};
            })

        .build();

    interp.register_native_module("json", m);
}

}  // namespace bnl
