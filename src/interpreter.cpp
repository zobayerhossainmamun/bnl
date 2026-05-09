#include "interpreter.h"

#include <fmt/core.h>

#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>

#include "native/builtins.h"

namespace bnl {

namespace {

// ---- string literal escape decoding -----------------------------------------

std::string decode_string_literal(const Token& tok) {
    // tok.lexeme includes the surrounding quotes.
    auto sv = tok.lexeme;
    if (sv.size() < 2 || sv.front() != '"' || sv.back() != '"') {
        throw RuntimeError(tok, "internal: malformed string literal");
    }
    std::string out;
    out.reserve(sv.size() - 2);
    for (std::size_t i = 1; i + 1 < sv.size(); ++i) {
        char c = sv[i];
        if (c != '\\') { out += c; continue; }
        if (i + 2 >= sv.size()) {
            throw RuntimeError(tok, "trailing backslash in string literal");
        }
        char esc = sv[++i];
        switch (esc) {
            case 'n':  out += '\n'; break;
            case 't':  out += '\t'; break;
            case 'r':  out += '\r'; break;
            case '\\': out += '\\'; break;
            case '"':  out += '"';  break;
            case '0':  out += '\0'; break;
            default:
                throw RuntimeError(tok, "unknown escape '\\" + std::string(1, esc) + "'");
        }
    }
    return out;
}

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

// ---- user-defined function --------------------------------------------------

// Backs both `function` declarations and anonymous `function (...) {...}`
// expressions. Holds raw pointers into the AST nodes that own the params
// and body vectors (the AST outlives every closure that references it).
class UserFunction : public Callable {
public:
    UserFunction(std::string                       name,
                 const std::vector<Token>*         params,
                 const std::vector<StmtPtr>*       body,
                 std::shared_ptr<Environment>      closure)
        : name_(std::move(name)), params_(params), body_(body),
          closure_(std::move(closure)) {}

    int         arity() const override { return static_cast<int>(params_->size()); }
    std::string name()  const override { return name_; }

