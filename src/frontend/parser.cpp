#include "frontend/parser.h"

#include <utility>

namespace bnl {

Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

std::vector<StmtPtr> Parser::parse() {
    std::vector<StmtPtr> stmts;
    while (!at_end()) {
        try {
            stmts.push_back(declaration());
        } catch (const ParseError&) {
            synchronize();
        }
    }
    return stmts;
}

// ============================================================================
// Statements
// ============================================================================

StmtPtr Parser::declaration() {
    if (match({TokenType::Var}))      return var_declaration();
    if (match({TokenType::Function})) return function_declaration("function");
    if (match({TokenType::Class}))    return class_declaration();
    if (match({TokenType::Import}))   return import_statement(previous());
    return statement();
}

StmtPtr Parser::import_statement(Token keyword) {
    Token path_token = consume(TokenType::String, "expected module path string after 'import'");
    consume(TokenType::As, "expected 'as' after import path");
    Token alias = consume(TokenType::Identifier, "expected alias identifier after 'as'");
    consume(TokenType::Semicolon, "expected ';' after import");
    return std::make_unique<ImportStmt>(keyword, path_token, alias);
}

StmtPtr Parser::var_declaration() {
    Token name = consume(TokenType::Identifier, "expected variable name");
    ExprPtr init = nullptr;
    if (match({TokenType::Assign})) {
        init = expression();
    }
    consume(TokenType::Semicolon, "expected ';' after variable declaration");
    return std::make_unique<VarStmt>(name, std::move(init));
}

std::unique_ptr<FunctionStmt> Parser::parse_function(const std::string& kind) {
    Token name = consume(TokenType::Identifier, "expected " + kind + " name");
    consume(TokenType::LParen, "expected '(' after " + kind + " name");
    std::vector<Token> params;
    if (!check(TokenType::RParen)) {
        do {
            if (params.size() >= 255) {
                throw_error(peek(), "cannot have more than 255 parameters");
            }
            params.push_back(consume(TokenType::Identifier, "expected parameter name"));
        } while (match({TokenType::Comma}));
    }
    consume(TokenType::RParen, "expected ')' after parameters");
    consume(TokenType::LBrace, "expected '{' before " + kind + " body");
    auto body = block_body();
    return std::make_unique<FunctionStmt>(name, std::move(params), std::move(body));
}

StmtPtr Parser::function_declaration(const std::string& kind) {
    return parse_function(kind);  // unique_ptr<FunctionStmt> -> StmtPtr (Base)
}

StmtPtr Parser::class_declaration() {
    Token name = consume(TokenType::Identifier, "expected class name");

    // Optional `extends Parent` for single inheritance.
    Token superclass{TokenType::Identifier, "", name.line, name.column};
    if (match({TokenType::Extends})) {
        superclass = consume(TokenType::Identifier, "expected superclass name after 'extends'");
    }

    consume(TokenType::LBrace, "expected '{' before class body");
    std::vector<std::unique_ptr<FunctionStmt>> methods;
    while (!check(TokenType::RBrace) && !at_end()) {
        if (!match({TokenType::Function})) {
            throw_error(peek(), "expected 'function' (method) inside class body");
        }
        methods.push_back(parse_function("method"));
    }
    consume(TokenType::RBrace, "expected '}' after class body");
    return std::make_unique<ClassStmt>(name, superclass, std::move(methods));
}

StmtPtr Parser::statement() {
    if (match({TokenType::If}))     return if_statement();
    if (match({TokenType::While}))  return while_statement();
    if (match({TokenType::Return})) return return_statement();
    if (match({TokenType::LBrace})) return block_statement();
    return expression_statement();
}

StmtPtr Parser::if_statement() {
    consume(TokenType::LParen, "expected '(' after 'if'");
    ExprPtr cond = expression();
    consume(TokenType::RParen, "expected ')' after if condition");
    StmtPtr then_branch = statement();
    StmtPtr else_branch = nullptr;
    if (match({TokenType::Else})) {
        else_branch = statement();
    }
    return std::make_unique<IfStmt>(std::move(cond), std::move(then_branch), std::move(else_branch));
}

StmtPtr Parser::while_statement() {
    consume(TokenType::LParen, "expected '(' after 'while'");
    ExprPtr cond = expression();
    consume(TokenType::RParen, "expected ')' after while condition");
    StmtPtr body = statement();
    return std::make_unique<WhileStmt>(std::move(cond), std::move(body));
}

StmtPtr Parser::return_statement() {
    Token   keyword = previous();
    ExprPtr value   = nullptr;
    if (!check(TokenType::Semicolon)) {
        value = expression();
    }
    consume(TokenType::Semicolon, "expected ';' after return value");
    return std::make_unique<ReturnStmt>(keyword, std::move(value));
}

StmtPtr Parser::block_statement() {
    return std::make_unique<BlockStmt>(block_body());
}

std::vector<StmtPtr> Parser::block_body() {
    std::vector<StmtPtr> stmts;
    while (!check(TokenType::RBrace) && !at_end()) {
        stmts.push_back(declaration());
    }
    consume(TokenType::RBrace, "expected '}' after block");
    return stmts;
}

StmtPtr Parser::expression_statement() {
    ExprPtr expr = expression();
    consume(TokenType::Semicolon, "expected ';' after expression");
    return std::make_unique<ExpressionStmt>(std::move(expr));
}

// ============================================================================
// Expressions — precedence climbing
//
// assignment      :  logical_or ( '=' assignment )?
// logical_or      :  logical_and ( ( '||' | 'or' ) logical_and )*
// logical_and     :  equality   ( ( '&&' | 'and' ) equality )*
// equality        :  comparison ( ( '==' | '!=' )   comparison )*
// comparison      :  term       ( ( '<' | '>' | '<=' | '>=' ) term )*
// term            :  factor     ( ( '+' | '-' ) factor )*
// factor          :  unary      ( ( '*' | '/' | '%' ) unary )*
// unary           :  ( '!' | '-' | 'not' ) unary | call
// call            :  primary ( '(' arguments? ')' )*
// primary         :  Number | String | true | false | null
//                 |  Identifier | '(' expression ')'
// ============================================================================

ExprPtr Parser::expression() { return assignment(); }

ExprPtr Parser::assignment() {
    ExprPtr expr = logical_or();
    if (match({TokenType::Assign})) {
        Token   equals = previous();
        ExprPtr value  = assignment();
        if (auto* id = dynamic_cast<IdentifierExpr*>(expr.get())) {
            return std::make_unique<AssignExpr>(id->name, std::move(value));
        }
        if (auto* m = dynamic_cast<MemberExpr*>(expr.get())) {
            // Steal the member's pieces, drop the original wrapper.
            auto obj = std::move(m->object);
            return std::make_unique<SetMemberExpr>(std::move(obj), m->name, std::move(value));
        }
        if (auto* ix = dynamic_cast<IndexExpr*>(expr.get())) {
            auto obj   = std::move(ix->object);
            auto index = std::move(ix->index);
            return std::make_unique<SetIndexExpr>(std::move(obj), ix->bracket,
                                                  std::move(index), std::move(value));
        }
        throw_error(equals, "invalid assignment target");
    }
    return expr;
}

ExprPtr Parser::logical_or() {
    ExprPtr expr = logical_and();
    while (match({TokenType::PipePipe, TokenType::Or})) {
        Token   op    = previous();
        ExprPtr right = logical_and();
        expr = std::make_unique<LogicalExpr>(std::move(expr), op, std::move(right));
    }
    return expr;
}

ExprPtr Parser::logical_and() {
    ExprPtr expr = equality();
    while (match({TokenType::AmpAmp, TokenType::And})) {
        Token   op    = previous();
        ExprPtr right = equality();
        expr = std::make_unique<LogicalExpr>(std::move(expr), op, std::move(right));
    }
    return expr;
}

ExprPtr Parser::equality() {
    ExprPtr expr = comparison();
    while (match({TokenType::EqEq, TokenType::BangEq})) {
        Token   op    = previous();
        ExprPtr right = comparison();
        expr = std::make_unique<BinaryExpr>(std::move(expr), op, std::move(right));
    }
    return expr;
}

ExprPtr Parser::comparison() {
    ExprPtr expr = term();
    while (match({TokenType::Lt, TokenType::Gt, TokenType::LtEq, TokenType::GtEq})) {
        Token   op    = previous();
        ExprPtr right = term();
        expr = std::make_unique<BinaryExpr>(std::move(expr), op, std::move(right));
    }
    return expr;
}

ExprPtr Parser::term() {
    ExprPtr expr = factor();
    while (match({TokenType::Plus, TokenType::Minus})) {
        Token   op    = previous();
        ExprPtr right = factor();
        expr = std::make_unique<BinaryExpr>(std::move(expr), op, std::move(right));
    }
    return expr;
}

ExprPtr Parser::factor() {
    ExprPtr expr = unary();
    while (match({TokenType::Star, TokenType::Slash, TokenType::Percent})) {
        Token   op    = previous();
        ExprPtr right = unary();
        expr = std::make_unique<BinaryExpr>(std::move(expr), op, std::move(right));
    }
    return expr;
}

ExprPtr Parser::unary() {
    if (match({TokenType::Bang, TokenType::Minus, TokenType::Not})) {
        Token   op      = previous();
        ExprPtr operand = unary();
        return std::make_unique<UnaryExpr>(op, std::move(operand));
    }
    return call();
}

ExprPtr Parser::call() {
    ExprPtr expr = primary();
    while (true) {
        if (match({TokenType::LParen})) {
            expr = finish_call(std::move(expr));
        } else if (match({TokenType::Dot})) {
            Token name = consume(TokenType::Identifier, "expected property name after '.'");
            expr = std::make_unique<MemberExpr>(std::move(expr), name);
        } else if (match({TokenType::LBracket})) {
            Token   bracket = previous();
            ExprPtr index   = expression();
            consume(TokenType::RBracket, "expected ']' after index expression");
            expr = std::make_unique<IndexExpr>(std::move(expr), bracket, std::move(index));
        } else {
            break;
        }
    }
    return expr;
}

ExprPtr Parser::finish_call(ExprPtr callee) {
    std::vector<ExprPtr> args;
    if (!check(TokenType::RParen)) {
        do {
            if (args.size() >= 255) {
                throw_error(peek(), "cannot have more than 255 arguments");
            }
            args.push_back(expression());
        } while (match({TokenType::Comma}));
    }
    Token paren = consume(TokenType::RParen, "expected ')' after arguments");
    return std::make_unique<CallExpr>(std::move(callee), paren, std::move(args));
}

ExprPtr Parser::primary() {
    if (match({TokenType::True, TokenType::False, TokenType::Null,
               TokenType::Number, TokenType::String})) {
        return std::make_unique<LiteralExpr>(previous());
    }
    if (match({TokenType::Identifier})) {
        return std::make_unique<IdentifierExpr>(previous());
    }
    if (match({TokenType::Function})) {
        return function_expression();
    }
    if (match({TokenType::Super})) {
        Token keyword = previous();
        if (match({TokenType::Dot})) {
            Token method = consume(TokenType::Identifier, "expected method name after 'super.'");
            return std::make_unique<SuperExpr>(keyword, method);
        }
        // Bare `super` is only legal as `super(args)` (call parent's init).
        // Synthesize a method token "init" — runtime treats this uniformly.
        Token init_method{TokenType::Identifier, "init", keyword.line, keyword.column};
        return std::make_unique<SuperExpr>(keyword, init_method);
    }
    if (match({TokenType::LBracket})) {
        Token bracket = previous();
        std::vector<ExprPtr> elements;
        if (!check(TokenType::RBracket)) {
            do {
                if (check(TokenType::RBracket)) break;  // trailing comma
                elements.push_back(expression());
            } while (match({TokenType::Comma}));
        }
        consume(TokenType::RBracket, "expected ']' to close list literal");
        return std::make_unique<ListExpr>(bracket, std::move(elements));
    }
    if (match({TokenType::LBrace})) {
        // Map literal: keys are bare identifiers or string literals.
        // Note: `{` only reaches here in expression context. In statement
        // position, statement() already routed `{` to block_statement().
        Token brace = previous();
        std::vector<std::pair<std::string, ExprPtr>> entries;
        if (!check(TokenType::RBrace)) {
            do {
                if (check(TokenType::RBrace)) break;  // trailing comma
                std::string key;
                if (match({TokenType::String})) {
                    auto sv = previous().lexeme;
                    // Strip surrounding quotes; no escapes for map keys (keep simple).
                    key = std::string(sv.substr(1, sv.size() - 2));
                } else if (match({TokenType::Identifier})) {
                    key = std::string(previous().lexeme);
                } else {
                    throw_error(peek(), "expected map key (identifier or string)");
                }
                consume(TokenType::Colon, "expected ':' after map key");
                ExprPtr value = expression();
                entries.emplace_back(std::move(key), std::move(value));
            } while (match({TokenType::Comma}));
        }
        consume(TokenType::RBrace, "expected '}' to close map literal");
        return std::make_unique<MapExpr>(brace, std::move(entries));
    }
    if (match({TokenType::LParen})) {
        ExprPtr inner = expression();
        consume(TokenType::RParen, "expected ')' after expression");
        return std::make_unique<GroupingExpr>(std::move(inner));
    }
    throw_error(peek(), "expected expression");
}

ExprPtr Parser::function_expression() {
    // 'function' (or 'কাজ') was already consumed by primary().
    std::string name;
    if (check(TokenType::Identifier)) {
        name = std::string(advance().lexeme);
    }
    consume(TokenType::LParen, "expected '(' after 'function'");
    std::vector<Token> params;
    if (!check(TokenType::RParen)) {
        do {
            if (params.size() >= 255) {
                throw_error(peek(), "cannot have more than 255 parameters");
            }
            params.push_back(consume(TokenType::Identifier, "expected parameter name"));
        } while (match({TokenType::Comma}));
    }
    consume(TokenType::RParen, "expected ')' after parameters");
    consume(TokenType::LBrace, "expected '{' before function body");
    auto body = block_body();
    return std::make_unique<FunctionExpr>(std::move(name), std::move(params), std::move(body));
}

// ============================================================================
// Token helpers
// ============================================================================

bool Parser::match(std::initializer_list<TokenType> types) {
    for (TokenType t : types) {
        if (check(t)) { advance(); return true; }
    }
    return false;
}

bool Parser::check(TokenType type) const {
    if (at_end()) return false;
    return peek().type == type;
}

Token Parser::advance() {
    if (!at_end()) current_++;
    return previous();
}

bool Parser::at_end() const {
    return peek().type == TokenType::EndOfFile;
}

const Token& Parser::peek() const     { return tokens_[current_]; }
const Token& Parser::previous() const { return tokens_[current_ - 1]; }

Token Parser::consume(TokenType type, const std::string& message) {
    if (check(type)) return advance();
    throw_error(peek(), message);
}

// ============================================================================
// Error reporting and panic-mode recovery
// ============================================================================

void Parser::throw_error(const Token& tok, const std::string& message) {
    std::string at = (tok.type == TokenType::EndOfFile)
        ? std::string(" at end of input")
        : (" at '" + std::string(tok.lexeme) + "'");
    diagnostics_.push_back({tok.line, tok.column, message + at});
    throw ParseError(message);
}

void Parser::synchronize() {
    advance();
    while (!at_end()) {
        if (previous().type == TokenType::Semicolon) return;
        switch (peek().type) {
            case TokenType::Class:
            case TokenType::Function:
            case TokenType::Var:
            case TokenType::For:
            case TokenType::If:
            case TokenType::While:
            case TokenType::Return:
                return;
            default:
                advance();
        }
    }
}

}  // namespace bnl
