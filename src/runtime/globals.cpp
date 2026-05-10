#include "bnl/interpreter.h"
#include "runtime/internal.h"

#include "runtime/environment.h"

#include <fmt/core.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bnl {

namespace {

// ---------- pretty-printer for `pretty()` and `dump()` ----------------------

// Count UTF-8 codepoints. Used so truncation never splits a multi-byte char.
std::size_t utf8_codepoint_count(const std::string& s) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < s.size(); ) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if      (c < 0x80)             i += 1;
        else if ((c & 0xE0) == 0xC0)   i += 2;
        else if ((c & 0xF0) == 0xE0)   i += 3;
        else if ((c & 0xF8) == 0xF0)   i += 4;
        else                            i += 1;
        ++n;
    }
    return n;
}

constexpr int kPrettyWrapAt = 60;   // inline up to this many chars; multi-line beyond

// Format a Value pretty. Lists/maps are inline if they fit in kPrettyWrapAt
// chars (single line), else multi-line with 4-space indent. All scalars use
// to_repr() so strings show their quotes, distinguishing them from
// identifiers when nested inside a collection.
std::string format_pretty(const Value& v, std::size_t indent) {
    if (v.is_list()) {
        const auto& list = *v.as_list();
        if (list.empty()) return "[]";

        std::string single = "[";
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (i) single += ", ";
            single += format_pretty(list[i], 0);
        }
        single += ']';
        if (single.size() <= static_cast<std::size_t>(kPrettyWrapAt)
            && single.find('\n') == std::string::npos) {
            return single;
        }

        std::string in (indent + 4, ' ');
        std::string close(indent, ' ');
        std::string out  = "[\n";
        for (std::size_t i = 0; i < list.size(); ++i) {
            out += in;
            out += format_pretty(list[i], indent + 4);
            if (i + 1 < list.size()) out += ',';
            out += '\n';
        }
        out += close;
        out += ']';
        return out;
    }

    if (v.is_map()) {
        const auto& m = *v.as_map();
        if (m.empty()) return "{}";

        std::string single = "{";
        bool first = true;
        for (const auto& [k, val] : m) {
            if (!first) single += ", ";
            single += k;
            single += ": ";
            single += format_pretty(val, 0);
            first = false;
        }
        single += '}';
        if (single.size() <= static_cast<std::size_t>(kPrettyWrapAt)
            && single.find('\n') == std::string::npos) {
            return single;
        }

        std::string in (indent + 4, ' ');
        std::string close(indent, ' ');
        std::string out  = "{\n";
        std::size_t i = 0;
        for (const auto& [k, val] : m) {
            out += in;
            out += k;
            out += ": ";
            out += format_pretty(val, indent + 4);
            if (i + 1 < m.size()) out += ',';
            out += '\n';
            ++i;
        }
        out += close;
        out += '}';
        return out;
    }

    return v.to_repr();
}

// Truncate to `max` codepoints, appending "..." if anything was cut.
// max == 0 means no limit.
std::string truncate_codepoints(std::string s, std::size_t max) {
    if (max == 0) return s;
    std::size_t count = 0;
    std::size_t i     = 0;
    while (i < s.size() && count < max) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t step = 1;
        if      ((c & 0xE0) == 0xC0) step = 2;
        else if ((c & 0xF0) == 0xE0) step = 3;
        else if ((c & 0xF8) == 0xF0) step = 4;
        i += step;
        ++count;
    }
    if (i >= s.size()) return s;
    return s.substr(0, i) + "...";
}

// Read BNL_PRINT_LIMIT once at startup. Default 100 codepoints; "0" disables.
std::size_t default_print_limit() {
    const char* env = std::getenv("BNL_PRINT_LIMIT");
    if (!env || !*env) return 100;
    char* end = nullptr;
    long  n   = std::strtol(env, &end, 10);
    if (end == env || n < 0) return 100;
    return static_cast<std::size_t>(n);
}

}  // namespace


