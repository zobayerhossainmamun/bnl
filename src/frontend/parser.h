#pragma once

#include <cstddef>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <vector>

#include "bnl/ast.h"
#include "bnl/token.h"
#include "diagnostic.h"

namespace bnl {

class Parser {
public:
    explicit Parser(std::vector<Token> tokens);

    std::vector<StmtPtr> parse();

    const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }
    bool has_errors() const { return !diagnostics_.empty(); }

private:
    // Sentinel thrown by helpers to unwind to the nearest synchronization point.
    struct ParseError : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    // ---- statements ---------------------------------------------------------
    StmtPtr declaration();
    StmtPtr var_declaration();
    StmtPtr function_declaration(const std::string& kind);
    StmtPtr class_declaration();
    std::unique_ptr<FunctionStmt> parse_function(const std::string& kind);
    StmtPtr statement();
    StmtPtr if_statement();
    StmtPtr while_statement();
    StmtPtr for_statement();
    StmtPtr return_statement();
    StmtPtr import_statement(Token keyword);
    StmtPtr try_statement();
    StmtPtr throw_statement();
    StmtPtr switch_statement();
    StmtPtr break_statement();
    StmtPtr continue_statement();
    StmtPtr block_statement();
    StmtPtr expression_statement();

    std::vector<StmtPtr> block_body();

    // ---- expressions (precedence climbing) ---------------------------------
    ExprPtr expression();
    ExprPtr assignment();
    ExprPtr logical_or();
    ExprPtr logical_and();
    ExprPtr equality();
    ExprPtr comparison();
    ExprPtr term();
    ExprPtr factor();
    ExprPtr unary();
    ExprPtr call();
    ExprPtr finish_call(ExprPtr callee);
    ExprPtr primary();
    ExprPtr function_expression();

    // ---- token-stream helpers ----------------------------------------------
    bool match(std::initializer_list<TokenType> types);
    bool check(TokenType type) const;
    Token advance();
    bool at_end() const;
    const Token& peek() const;
    const Token& previous() const;
    Token consume(TokenType type, const std::string& message);

    // ---- error reporting / recovery ----------------------------------------
    [[noreturn]] void throw_error(const Token& tok, const std::string& message);
    void synchronize();

    std::vector<Token>      tokens_;
    std::size_t             current_ = 0;
    std::vector<Diagnostic> diagnostics_;
};

}  // namespace bnl
