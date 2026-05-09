#pragma once

// Private to src/interpreter/*.cpp. Holds declarations that need to be shared
// across the runtime/exprs/stmts/builtins translation units but should not
// leak into the public interpreter.h surface.

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ast.h"
#include "environment.h"
#include "interpreter.h"
#include "token.h"
#include "value.h"

namespace bnl::interp_detail {

// Decode a string-literal token's lexeme (strips the surrounding quotes and
// resolves \n \t \r \\ \" \0 escapes). Used by LiteralExpr's String branch
// and by ImportStmt for the import path.
inline std::string decode_string_literal(const Token& tok) {
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

// ASCII-only space test (matches the bnl runtime's existing trim/to_number
// behavior — Unicode whitespace is intentionally not recognized for v1).
inline bool is_ascii_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// Backs both `function` declarations and anonymous `function (...) {...}`
// expressions. Holds raw pointers into the AST nodes that own the params and
// body vectors (the AST outlives every closure that references it).
class UserFunction : public Callable {
public:
    UserFunction(std::string                  name,
                 const std::vector<Token>*    params,
                 const std::vector<StmtPtr>*  body,
                 std::shared_ptr<Environment> closure)
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

}  // namespace bnl::interp_detail
