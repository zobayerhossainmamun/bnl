#include "interpreter.h"
#include "interpreter/internal.h"

#include <fmt/core.h>

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bnl {

using interp_detail::decode_string_literal;
using interp_detail::is_ascii_space;

namespace {

// ---- operator helpers -------------------------------------------------------

void check_number_operand(const Token& op, const Value& v) {
    if (!v.is_number()) {
        throw RuntimeError(op, fmt::format("operand of '{}' must be a number, got {}",
                                           op.lexeme, v.type_name()));
    }
}

void check_number_operands(const Token& op, const Value& a, const Value& b) {
    if (!a.is_number() || !b.is_number()) {
        throw RuntimeError(op, fmt::format("operands of '{}' must be numbers (got {} and {})",
                                           op.lexeme, a.type_name(), b.type_name()));
    }
}

// ----- UTF-8 helpers --------------------------------------------------------
//
// bnl strings are UTF-8 byte sequences; codepoint indices (used by .length,
// .char_at, .slice, .index_of, .split) need translation to byte offsets.

std::size_t utf8_step(unsigned char b) {
    if (b < 0x80)        return 1;
    if ((b & 0xE0) == 0xC0) return 2;
    if ((b & 0xF0) == 0xE0) return 3;
    if ((b & 0xF8) == 0xF0) return 4;
    return 1;
}

std::size_t utf8_codepoint_count(const std::string& s) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < s.size();) {
        i += utf8_step(static_cast<unsigned char>(s[i]));
        ++count;
    }
    return count;
}

std::size_t utf8_byte_offset(const std::string& s, long long cp_index) {
    if (cp_index <= 0) return 0;
    std::size_t byte = 0;
    for (long long i = 0; i < cp_index && byte < s.size(); ++i) {
        byte += utf8_step(static_cast<unsigned char>(s[byte]));
    }
    return byte;
}

long long utf8_codepoint_at_byte(const std::string& s, std::size_t byte_target) {
    long long cp = 0;
    std::size_t byte = 0;
    while (byte < byte_target && byte < s.size()) {
        byte += utf8_step(static_cast<unsigned char>(s[byte]));
        cp++;
    }
    return cp;
}

// Wraps a C++ lambda as a Callable bound to a particular receiver. Used to
// produce list/map/string intrinsic methods like .push, .keys, .slice, etc.
CallablePtr make_bound_native(std::string name, int arity, NativeFunction::Fn fn) {
    auto callable = std::make_shared<NativeFunction>(std::move(name), arity, std::move(fn));
    return std::static_pointer_cast<Callable>(callable);
}

}  // namespace

// ---- expressions -----------------------------------------------------------

void Interpreter::visit(LiteralExpr& e) {
    switch (e.value.type) {
        case TokenType::Null:   result_ = Value{};                  return;
        case TokenType::True:   result_ = Value{true};              return;
        case TokenType::False:  result_ = Value{false};             return;
        case TokenType::Number: {
            try {
                result_ = Value{std::stod(std::string(e.value.lexeme))};
            } catch (const std::exception&) {
                throw RuntimeError(e.value, "invalid numeric literal");
            }
            return;
        }
        case TokenType::String: result_ = Value{decode_string_literal(e.value)}; return;
        default:
            throw RuntimeError(e.value, "internal: unexpected literal token type");
    }
}

void Interpreter::visit(IdentifierExpr& e) {
    const Value* v = environment_->lookup(std::string(e.name.lexeme));
    if (!v) throw RuntimeError(e.name, "undefined variable '" + std::string(e.name.lexeme) + "'");
    result_ = *v;
}

void Interpreter::visit(GroupingExpr& e) { result_ = evaluate(*e.inner); }

void Interpreter::visit(UnaryExpr& e) {
    Value operand = evaluate(*e.operand);
    switch (e.op.type) {
        case TokenType::Minus:
            check_number_operand(e.op, operand);
            result_ = Value{-operand.as_number()};
            return;
        case TokenType::Bang:
        case TokenType::Not:
            result_ = Value{!operand.truthy()};
            return;
        default:
            throw RuntimeError(e.op, "unknown unary operator");
    }
}