void Interpreter::register_builtins() {
    using interp_detail::is_ascii_space;

    // print(...): variadic; values displayed space-separated, then newline.
    auto print_fn = std::make_shared<NativeFunction>(
        "print", -1,
        [](Interpreter&, std::vector<Value> args) -> Value {
            for (std::size_t i = 0; i < args.size(); ++i) {
                if (i) fmt::print(" ");
                fmt::print("{}", args[i].to_display());
            }
            fmt::print("\n");
            return Value{};
        });

    // Both names point at the same Callable instance.
    globals_->define("print",  Value{print_fn});
    globals_->define("\xe0\xa6\xb2\xe0\xa6\xbf\xe0\xa6\x96\xe0\xa7\x81\xe0\xa6\xa8", Value{print_fn});  // লিখুন

    // str(x): convert any value to its display string.
    auto str_fn = std::make_shared<NativeFunction>(
        "str", 1,
        [](Interpreter&, std::vector<Value> args) -> Value {
            return Value{args[0].to_display()};
        });
    globals_->define("str", Value{str_fn});
    globals_->define("\xe0\xa6\xb2\xe0\xa7\x87\xe0\xa6\x96", Value{str_fn});  // লেখ

    // type(x): return the type name as a string.
    auto type_fn = std::make_shared<NativeFunction>(
        "type", 1,
        [](Interpreter&, std::vector<Value> args) -> Value {
            return Value{std::string(args[0].type_name())};
        });
    globals_->define("type", Value{type_fn});
    globals_->define("\xe0\xa6\xa7\xe0\xa6\xb0\xe0\xa6\xa3", Value{type_fn});  // ধরণ

    // to_number(s): parse a string to a number, returning null on failure.
    auto to_number_fn = std::make_shared<NativeFunction>(
        "to_number", 1,
        [](Interpreter&, std::vector<Value> args) -> Value {
            if (args[0].is_number()) return args[0];
            if (!args[0].is_string()) return Value{};
            const std::string& s = args[0].as_string();
            try {
                std::size_t consumed = 0;
                double n = std::stod(s, &consumed);
                // Reject if there's trailing non-whitespace.
                while (consumed < s.size() && is_ascii_space(s[consumed])) ++consumed;
                if (consumed != s.size()) return Value{};
                return Value{n};
            } catch (...) {
                return Value{};
            }
        });
    globals_->define("to_number", Value{to_number_fn});

    // chr(n): single-byte string from byte value 0..255.
    auto chr_fn = std::make_shared<NativeFunction>(
        "chr", 1,
        [](Interpreter&, std::vector<Value> args) -> Value {
            if (!args[0].is_number())
                throw std::runtime_error("chr(n): n must be a number");
            int n = static_cast<int>(args[0].as_number());
            if (n < 0 || n > 255)
                throw std::runtime_error("chr(n): n must be in 0..255");
            return Value{std::string(1, static_cast<char>(n))};
        });
    globals_->define("chr", Value{chr_fn});

    // try_call(thunk, on_err): call thunk() with no args; if it throws, call
    // on_err(message_string). Returns thunk()'s return value, or on_err's
    // return value when the thunk threw. bnl has no try/catch syntax — this
    // is the escape hatch for code (like web.serve) that must turn handler
    // errors into responses instead of letting them crash the loop.
    auto try_call_fn = std::make_shared<NativeFunction>(
        "try_call", 2,
        [](Interpreter& interp, std::vector<Value> args) -> Value {
            if (!args[0].is_callable())
                throw std::runtime_error("try_call: first arg must be a function");
            if (!args[1].is_callable())
                throw std::runtime_error("try_call: second arg must be a function");
            try {
                return args[0].as_callable()->call(interp, {});
            } catch (ThrowSignal& sig) {
                return args[1].as_callable()->call(interp, { std::move(sig.value) });
            } catch (const RuntimeError& e) {
                return args[1].as_callable()->call(interp, { Value{std::string(e.what())} });
            } catch (const std::exception& e) {
                return args[1].as_callable()->call(interp, { Value{std::string(e.what())} });
            }
        });
    globals_->define("try_call", Value{try_call_fn});

    // pretty(x): pretty-formatted display string. Lists/maps are inline if
    // short, multi-line indented otherwise. Strings show their quotes
    // (distinguishing "x" from x as an identifier when nested). Truncates
    // at BNL_PRINT_LIMIT codepoints (default 100) and appends "..." if cut.
    // Use as `print(pretty(big));`. For full no-truncate output, use dump.
    auto pretty_fn = std::make_shared<NativeFunction>(
        "pretty", 1,
        [limit = default_print_limit()](Interpreter&, std::vector<Value> args) -> Value {
            std::string s = format_pretty(args[0], 0);
            return Value{truncate_codepoints(std::move(s), limit)};
        });
    globals_->define("pretty", Value{pretty_fn});

    // dump(x): same pretty format as `pretty()` but no truncation. Useful
    // when you need to see ALL of a deep value during debugging.
    auto dump_fn = std::make_shared<NativeFunction>(
        "dump", 1,
        [](Interpreter&, std::vector<Value> args) -> Value {
            return Value{format_pretty(args[0], 0)};
        });
    globals_->define("dump", Value{dump_fn});
    (void)utf8_codepoint_count;  // reserved for future use

    // input(prompt?): write the optional prompt to stdout (no newline), then
    // read one line from stdin. Returns the line WITHOUT its trailing newline.
    // Returns null on EOF (Ctrl-Z + Enter on Windows, Ctrl-D on POSIX) so
    // scripts can do `if (x == null) { ... }` to detect end-of-input.
    auto input_fn = std::make_shared<NativeFunction>(
        "input", -1,
        [](Interpreter&, std::vector<Value> args) -> Value {
            if (args.size() > 1)
                throw std::runtime_error("input(prompt?): too many arguments");
            if (!args.empty()) {
                if (!args[0].is_string())
                    throw std::runtime_error("input(prompt?): prompt must be a string");
                fmt::print("{}", args[0].as_string());
                std::fflush(stdout);   // fmt::print doesn't auto-flush stdout
            }
            std::string line;
            if (!std::getline(std::cin, line)) return Value{};
            // Some shells / piped input leave a stray \r before \n. Strip it
            // so users don't have to .trim() every result on Windows.
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return Value{std::move(line)};
        });
    globals_->define("input", Value{input_fn});
    // ইনপুট — Bangla transliteration; same callable.
    globals_->define("\xe0\xa6\x87\xe0\xa6\xa8\xe0\xa6\xaa\xe0\xa7\x81\xe0\xa6\x9f",
                     Value{input_fn});
}

}  // namespace bnl
