#include "frontend/parser.h"

#include <string>
#include <string_view>
#include <utility>

#include "frontend/lexer.h"

namespace bnl {

Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

namespace {
// True for token types that have an alphabetic lexeme — every keyword plus
// Identifier itself. Map keys and member access (`obj.field`) accept any of
// these as a bareword, so users can write `{default: 1}` or `args.default`
// even when `default` is a reserved keyword.
bool is_namelike_token(TokenType t) {
    switch (t) {
        case TokenType::Identifier:
        case TokenType::If:    case TokenType::Else:
        case TokenType::While: case TokenType::For:    case TokenType::Of:
        case TokenType::Function: case TokenType::Return:
        case TokenType::Var:
        case TokenType::Class: case TokenType::Extends: case TokenType::Super:
        case TokenType::Import: case TokenType::As:
        case TokenType::And: case TokenType::Or: case TokenType::Not:
        case TokenType::True: case TokenType::False: case TokenType::Null:
        case TokenType::Try: case TokenType::Catch: case TokenType::Throw: case TokenType::Finally:
        case TokenType::Switch: case TokenType::Case: case TokenType::Default:
        case TokenType::Break:  case TokenType::Continue:
            return true;
        default:
            return false;
    }
}
}  // namespace

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
    auto params = parse_param_list(kind);
    consume(TokenType::LBrace, "expected '{' before " + kind + " body");
    auto body = block_body();
    return std::make_unique<FunctionStmt>(name, std::move(params), std::move(body));
}

// Parses a comma-separated parameter list and the closing ')'. Each param
// is `name` or `name = default_expr`. Once one param has a default, every
// subsequent param must also have one (defaults are tail-only).
std::vector<Param> Parser::parse_param_list(const std::string& kind) {
    std::vector<Param> params;
    bool seen_default = false;
    if (!check(TokenType::RParen)) {
        do {
            if (params.size() >= 255) {
                throw_error(peek(), "cannot have more than 255 parameters");
            }
            Token pname = consume(TokenType::Identifier, "expected parameter name");
            ExprPtr def;
            if (match({TokenType::Assign})) {
                def = expression();
                seen_default = true;
            } else if (seen_default) {
                throw_error(pname, "required parameter cannot follow one with a "
                                   "default value (defaults must be at the end)");
            }
            params.emplace_back(pname, std::move(def));
        } while (match({TokenType::Comma}));
    }
    consume(TokenType::RParen, "expected ')' after parameters");
    (void)kind;
    return params;
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
    if (match({TokenType::If}))       return if_statement();
    if (match({TokenType::While}))    return while_statement();
    if (match({TokenType::For}))      return for_statement();
    if (match({TokenType::Return}))   return return_statement();
    if (match({TokenType::Try}))      return try_statement();
    if (match({TokenType::Throw}))    return throw_statement();
    if (match({TokenType::Switch}))   return switch_statement();
    if (match({TokenType::Break}))    return break_statement();
    if (match({TokenType::Continue})) return continue_statement();
    if (match({TokenType::LBrace}))   return block_statement();
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
    Token keyword = previous();   // the 'while' token consumed by statement()
    consume(TokenType::LParen, "expected '(' after 'while'");
    ExprPtr cond = expression();
    consume(TokenType::RParen, "expected ')' after while condition");
    StmtPtr body = statement();
    return std::make_unique<WhileStmt>(keyword, std::move(cond), std::move(body));
}