void Interpreter::visit(BinaryExpr& e) {
    Value left  = evaluate(*e.left);
    Value right = evaluate(*e.right);

    switch (e.op.type) {
        case TokenType::Plus:
            if (left.is_number() && right.is_number()) {
                result_ = Value{left.as_number() + right.as_number()};
                return;
            }
            // String concat: if either side is a string, both get displayed.
            if (left.is_string() || right.is_string()) {
                result_ = Value{left.to_display() + right.to_display()};
                return;
            }
            throw RuntimeError(e.op, fmt::format("'+' expects numbers or strings, got {} and {}",
                                                 left.type_name(), right.type_name()));

        case TokenType::Minus:   check_number_operands(e.op, left, right); result_ = Value{left.as_number() - right.as_number()}; return;
        case TokenType::Star:    check_number_operands(e.op, left, right); result_ = Value{left.as_number() * right.as_number()}; return;
        case TokenType::Percent: check_number_operands(e.op, left, right); result_ = Value{std::fmod(left.as_number(), right.as_number())}; return;

        case TokenType::Slash:
            check_number_operands(e.op, left, right);
            if (right.as_number() == 0.0) throw RuntimeError(e.op, "division by zero");
            result_ = Value{left.as_number() / right.as_number()};
            return;

        case TokenType::Lt:  check_number_operands(e.op, left, right); result_ = Value{left.as_number() <  right.as_number()}; return;
        case TokenType::Gt:  check_number_operands(e.op, left, right); result_ = Value{left.as_number() >  right.as_number()}; return;
        case TokenType::LtEq: check_number_operands(e.op, left, right); result_ = Value{left.as_number() <= right.as_number()}; return;
        case TokenType::GtEq: check_number_operands(e.op, left, right); result_ = Value{left.as_number() >= right.as_number()}; return;

        case TokenType::EqEq:   result_ = Value{ left.equals(right)}; return;
        case TokenType::BangEq: result_ = Value{!left.equals(right)}; return;

        default:
            throw RuntimeError(e.op, "unknown binary operator");
    }
}

void Interpreter::visit(LogicalExpr& e) {
    Value left = evaluate(*e.left);
    bool is_or = (e.op.type == TokenType::PipePipe || e.op.type == TokenType::Or);
    if (is_or) {
        if (left.truthy()) { result_ = std::move(left); return; }
    } else {
        if (!left.truthy()) { result_ = std::move(left); return; }
    }
    result_ = evaluate(*e.right);
}

void Interpreter::visit(AssignExpr& e) {
    Value value = evaluate(*e.value);
    if (!environment_->assign(std::string(e.name.lexeme), value)) {
        throw RuntimeError(e.name, "undefined variable '" + std::string(e.name.lexeme) + "'");
    }
    result_ = std::move(value);
}

void Interpreter::visit(CallExpr& e) {
    Value callee = evaluate(*e.callee);
    if (!callee.is_callable()) {
        throw RuntimeError(e.paren,
            fmt::format("only functions are callable, got {}", callee.type_name()));
    }
    std::vector<Value> args;
    args.reserve(e.arguments.size());
    for (auto& a : e.arguments) args.push_back(evaluate(*a));

    auto fn = callee.as_callable();
    if (fn->arity() >= 0 && static_cast<int>(args.size()) != fn->arity()) {
        throw RuntimeError(e.paren,
            fmt::format("function '{}' expects {} arguments, got {}",
                        fn->name(), fn->arity(), args.size()));
    }
    // Native functions surface failures as std::runtime_error; attach the
    // call site's token so the user sees a useful line/column.
    try {
        result_ = fn->call(*this, std::move(args));
    } catch (const RuntimeError&) {
        throw;
    } catch (const ReturnSignal&) {
        throw;
    } catch (const std::exception& ex) {
        throw RuntimeError(e.paren, ex.what());
    }
}

