#include "stdlib/registry.h"

#include "bnl/interpreter.h"
#include "bnl/native_module.h"

#include <cstddef>
#include <memory>
#include <regex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace bnl {

namespace {

// ---------- helpers ----------------------------------------------------------

// Translate the user's flag string into std::regex syntax flags. Default
// ECMAScript grammar (the same family as JS / Java / Python's `re`). Only
// the two most-asked flags are exposed for v1; more can be added later.
std::regex::flag_type parse_flags(const std::string& flags) {
    auto f = std::regex::ECMAScript;
    for (char c : flags) {
        switch (c) {
            case 'i': f |= std::regex::icase;     break;  // case-insensitive
            case 'm': f |= std::regex::multiline; break;  // ^ / $ match line edges
            default:
                throw std::runtime_error(
                    std::string("regex: unknown flag '") + c + "' (supported: i, m)");
        }
    }
    return f;
}

std::regex compile_regex(const std::string& pattern, const std::string& flags) {
    try {
        return std::regex(pattern, parse_flags(flags));
    } catch (const std::regex_error& e) {
        throw std::runtime_error(std::string("regex: invalid pattern: ") + e.what());
    }
}

const std::string& require_string(const Value& v, const char* where) {
    if (!v.is_string()) throw std::runtime_error(std::string(where) + ": expected string");
    return v.as_string();
}

// Optional flags arg: missing or null = no flags. Anything else must be a string.
std::string optional_flags(const std::vector<Value>& args, std::size_t i) {
    if (args.size() <= i || args[i].is_null()) return {};
    if (!args[i].is_string()) throw std::runtime_error("regex: flags must be a string");
    return args[i].as_string();
}

void check_arity(const std::vector<Value>& args, std::size_t min, std::size_t max,
                 const char* signature) {
    if (args.size() < min || args.size() > max)
        throw std::runtime_error(std::string("regex.") + signature + ": wrong arity");
}

// Build the {match, groups, index} map returned by match / match_all.
Value make_match_value(const std::smatch& m) {
    auto out = std::make_shared<std::unordered_map<std::string, Value>>();
    (*out)["match"] = Value{m.str(0)};

    auto groups = std::make_shared<std::vector<Value>>();
    groups->reserve(m.size() > 0 ? m.size() - 1 : 0);
    for (std::size_t i = 1; i < m.size(); ++i) {
        groups->push_back(m[i].matched ? Value{m.str(i)} : Value{});
    }
    (*out)["groups"] = Value{groups};
    (*out)["index"]  = Value{static_cast<double>(m.position(0))};
    return Value{out};
}

}  // namespace

void register_regex(Interpreter& interp) {
    auto m = NativeModule("regex")

        // regex.test(pattern, str, flags?) — true if pattern matches anywhere in str.
        .add_function("test", -1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                check_arity(args, 2, 3, "test(pattern, str, flags?)");
                auto re = compile_regex(require_string(args[0], "regex.test: pattern"),
                                        optional_flags(args, 2));
                return Value{std::regex_search(require_string(args[1], "regex.test: str"), re)};
            })

        // regex.match(pattern, str, flags?) — first match as a map, or null if none.
        // Map shape: { match: <whole match>, groups: [<g1>, <g2>, ...], index: <pos> }.
        // Unmatched groups are null.
        .add_function("match", -1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                check_arity(args, 2, 3, "match(pattern, str, flags?)");
                auto re = compile_regex(require_string(args[0], "regex.match: pattern"),
                                        optional_flags(args, 2));
                const auto& s = require_string(args[1], "regex.match: str");
                std::smatch m;
                if (!std::regex_search(s, m, re)) return Value{};
                return make_match_value(m);
            })

        // regex.match_all(pattern, str, flags?) — list of every match (each shaped
        // like regex.match's map). Empty list if there are no matches.
        .add_function("match_all", -1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                check_arity(args, 2, 3, "match_all(pattern, str, flags?)");
                auto re = compile_regex(require_string(args[0], "regex.match_all: pattern"),
                                        optional_flags(args, 2));
                const auto& s = require_string(args[1], "regex.match_all: str");
                auto out = std::make_shared<std::vector<Value>>();
                auto it  = std::sregex_iterator(s.begin(), s.end(), re);
                auto end = std::sregex_iterator();
                for (; it != end; ++it) out->push_back(make_match_value(*it));
                return Value{out};
            })

        // regex.replace(pattern, str, repl, flags?) — replace ALL matches with repl.
        // `repl` supports backreferences: $1..$9, $& (whole match), $$ (literal $).
        .add_function("replace", -1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                check_arity(args, 3, 4, "replace(pattern, str, repl, flags?)");
                auto re = compile_regex(require_string(args[0], "regex.replace: pattern"),
                                        optional_flags(args, 3));
                return Value{std::regex_replace(
                    require_string(args[1], "regex.replace: str"), re,
                    require_string(args[2], "regex.replace: repl"))};
            })

        // regex.replace_first(pattern, str, repl, flags?) — replace only the first match.
        .add_function("replace_first", -1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                check_arity(args, 3, 4, "replace_first(pattern, str, repl, flags?)");
                auto re = compile_regex(require_string(args[0], "regex.replace_first: pattern"),
                                        optional_flags(args, 3));
                return Value{std::regex_replace(
                    require_string(args[1], "regex.replace_first: str"), re,
                    require_string(args[2], "regex.replace_first: repl"),
                    std::regex_constants::format_first_only)};
            })

        // regex.split(pattern, str, flags?) — split str at each match. Adjacent
        // matches produce empty-string entries (consistent with most regex libs).
        .add_function("split", -1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                check_arity(args, 2, 3, "split(pattern, str, flags?)");
                auto re = compile_regex(require_string(args[0], "regex.split: pattern"),
                                        optional_flags(args, 2));
                const auto& s = require_string(args[1], "regex.split: str");
                auto out = std::make_shared<std::vector<Value>>();
                auto it  = std::sregex_token_iterator(s.begin(), s.end(), re, -1);
                auto end = std::sregex_token_iterator();
                for (; it != end; ++it) out->push_back(Value{it->str()});
                return Value{out};
            })

        // regex.escape(str) — escape every regex metacharacter in str so that
        // the result matches str literally when used as a pattern.
        .add_function("escape", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                const auto& s = require_string(args[0], "regex.escape: str");
                static const std::string meta = R"(\^$.|?*+()[]{})";
                std::string out;
                out.reserve(s.size());
                for (char c : s) {
                    if (meta.find(c) != std::string::npos) out += '\\';
                    out += c;
                }
                return Value{out};
            })

        .build();

    interp.register_native_module("regex", m);
}

}  // namespace bnl