    Value call(Interpreter& interp, std::vector<Value> args) override {
        auto env = std::make_shared<Environment>(closure_);
        for (std::size_t i = 0; i < params_->size(); ++i) {
            env->define(std::string((*params_)[i].lexeme), std::move(args[i]));
        }
        try {
            interp.execute_block(*body_, env);
        } catch (ReturnSignal& r) {
            return std::move(r.value);
        }
        return Value{};  // implicit null
    }

private:
    std::string                  name_;
    const std::vector<Token>*    params_;
    const std::vector<StmtPtr>*  body_;
    std::shared_ptr<Environment> closure_;
};

}  // namespace

// ============================================================================
// Interpreter
// ============================================================================

Interpreter::Interpreter()
    : globals_(std::make_shared<Environment>()),
      environment_(globals_),
      modules_(*this) {
    uv_loop_init(&loop_);
    register_builtins();
    register_sys   (*this);
    register_io    (*this);
    register_timers(*this);
    register_crypto(*this);
    register_zlib  (*this);
    register_httpp (*this);
    register_net   (*this);
    register_json  (*this);
}

Interpreter::~Interpreter() {
    // Force-close any handles that survived (e.g. user crashed mid-loop).
    uv_walk(&loop_, [](uv_handle_t* h, void*) {
        if (!uv_is_closing(h)) uv_close(h, nullptr);
    }, nullptr);
    uv_run(&loop_, UV_RUN_DEFAULT);  // drain close callbacks
    uv_loop_close(&loop_);
}

void Interpreter::register_native_module(const std::string& name, ModulePtr m) {
    native_modules_[name] = std::move(m);
}

ModulePtr Interpreter::native_module(const std::string& name) const {
    auto it = native_modules_.find(name);
    return it != native_modules_.end() ? it->second : nullptr;
}

void Interpreter::mark_loop_failed() {
    loop_failed_ = true;
    uv_stop(&loop_);
}

bool Interpreter::run(const std::vector<StmtPtr>& program,
                      const std::filesystem::path& entry_path) {
    current_file_ = entry_path;
    try {
        for (const auto& s : program) execute(*s);
    } catch (const RuntimeError& e) {
        fmt::print(stderr, "runtime error at {}:{} (near '{}'): {}\n",
                   e.token.line, e.token.column, e.token.lexeme, e.what());
        return false;
    } catch (const ModuleError& e) {
        fmt::print(stderr, "module error at {}:{} (near '{}'): {}\n",
                   e.token.line, e.token.column, e.token.lexeme, e.what());
        return false;
    }

    // Drain the event loop. Async callbacks (timers, fs, net, ...) run here.
    // If any of them sets loop_failed_, the program exits non-zero.
    uv_run(&loop_, UV_RUN_DEFAULT);
    return !loop_failed_;
}

void Interpreter::run_module(Module& m) {
    auto previous_file = current_file_;
    current_file_      = m.path();
    try {
        execute_block(m.program(), m.exports());
    } catch (...) {
        current_file_ = previous_file;
        throw;
    }
    current_file_ = previous_file;
}

Value Interpreter::evaluate(Expr& e) {
    e.accept(*this);
    return std::move(result_);
}

void Interpreter::execute(Stmt& s) { s.accept(*this); }

void Interpreter::execute_block(const std::vector<StmtPtr>& stmts,
                                std::shared_ptr<Environment> env) {
    auto previous = environment_;
    environment_ = std::move(env);
    try {
        for (const auto& s : stmts) execute(*s);
    } catch (...) {
        environment_ = previous;
        throw;
    }
    environment_ = previous;
}

// ---- expressions ------------------------------------------------------------

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

namespace {

// ----- UTF-8 helpers --------------------------------------------------------

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

// Convert a codepoint index to a byte offset. Out-of-range indices clamp to
// the string length.
std::size_t utf8_byte_offset(const std::string& s, long long cp_index) {
    if (cp_index <= 0) return 0;
    std::size_t byte = 0;
    for (long long i = 0; i < cp_index && byte < s.size(); ++i) {
        byte += utf8_step(static_cast<unsigned char>(s[byte]));
    }
    return byte;
}

// Convert a byte offset to a codepoint index.
long long utf8_codepoint_at_byte(const std::string& s, std::size_t byte_target) {
    long long cp = 0;
    std::size_t byte = 0;
    while (byte < byte_target && byte < s.size()) {
        byte += utf8_step(static_cast<unsigned char>(s[byte]));
        cp++;
    }
    return cp;
}

bool is_ascii_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

}  // namespace

namespace {

// Wraps a C++ lambda as a Callable bound to a particular receiver. Used to
// produce list/map intrinsic methods like .push, .keys, etc.
CallablePtr make_bound_native(std::string name, int arity, NativeFunction::Fn fn) {
    auto callable = std::make_shared<NativeFunction>(std::move(name), arity, std::move(fn));
    return std::static_pointer_cast<Callable>(callable);
}

}  // namespace

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

// ---- statements -------------------------------------------------------------

void Interpreter::visit(ExpressionStmt& s) { evaluate(*s.expr); }

void Interpreter::visit(VarStmt& s) {
    Value v = s.initializer ? evaluate(*s.initializer) : Value{};
    environment_->define(std::string(s.name.lexeme), std::move(v));
}

void Interpreter::visit(BlockStmt& s) {
    execute_block(s.statements, std::make_shared<Environment>(environment_));
}

void Interpreter::visit(IfStmt& s) {
    if (evaluate(*s.cond).truthy()) execute(*s.then_branch);
    else if (s.else_branch)         execute(*s.else_branch);
}

void Interpreter::visit(WhileStmt& s) {
    while (evaluate(*s.cond).truthy()) execute(*s.body);
}

void Interpreter::visit(FunctionStmt& s) {
    auto fn = std::make_shared<UserFunction>(
        std::string(s.name.lexeme), &s.params, &s.body, environment_);
    environment_->define(std::string(s.name.lexeme),
                         Value{std::static_pointer_cast<Callable>(fn)});
}

void Interpreter::visit(FunctionExpr& e) {
    std::string name = e.name.empty() ? std::string("<anonymous>") : e.name;
    auto fn = std::make_shared<UserFunction>(
        std::move(name), &e.params, &e.body, environment_);
    result_ = Value{std::static_pointer_cast<Callable>(fn)};
}

void Interpreter::visit(ReturnStmt& s) {
    Value v = s.value ? evaluate(*s.value) : Value{};
    throw ReturnSignal{std::move(v)};
}

void Interpreter::visit(ImportStmt& s) {
    // Decode the path string literal (strip quotes, process escapes).
    std::string path = decode_string_literal(s.path_token);

    // Where to resolve relative paths from: the importing file's directory,
    // or the process CWD if there is no current file.
    std::filesystem::path requesting_dir = current_file_.has_parent_path()
        ? current_file_.parent_path()
        : std::filesystem::current_path();

    auto m = modules_.load(path, requesting_dir, s.keyword);
    environment_->define(std::string(s.alias.lexeme), Value{m});
}

// ---- builtins ---------------------------------------------------------------

void Interpreter::register_builtins() {
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
    globals_->define("\xe0\xa6\xa6\xe0\xa7\x87\xe0\xa6\x96\xe0\xa6\xbe\xe0\xa6\x93", Value{print_fn});  // দেখাও

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
}

}  // namespace bnl