void Interpreter::visit(MemberExpr& e) {
    Value obj  = evaluate(*e.object);
    auto  name = std::string(e.name.lexeme);

    // Strings: intrinsic properties + bound methods (codepoint-aware).
    if (obj.is_string()) {
        const std::string& s = obj.as_string();

        if (name == "length")      { result_ = Value{static_cast<double>(utf8_codepoint_count(s))}; return; }
        if (name == "byte_length") { result_ = Value{static_cast<double>(s.size())};                return; }

        // Capture the string by value so the method outlives this Value.
        std::string captured = s;

        if (name == "slice") {
            result_ = Value{make_bound_native("slice", -1,
                [captured](Interpreter&, std::vector<Value> args) -> Value {
                    if (args.empty() || args.size() > 2)
                        throw std::runtime_error("string.slice expects 1 or 2 arguments");
                    long long len = static_cast<long long>(utf8_codepoint_count(captured));
                    auto clamp = [&](long long n) {
                        if (n < 0) n = 0;
                        if (n > len) n = len;
                        return n;
                    };
                    long long start = clamp(static_cast<long long>(args[0].as_number()));
                    long long end   = clamp(args.size() == 2
                        ? static_cast<long long>(args[1].as_number()) : len);
                    if (end < start) end = start;
                    std::size_t b0 = utf8_byte_offset(captured, start);
                    std::size_t b1 = utf8_byte_offset(captured, end);
                    return Value{captured.substr(b0, b1 - b0)};
                })};
            return;
        }
        if (name == "char_at") {
            result_ = Value{make_bound_native("char_at", 1,
                [captured](Interpreter&, std::vector<Value> args) -> Value {
                    long long i   = static_cast<long long>(args[0].as_number());
                    long long len = static_cast<long long>(utf8_codepoint_count(captured));
                    if (i < 0 || i >= len) return Value{std::string{}};
                    std::size_t b0 = utf8_byte_offset(captured, i);
                    std::size_t b1 = utf8_byte_offset(captured, i + 1);
                    return Value{captured.substr(b0, b1 - b0)};
                })};
            return;
        }
        if (name == "index_of") {
            result_ = Value{make_bound_native("index_of", -1,
                [captured](Interpreter&, std::vector<Value> args) -> Value {
                    if (args.empty() || args.size() > 2)
                        throw std::runtime_error("string.index_of expects 1 or 2 arguments");
                    if (!args[0].is_string())
                        throw std::runtime_error("string.index_of: needle must be a string");
                    const std::string& needle = args[0].as_string();
                    long long start_cp = (args.size() == 2)
                        ? static_cast<long long>(args[1].as_number()) : 0;
                    if (start_cp < 0) start_cp = 0;
                    std::size_t start_byte = utf8_byte_offset(captured, start_cp);
                    auto pos = captured.find(needle, start_byte);
                    if (pos == std::string::npos) return Value{static_cast<double>(-1)};
                    return Value{static_cast<double>(utf8_codepoint_at_byte(captured, pos))};
                })};
            return;
        }
        if (name == "starts_with") {
            result_ = Value{make_bound_native("starts_with", 1,
                [captured](Interpreter&, std::vector<Value> args) -> Value {
                    if (!args[0].is_string())
                        throw std::runtime_error("string.starts_with: prefix must be a string");
                    const std::string& p = args[0].as_string();
                    return Value{captured.size() >= p.size() &&
                                 captured.compare(0, p.size(), p) == 0};
                })};
            return;
        }
        if (name == "ends_with") {
            result_ = Value{make_bound_native("ends_with", 1,
                [captured](Interpreter&, std::vector<Value> args) -> Value {
                    if (!args[0].is_string())
                        throw std::runtime_error("string.ends_with: suffix must be a string");
                    const std::string& p = args[0].as_string();
                    return Value{captured.size() >= p.size() &&
                                 captured.compare(captured.size() - p.size(), p.size(), p) == 0};
                })};
            return;
        }
        if (name == "split") {
            result_ = Value{make_bound_native("split", 1,
                [captured](Interpreter&, std::vector<Value> args) -> Value {
                    if (!args[0].is_string())
                        throw std::runtime_error("string.split: separator must be a string");
                    const std::string& sep = args[0].as_string();
                    auto out = std::make_shared<std::vector<Value>>();
                    if (sep.empty()) {
                        // Split into individual codepoints.
                        for (std::size_t b = 0; b < captured.size();) {
                            std::size_t step = utf8_step(static_cast<unsigned char>(captured[b]));
                            out->emplace_back(captured.substr(b, step));
                            b += step;
                        }
                        return Value{out};
                    }
                    std::size_t start = 0;
                    while (true) {
                        auto pos = captured.find(sep, start);
                        if (pos == std::string::npos) {
                            out->emplace_back(captured.substr(start));
                            break;
                        }
                        out->emplace_back(captured.substr(start, pos - start));
                        start = pos + sep.size();
                    }
                    return Value{out};
                })};
            return;
        }
        if (name == "trim") {
            result_ = Value{make_bound_native("trim", 0,
                [captured](Interpreter&, std::vector<Value>) -> Value {
                    std::size_t a = 0, b = captured.size();
                    while (a < b && is_ascii_space(captured[a]))     ++a;
                    while (b > a && is_ascii_space(captured[b - 1])) --b;
                    return Value{captured.substr(a, b - a)};
                })};
            return;
        }
        if (name == "to_lower") {
            result_ = Value{make_bound_native("to_lower", 0,
                [captured](Interpreter&, std::vector<Value>) -> Value {
                    std::string out = captured;
                    for (auto& c : out)
                        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                    return Value{std::move(out)};
                })};
            return;
        }
        if (name == "to_upper") {
            result_ = Value{make_bound_native("to_upper", 0,
                [captured](Interpreter&, std::vector<Value>) -> Value {
                    std::string out = captured;
                    for (auto& c : out)
                        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
                    return Value{std::move(out)};
                })};
            return;
        }
        if (name == "replace") {
            result_ = Value{make_bound_native("replace", 2,
                [captured](Interpreter&, std::vector<Value> args) -> Value {
                    if (!args[0].is_string() || !args[1].is_string())
                        throw std::runtime_error("string.replace: both args must be strings");
                    const std::string& needle = args[0].as_string();
                    const std::string& with   = args[1].as_string();
                    if (needle.empty()) return Value{captured};
                    std::string out;
                    out.reserve(captured.size());
                    std::size_t start = 0;
                    while (true) {
                        auto pos = captured.find(needle, start);
                        if (pos == std::string::npos) { out.append(captured, start, std::string::npos); break; }
                        out.append(captured, start, pos - start);
                        out.append(with);
                        start = pos + needle.size();
                    }
                    return Value{std::move(out)};
                })};
            return;
        }

        throw RuntimeError(e.name, fmt::format("string has no property '{}'", name));
    }

    // Modules: name lookup in the module's exports environment.
    if (obj.is_module()) {
        const auto* v = obj.as_module()->exports()->lookup(name);
        if (!v) throw RuntimeError(e.name,
            fmt::format("module '{}' has no export '{}'", obj.as_module()->path(), name));
        result_ = *v;
        return;
    }

    // Lists: .length plus a few intrinsic methods returning bound callables.
    if (obj.is_list()) {
        ListPtr list = obj.as_list();
        if (name == "length") { result_ = Value{static_cast<double>(list->size())}; return; }
        if (name == "push") {
            result_ = Value{make_bound_native("push", 1,
                [list](Interpreter&, std::vector<Value> args) -> Value {
                    list->push_back(std::move(args[0]));
                    return Value{static_cast<double>(list->size())};
                })};
            return;
        }
        if (name == "pop") {
            result_ = Value{make_bound_native("pop", 0,
                [list](Interpreter&, std::vector<Value>) -> Value {
                    if (list->empty()) return Value{};
                    Value back = std::move(list->back());
                    list->pop_back();
                    return back;
                })};
            return;
        }
        throw RuntimeError(e.name, fmt::format("list has no property '{}'", name));
    }

    // Maps: dot lookup is sugar for string-key index. Plus a few intrinsics.
    if (obj.is_map()) {
        MapPtr map = obj.as_map();
        if (name == "size") { result_ = Value{static_cast<double>(map->size())}; return; }
        if (name == "has") {
            result_ = Value{make_bound_native("has", 1,
                [map](Interpreter&, std::vector<Value> args) -> Value {
                    if (!args[0].is_string())
                        throw std::runtime_error("map.has(key): key must be a string");
                    return Value{map->find(args[0].as_string()) != map->end()};
                })};
            return;
        }
        if (name == "keys") {
            result_ = Value{make_bound_native("keys", 0,
                [map](Interpreter&, std::vector<Value>) -> Value {
                    auto out = std::make_shared<std::vector<Value>>();
                    out->reserve(map->size());
                    for (const auto& [k, _] : *map) out->emplace_back(k);
                    return Value{out};
                })};
            return;
        }
        // Otherwise: look up the literal key. Returns null if absent.
        auto it = map->find(name);
        if (it != map->end()) { result_ = it->second; return; }
        result_ = Value{};
        return;
    }

    throw RuntimeError(e.name,
        fmt::format("{} value has no property '{}'", obj.type_name(), name));
}

