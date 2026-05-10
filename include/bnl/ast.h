#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "bnl/token.h"

namespace bnl {

// ---- forward declarations ---------------------------------------------------

class Expr;
class Stmt;

class LiteralExpr;
class IdentifierExpr;
class GroupingExpr;
class UnaryExpr;
class BinaryExpr;
class LogicalExpr;
class AssignExpr;
class CallExpr;
class MemberExpr;
class FunctionExpr;
class ListExpr;
class MapExpr;
class IndexExpr;
class SetIndexExpr;
class SetMemberExpr;
class SuperExpr;

class ExpressionStmt;
class VarStmt;
class BlockStmt;
class IfStmt;
class WhileStmt;
class FunctionStmt;
class ReturnStmt;
class ImportStmt;
class ClassStmt;
class TryStmt;
class ThrowStmt;

using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;

// ---- visitors ---------------------------------------------------------------

class ExprVisitor {
public:
    virtual ~ExprVisitor() = default;
    virtual void visit(LiteralExpr&)    = 0;
    virtual void visit(IdentifierExpr&) = 0;
    virtual void visit(GroupingExpr&)   = 0;
    virtual void visit(UnaryExpr&)      = 0;
    virtual void visit(BinaryExpr&)     = 0;
    virtual void visit(LogicalExpr&)    = 0;
    virtual void visit(AssignExpr&)     = 0;
    virtual void visit(CallExpr&)       = 0;
    virtual void visit(MemberExpr&)     = 0;
    virtual void visit(FunctionExpr&)   = 0;
    virtual void visit(ListExpr&)       = 0;
    virtual void visit(MapExpr&)        = 0;
    virtual void visit(IndexExpr&)      = 0;
    virtual void visit(SetIndexExpr&)   = 0;
    virtual void visit(SetMemberExpr&)  = 0;
    virtual void visit(SuperExpr&)      = 0;
};

class StmtVisitor {
public:
    virtual ~StmtVisitor() = default;
    virtual void visit(ExpressionStmt&) = 0;
    virtual void visit(VarStmt&)        = 0;
    virtual void visit(BlockStmt&)      = 0;
    virtual void visit(IfStmt&)         = 0;
    virtual void visit(WhileStmt&)      = 0;
    virtual void visit(FunctionStmt&)   = 0;
    virtual void visit(ReturnStmt&)     = 0;
    virtual void visit(ImportStmt&)     = 0;
    virtual void visit(ClassStmt&)      = 0;
    virtual void visit(TryStmt&)        = 0;
    virtual void visit(ThrowStmt&)      = 0;
};

// ---- bases ------------------------------------------------------------------

class Expr {
public:
    virtual ~Expr() = default;
    virtual void accept(ExprVisitor&) = 0;
};

class Stmt {
public:
    virtual ~Stmt() = default;
    virtual void accept(StmtVisitor&) = 0;
};

// ---- expression nodes -------------------------------------------------------

class LiteralExpr : public Expr {
public:
    Token value;  // Number / String / True / False / Null
    explicit LiteralExpr(Token v) : value(v) {}
    void accept(ExprVisitor& v) override { v.visit(*this); }
};

class IdentifierExpr : public Expr {
public:
    Token name;
    explicit IdentifierExpr(Token n) : name(n) {}
    void accept(ExprVisitor& v) override { v.visit(*this); }
};

class GroupingExpr : public Expr {
public:
    ExprPtr inner;
    explicit GroupingExpr(ExprPtr e) : inner(std::move(e)) {}
    void accept(ExprVisitor& v) override { v.visit(*this); }
};

class UnaryExpr : public Expr {
public:
    Token   op;
    ExprPtr operand;
    UnaryExpr(Token o, ExprPtr e) : op(o), operand(std::move(e)) {}
    void accept(ExprVisitor& v) override { v.visit(*this); }
};

class BinaryExpr : public Expr {
public:
    ExprPtr left;
    Token   op;
    ExprPtr right;
    BinaryExpr(ExprPtr l, Token o, ExprPtr r)
        : left(std::move(l)), op(o), right(std::move(r)) {}
    void accept(ExprVisitor& v) override { v.visit(*this); }
};

class LogicalExpr : public Expr {
public:
    ExprPtr left;
    Token   op;  // AmpAmp / PipePipe / And / Or
    ExprPtr right;
    LogicalExpr(ExprPtr l, Token o, ExprPtr r)
        : left(std::move(l)), op(o), right(std::move(r)) {}
    void accept(ExprVisitor& v) override { v.visit(*this); }
};

class AssignExpr : public Expr {
public:
    Token   name;
    ExprPtr value;
    AssignExpr(Token n, ExprPtr v) : name(n), value(std::move(v)) {}
    void accept(ExprVisitor& v) override { v.visit(*this); }
};

class CallExpr : public Expr {
public:
    ExprPtr              callee;
    Token                paren;  // closing ')' — kept for error reporting
    std::vector<ExprPtr> arguments;
    CallExpr(ExprPtr c, Token p, std::vector<ExprPtr> a)
        : callee(std::move(c)), paren(p), arguments(std::move(a)) {}
    void accept(ExprVisitor& v) override { v.visit(*this); }
};

class MemberExpr : public Expr {
public:
    ExprPtr object;
    Token   name;  // the identifier after the dot
    MemberExpr(ExprPtr o, Token n) : object(std::move(o)), name(n) {}
    void accept(ExprVisitor& v) override { v.visit(*this); }
};

// Anonymous (or optionally named) function used as an expression value:
//     var f = function (x) { return x + 1; };
//     timers.set(0, function () { print("hi"); });
// The optional name is descriptive only; it does NOT bind in the surrounding
// scope (use a `function` statement for that).
class FunctionExpr : public Expr {
public:
    std::string          name;   // empty if anonymous
    std::vector<Token>   params;
    std::vector<StmtPtr> body;
    FunctionExpr(std::string n, std::vector<Token> p, std::vector<StmtPtr> b)
        : name(std::move(n)), params(std::move(p)), body(std::move(b)) {}
    void accept(ExprVisitor& v) override { v.visit(*this); }
};

// [a, b, c]
class ListExpr : public Expr {
public:
    Token                bracket;  // opening '[' for error reporting
    std::vector<ExprPtr> elements;
    ListExpr(Token b, std::vector<ExprPtr> e) : bracket(b), elements(std::move(e)) {}
    void accept(ExprVisitor& v) override { v.visit(*this); }
};

// {key: value, "other": expr}
class MapExpr : public Expr {
public:
    Token                                       brace;  // opening '{'
    std::vector<std::pair<std::string, ExprPtr>> entries;  // key already decoded
    MapExpr(Token b, std::vector<std::pair<std::string, ExprPtr>> e)
        : brace(b), entries(std::move(e)) {}
    void accept(ExprVisitor& v) override { v.visit(*this); }
};

// obj[index]   (read)
class IndexExpr : public Expr {
public:
    ExprPtr object;
    Token   bracket;  // opening '[' for error reporting
    ExprPtr index;
    IndexExpr(ExprPtr o, Token b, ExprPtr i)
        : object(std::move(o)), bracket(b), index(std::move(i)) {}
    void accept(ExprVisitor& v) override { v.visit(*this); }
};

// obj[index] = value
class SetIndexExpr : public Expr {
public:
    ExprPtr object;
    Token   bracket;
    ExprPtr index;
    ExprPtr value;
    SetIndexExpr(ExprPtr o, Token b, ExprPtr i, ExprPtr v)
        : object(std::move(o)), bracket(b), index(std::move(i)), value(std::move(v)) {}
    void accept(ExprVisitor& v) override { v.visit(*this); }
};

// obj.field = value
class SetMemberExpr : public Expr {
public:
    ExprPtr object;
    Token   name;
    ExprPtr value;
    SetMemberExpr(ExprPtr o, Token n, ExprPtr v)
        : object(std::move(o)), name(n), value(std::move(v)) {}
    void accept(ExprVisitor& v) override { v.visit(*this); }
};

// super.method   (or bare `super` followed by `(args)`, which the parser
// rewrites as SuperExpr with method.lexeme == "init")
//
// Only legal inside a method body of a class declared with `extends`. The
// runtime resolves super.method by walking up the parent-class chain via
// find_method, then binds it to the current `self`.
class SuperExpr : public Expr {
public:
    Token keyword;   // the `super` token, kept for error reporting
    Token method;    // identifier after the dot, or synthetic "init" for bare super
    SuperExpr(Token k, Token m) : keyword(k), method(m) {}
    void accept(ExprVisitor& v) override { v.visit(*this); }
};

// ---- statement nodes --------------------------------------------------------

class ExpressionStmt : public Stmt {
public:
    ExprPtr expr;
    explicit ExpressionStmt(ExprPtr e) : expr(std::move(e)) {}
    void accept(StmtVisitor& v) override { v.visit(*this); }
};

class VarStmt : public Stmt {
public:
    Token   name;
    ExprPtr initializer;  // may be null
    VarStmt(Token n, ExprPtr i) : name(n), initializer(std::move(i)) {}
    void accept(StmtVisitor& v) override { v.visit(*this); }
};

class BlockStmt : public Stmt {
public:
    std::vector<StmtPtr> statements;
    explicit BlockStmt(std::vector<StmtPtr> s) : statements(std::move(s)) {}
    void accept(StmtVisitor& v) override { v.visit(*this); }
};

class IfStmt : public Stmt {
public:
    ExprPtr cond;
    StmtPtr then_branch;
    StmtPtr else_branch;  // may be null
    IfStmt(ExprPtr c, StmtPtr t, StmtPtr e)
        : cond(std::move(c)), then_branch(std::move(t)), else_branch(std::move(e)) {}
    void accept(StmtVisitor& v) override { v.visit(*this); }
};

class WhileStmt : public Stmt {
public:
    ExprPtr cond;
    StmtPtr body;
    WhileStmt(ExprPtr c, StmtPtr b) : cond(std::move(c)), body(std::move(b)) {}
    void accept(StmtVisitor& v) override { v.visit(*this); }
};

class FunctionStmt : public Stmt {
public:
    Token                name;
    std::vector<Token>   params;
    std::vector<StmtPtr> body;
    FunctionStmt(Token n, std::vector<Token> p, std::vector<StmtPtr> b)
        : name(n), params(std::move(p)), body(std::move(b)) {}
    void accept(StmtVisitor& v) override { v.visit(*this); }
};

class ReturnStmt : public Stmt {
public:
    Token   keyword;
    ExprPtr value;  // may be null
    ReturnStmt(Token k, ExprPtr v) : keyword(k), value(std::move(v)) {}
    void accept(StmtVisitor& v) override { v.visit(*this); }
};

class ImportStmt : public Stmt {
public:
    Token keyword;     // 'import' / 'আমদানি'
    Token path_token;  // String literal (lexeme includes quotes)
    Token alias;       // Identifier bound to the imported module
    ImportStmt(Token k, Token p, Token a) : keyword(k), path_token(p), alias(a) {}
    void accept(StmtVisitor& v) override { v.visit(*this); }
};

// class Foo extends Bar { function method(self, ...) { ... } }
// Methods are FunctionStmts so they reuse the existing function machinery —
// each is bound at evaluation time as a Callable in the class's method
// dictionary. The first parameter is conventionally `self`; calling
// `instance.method(args)` prepends the instance via a BoundMethod wrapper.
//
// `extends` is optional. If superclass.lexeme is empty, the class has no
// parent. If non-empty, the runtime looks up the named class at definition
// time and chains parent for method-resolution + super support.
class ClassStmt : public Stmt {
public:
    Token                                      name;
    Token                                      superclass;   // lexeme empty if none
    std::vector<std::unique_ptr<FunctionStmt>> methods;
    ClassStmt(Token n, Token s, std::vector<std::unique_ptr<FunctionStmt>> m)
        : name(n), superclass(s), methods(std::move(m)) {}
    void accept(StmtVisitor& v) override { v.visit(*this); }
};

// try { ... } catch (e) { ... }
//
// On a runtime error or `throw` inside try_block, control jumps to catch_block
// with `e` (the catch_var lexeme) bound in a fresh scope. The bound value is:
//   - the value passed to `throw <expr>;`           (any bnl value)
//   - or the runtime-error message string           (for runtime-origin errors)
class TryStmt : public Stmt {
public:
    Token                keyword;     // 'try' / 'চেষ্টা' — for diagnostics
    std::vector<StmtPtr> try_block;
    Token                catch_var;   // identifier bound inside catch_block
    std::vector<StmtPtr> catch_block;
    TryStmt(Token k, std::vector<StmtPtr> t, Token cv, std::vector<StmtPtr> c)
        : keyword(k), try_block(std::move(t)),
          catch_var(cv), catch_block(std::move(c)) {}
    void accept(StmtVisitor& v) override { v.visit(*this); }
};

// throw <expr>;  (or 'নিক্ষেপ <expr>;')
class ThrowStmt : public Stmt {
public:
    Token   keyword;   // 'throw' — kept for error reporting
    ExprPtr value;
    ThrowStmt(Token k, ExprPtr v) : keyword(k), value(std::move(v)) {}
    void accept(StmtVisitor& v) override { v.visit(*this); }
};

}  // namespace bnl