// for-loop is a tiny grammar fork:
//
//   for ( var X of EXPR )  body          → ForOfStmt
//   for ( init; cond; update )  body     → ForStmt   (any of the three may be empty)
//
// We disambiguate by lookahead: after `for (`, if we see `var IDENT of`, it's
// the iterator form; otherwise C-style.
StmtPtr Parser::for_statement() {
    Token keyword = previous();   // the 'for' token consumed by statement()
    consume(TokenType::LParen, "expected '(' after 'for'");

    // ---- iterator form: for (var X of EXPR) body -----------------------
    if (check(TokenType::Var) &&
        current_ + 2 < tokens_.size() &&
        tokens_[current_ + 1].type == TokenType::Identifier &&
        tokens_[current_ + 2].type == TokenType::Of) {

        advance();                        // consume `var`
        Token name = consume(TokenType::Identifier, "expected loop variable name");
        consume(TokenType::Of, "expected 'of' after loop variable");
        ExprPtr iter = expression();
        consume(TokenType::RParen, "expected ')' after for-of header");
        StmtPtr body = statement();
        return std::make_unique<ForOfStmt>(name, std::move(iter), std::move(body));
    }

    // ---- C-style: for (init; cond; update) body ------------------------
    StmtPtr init;
    if (match({TokenType::Semicolon})) {
        init = nullptr;
    } else if (match({TokenType::Var})) {
        init = var_declaration();
    } else {
        init = expression_statement();
    }

    ExprPtr cond;
    if (!check(TokenType::Semicolon)) cond = expression();
    consume(TokenType::Semicolon, "expected ';' after for condition");

    ExprPtr update;
    if (!check(TokenType::RParen)) update = expression();
    consume(TokenType::RParen, "expected ')' after for header");

    StmtPtr body = statement();
    return std::make_unique<ForStmt>(keyword, std::move(init), std::move(cond),
                                     std::move(update), std::move(body));
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

StmtPtr Parser::try_statement() {
    Token keyword = previous();   // 'try' / 'চেষ্টা'

    consume(TokenType::LBrace, "expected '{' after 'try'");
    std::vector<StmtPtr> try_block = block_body();

    bool                 has_catch = false;
    Token                catch_var{};
    std::vector<StmtPtr> catch_block;

    if (match({TokenType::Catch})) {
        has_catch = true;
        consume(TokenType::LParen, "expected '(' after 'catch'");
        catch_var = consume(TokenType::Identifier,
                            "expected identifier for caught value");
        consume(TokenType::RParen, "expected ')' after catch identifier");
        consume(TokenType::LBrace, "expected '{' after catch declaration");
        catch_block = block_body();
    }

    bool                 has_finally = false;
    std::vector<StmtPtr> finally_block;
    if (match({TokenType::Finally})) {
        has_finally = true;
        consume(TokenType::LBrace, "expected '{' after 'finally'");
        finally_block = block_body();
    }

    if (!has_catch && !has_finally) {
        throw_error(peek(), "expected 'catch' or 'finally' after try block");
    }

    return std::make_unique<TryStmt>(
        keyword, std::move(try_block),
        has_catch, catch_var, std::move(catch_block),
        has_finally, std::move(finally_block));
}

StmtPtr Parser::throw_statement() {
    Token keyword = previous();   // 'throw' / 'নিক্ষেপ'
    ExprPtr value = expression();
    consume(TokenType::Semicolon, "expected ';' after throw value");
    return std::make_unique<ThrowStmt>(keyword, std::move(value));
}

// switch (subject) { case X: { ... } case Y: case Z: { ... } default: { ... } }
//
// No fall-through. Each case body is required to be wrapped in `{ ... }`,
// and after the matching block runs control exits the switch. Multiple
// `case <expr>:` lines stacked before a single block share that block.
StmtPtr Parser::switch_statement() {
    Token keyword = previous();   // 'switch' / 'বিকল্প'
    consume(TokenType::LParen, "expected '(' after 'switch'");
    ExprPtr subject = expression();
    consume(TokenType::RParen, "expected ')' after switch subject");
    consume(TokenType::LBrace, "expected '{' before switch body");

    std::vector<SwitchCase> cases;
    bool                    has_default  = false;
    std::vector<StmtPtr>    default_body;

    // Pending case values waiting for a body.
    std::vector<ExprPtr> pending_values;

    while (!check(TokenType::RBrace) && !at_end()) {
        if (match({TokenType::Case})) {
            ExprPtr v = expression();
            consume(TokenType::Colon, "expected ':' after case value");
            pending_values.push_back(std::move(v));

            // If the next token is `{`, this case takes a body. Otherwise
            // it stacks onto the next case / default.
            if (check(TokenType::LBrace)) {
                advance();   // consume '{'
                std::vector<StmtPtr> body = block_body();
                cases.push_back(SwitchCase{
                    std::move(pending_values), std::move(body)});
                pending_values.clear();
            }
        } else if (match({TokenType::Default})) {
            consume(TokenType::Colon, "expected ':' after 'default'");
            consume(TokenType::LBrace, "expected '{' after 'default:'");
            std::vector<StmtPtr> body = block_body();
            if (!pending_values.empty()) {
                // Cases stacked before `default` share the default body.
                cases.push_back(SwitchCase{
                    std::move(pending_values), std::move(body)});
                pending_values.clear();
                has_default  = true;
                default_body.clear();
            } else {
                has_default  = true;
                default_body = std::move(body);
            }
        } else {
            throw_error(peek(), "expected 'case' or 'default' inside switch body");
        }
    }
    consume(TokenType::RBrace, "expected '}' after switch body");

    if (!pending_values.empty()) {
        throw_error(keyword, "switch has 'case' values with no body");
    }

    return std::make_unique<SwitchStmt>(keyword, std::move(subject),
                                        std::move(cases),
                                        has_default, std::move(default_body));
}

StmtPtr Parser::break_statement() {
    Token keyword = previous();   // 'break' / 'থামুন'
    consume(TokenType::Semicolon, "expected ';' after 'break'");
    return std::make_unique<BreakStmt>(keyword);
}

StmtPtr Parser::continue_statement() {
    Token keyword = previous();   // 'continue' / 'চলুন'
    consume(TokenType::Semicolon, "expected ';' after 'continue'");
    return std::make_unique<ContinueStmt>(keyword);
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

namespace {
// True for the three lvalue shapes that all assignments (plain and compound)
// accept: bare identifier, member access, or indexed access.
bool is_lvalue(const Expr* e) {
    return dynamic_cast<const IdentifierExpr*>(e)
        || dynamic_cast<const MemberExpr*>(e)
        || dynamic_cast<const IndexExpr*>(e);
}

// Map +=, -=, ... to the underlying binary op token type. Used to build the
// op Token stored on CompoundAssignExpr.
TokenType compound_to_binary_op(TokenType t) {
    switch (t) {
        case TokenType::PlusEq:    return TokenType::Plus;
        case TokenType::MinusEq:   return TokenType::Minus;
        case TokenType::StarEq:    return TokenType::Star;
        case TokenType::SlashEq:   return TokenType::Slash;
        case TokenType::PercentEq: return TokenType::Percent;
        default:                   return t;
    }
}
}  // namespace

ExprPtr Parser::assignment() {
    ExprPtr expr = coalesce();
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
    if (match({TokenType::PlusEq, TokenType::MinusEq, TokenType::StarEq,
               TokenType::SlashEq, TokenType::PercentEq})) {
        Token op_eq = previous();
        if (!is_lvalue(expr.get())) {
            throw_error(op_eq, "invalid compound-assignment target");
        }
        ExprPtr rhs = assignment();
        Token   bin_op{compound_to_binary_op(op_eq.type), op_eq.lexeme,
                       op_eq.line, op_eq.column};
        return std::make_unique<CompoundAssignExpr>(
            std::move(expr), bin_op, std::move(rhs));
    }
    return expr;
}

// `a ?? b` short-circuits on null. Sits between assignment and logical_or so
// `a || b ?? c` parses as `(a || b) ?? c` (the precedence chain is climbed
// outward, so a tighter operator runs first).
ExprPtr Parser::coalesce() {
    ExprPtr expr = logical_or();
    while (match({TokenType::QuestionQuestion})) {
        Token   op    = previous();
        ExprPtr right = logical_or();
        expr = std::make_unique<LogicalExpr>(std::move(expr), op, std::move(right));
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
    if (match({TokenType::PlusPlus, TokenType::MinusMinus})) {
        // Prefix increment / decrement. Same desugar as postfix: target += 1.
        // Yields the new value (matches +=, deviates slightly from C semantics
        // for prefix-vs-postfix distinction — see lang notes).
        Token   op      = previous();
        ExprPtr operand = unary();
        if (!is_lvalue(operand.get())) {
            throw_error(op, "operand of '++' / '--' must be assignable");
        }
        bool   is_inc = (op.type == TokenType::PlusPlus);
        Token  bin_op{is_inc ? TokenType::Plus : TokenType::Minus,
                      op.lexeme, op.line, op.column};
        Token  one_tok{TokenType::Number, "1", op.line, op.column};
        auto   one = std::make_unique<LiteralExpr>(one_tok);
        return std::make_unique<CompoundAssignExpr>(
            std::move(operand), bin_op, std::move(one));
    }
    if (match({TokenType::Wait})) {
        Token   keyword = previous();
        ExprPtr operand = unary();
        return std::make_unique<WaitExpr>(keyword, std::move(operand));
    }
    return call();
}

ExprPtr Parser::call() {
    ExprPtr expr = primary();
    while (true) {
        if (match({TokenType::LParen})) {
            expr = finish_call(std::move(expr));
        } else if (match({TokenType::Dot})) {
            // Allow any name-like token (identifier or keyword) so users can
            // write `args.default`, `obj.case`, etc.
            if (!is_namelike_token(peek().type)) {
                throw_error(peek(), "expected property name after '.'");
            }
            Token name = advance();
            expr = std::make_unique<MemberExpr>(std::move(expr), name);
        } else if (match({TokenType::LBracket})) {
            Token   bracket = previous();
            ExprPtr index   = expression();
            consume(TokenType::RBracket, "expected ']' after index expression");
            expr = std::make_unique<IndexExpr>(std::move(expr), bracket, std::move(index));
        } else if (match({TokenType::PlusPlus, TokenType::MinusMinus})) {
            // Postfix `x++` / `x--`. Same desugar as prefix.
            Token op = previous();
            if (!is_lvalue(expr.get())) {
                throw_error(op, "operand of '++' / '--' must be assignable");
            }
            bool   is_inc = (op.type == TokenType::PlusPlus);
            Token  bin_op{is_inc ? TokenType::Plus : TokenType::Minus,
                          op.lexeme, op.line, op.column};
            Token  one_tok{TokenType::Number, "1", op.line, op.column};
            auto   one = std::make_unique<LiteralExpr>(one_tok);
            expr = std::make_unique<CompoundAssignExpr>(
                std::move(expr), bin_op, std::move(one));
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
    if (match({TokenType::TemplateString})) {
        return template_string(previous());
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
                } else if (is_namelike_token(peek().type)) {
                    // Accept any keyword as a bareword map key (e.g. `default`).
                    key = std::string(advance().lexeme);
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

// Walks a TemplateString token's lexeme and rebuilds it as a left-associative
// `+` chain of segments and interpolated expressions. The string's `+`
// operator coerces non-string operands to their display form, so a number,
// list, etc. inside `${...}` formats predictably.
//
// Sub-lexing is done over a string_view into the original source so every
// synthesized Token's lexeme remains a stable view (the source outlives the
// AST). Decoded text segments live on LiteralExpr::precomputed_string.
ExprPtr Parser::template_string(const Token& tok) {
    auto sv = tok.lexeme;
    if (sv.size() < 2 || sv.front() != '"' || sv.back() != '"') {
        throw_error(tok, "internal: malformed template string");
    }

    std::vector<ExprPtr> parts;
    std::string          current;

    auto flush_text = [&]() {
        Token seg{TokenType::String, std::string_view{}, tok.line, tok.column};
        parts.push_back(std::make_unique<LiteralExpr>(seg, std::move(current)));
        current.clear();
    };

    std::size_t i = 1;  // skip opening "
    while (i + 1 < sv.size()) {  // last byte is closing "
        char c = sv[i];
        if (c == '\\') {
            if (i + 2 >= sv.size()) {
                throw_error(tok, "trailing backslash in template string");
            }
            char esc = sv[i + 1];
            switch (esc) {
                case 'n':  current += '\n'; break;
                case 't':  current += '\t'; break;
                case 'r':  current += '\r'; break;
                case '\\': current += '\\'; break;
                case '"':  current += '"';  break;
                case '\'': current += '\''; break;
                case '0':  current += '\0'; break;
                default:
                    throw_error(tok, "unknown escape '\\" + std::string(1, esc)
                                     + "' in template string");
            }
            i += 2;
            continue;
        }
        if (c == '$' && i + 1 < sv.size() - 1 && sv[i + 1] == '{') {
            flush_text();
            std::size_t expr_start = i + 2;
            std::size_t j          = expr_start;
            int         depth      = 1;
            // Skip the same constructs the lexer skipped, so braces inside
            // nested strings don't fool the depth counter.
            while (j + 1 < sv.size() && depth > 0) {
                char d = sv[j];
                if (d == '\\') {
                    j += 2;
                    continue;
                }
                if (d == '"' || d == '\'') {
                    char inner_q = d;
                    j++;
                    while (j + 1 < sv.size() && sv[j] != inner_q) {
                        if (sv[j] == '\\') { j += 2; continue; }
                        j++;
                    }
                    if (j + 1 < sv.size()) j++;
                    continue;
                }
                if (d == '{') { depth++; j++; continue; }
                if (d == '}') { depth--; if (depth == 0) break; j++; continue; }
                j++;
            }
            if (depth != 0) {
                throw_error(tok, "unterminated interpolation in template string");
            }
            std::string_view expr_src = sv.substr(expr_start, j - expr_start);
            Lexer  sub_lexer(expr_src);
            auto   sub_tokens = sub_lexer.tokenize();
            for (auto& dg : sub_lexer.diagnostics()) diagnostics_.push_back(dg);
            Parser sub_parser(std::move(sub_tokens));
            ExprPtr sub_expr;
            try {
                sub_expr = sub_parser.expression();
                if (!sub_parser.at_end()) {
                    throw_error(tok, "extra tokens in interpolation expression");
                }
            } catch (const ParseError&) {
                for (auto& dg : sub_parser.diagnostics()) diagnostics_.push_back(dg);
                throw;
            }
            for (auto& dg : sub_parser.diagnostics()) diagnostics_.push_back(dg);
            parts.push_back(std::move(sub_expr));
            i = j + 1;  // step past '}'
            continue;
        }
        current += c;
        i++;
    }
    flush_text();

    Token   plus_tok{TokenType::Plus, "+", tok.line, tok.column};
    ExprPtr result = std::move(parts[0]);
    for (std::size_t k = 1; k < parts.size(); ++k) {
        result = std::make_unique<BinaryExpr>(
            std::move(result), plus_tok, std::move(parts[k]));
    }
    return result;
}

ExprPtr Parser::function_expression() {
    // 'function' (or 'কাজ') was already consumed by primary().
    std::string name;
    if (check(TokenType::Identifier)) {
        name = std::string(advance().lexeme);
    }
    consume(TokenType::LParen, "expected '(' after 'function'");
    auto params = parse_param_list("function expression");
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
            case TokenType::Try:
            case TokenType::Throw:
            case TokenType::Switch:
            case TokenType::Break:
            case TokenType::Continue:
                return;
            default:
                advance();
        }
    }
}

}  // namespace bnl