void Interpreter::visit(FunctionExpr& e) {
    std::string name = e.name.empty() ? std::string("<anonymous>") : e.name;
    auto fn = std::make_shared<interp_detail::UserFunction>(
        std::move(name), &e.params, &e.body, environment_);
    result_ = Value{std::static_pointer_cast<Callable>(fn)};
}

void Interpreter::visit(ListExpr& e) {
    auto list = std::make_shared<std::vector<Value>>();
    list->reserve(e.elements.size());
    for (auto& el : e.elements) list->push_back(evaluate(*el));
    result_ = Value{list};
}

void Interpreter::visit(MapExpr& e) {
    auto map = std::make_shared<std::unordered_map<std::string, Value>>();
    map->reserve(e.entries.size());
    for (auto& [k, v] : e.entries) (*map)[k] = evaluate(*v);
    result_ = Value{map};
}

void Interpreter::visit(IndexExpr& e) {
    Value obj   = evaluate(*e.object);
    Value index = evaluate(*e.index);

    if (obj.is_list()) {
        if (!index.is_number())
            throw RuntimeError(e.bracket,
                fmt::format("list index must be a number, got {}", index.type_name()));
        const auto& list = *obj.as_list();
        auto i = static_cast<long long>(index.as_number());
        if (i < 0 || static_cast<std::size_t>(i) >= list.size())
            throw RuntimeError(e.bracket,
                fmt::format("list index {} out of range (size {})", i, list.size()));
        result_ = list[static_cast<std::size_t>(i)];
        return;
    }
    if (obj.is_map()) {
        if (!index.is_string())
            throw RuntimeError(e.bracket,
                fmt::format("map key must be a string, got {}", index.type_name()));
        const auto& map = *obj.as_map();
        auto it = map.find(index.as_string());
        result_ = (it != map.end()) ? it->second : Value{};
        return;
    }
    throw RuntimeError(e.bracket,
        fmt::format("cannot index {} value", obj.type_name()));
}

void Interpreter::visit(SetIndexExpr& e) {
    Value obj   = evaluate(*e.object);
    Value index = evaluate(*e.index);
    Value value = evaluate(*e.value);

    if (obj.is_list()) {
        if (!index.is_number())
            throw RuntimeError(e.bracket,
                fmt::format("list index must be a number, got {}", index.type_name()));
        auto& list = *obj.as_list();
        auto i = static_cast<long long>(index.as_number());
        if (i < 0 || static_cast<std::size_t>(i) >= list.size())
            throw RuntimeError(e.bracket,
                fmt::format("list index {} out of range (size {})", i, list.size()));
        list[static_cast<std::size_t>(i)] = value;
        result_ = std::move(value);
        return;
    }
    if (obj.is_map()) {
        if (!index.is_string())
            throw RuntimeError(e.bracket,
                fmt::format("map key must be a string, got {}", index.type_name()));
        (*obj.as_map())[index.as_string()] = value;
        result_ = std::move(value);
        return;
    }
    throw RuntimeError(e.bracket,
        fmt::format("cannot assign by index on {} value", obj.type_name()));
}

void Interpreter::visit(SetMemberExpr& e) {
    Value obj   = evaluate(*e.object);
    Value value = evaluate(*e.value);

    if (obj.is_map()) {
        (*obj.as_map())[std::string(e.name.lexeme)] = value;
        result_ = std::move(value);
        return;
    }
    throw RuntimeError(e.name,
        fmt::format("cannot set property '{}' on {} value",
                    std::string(e.name.lexeme), obj.type_name()));
}

}  // namespace bnl
