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
class CompoundAssignExpr;
class CallExpr;
class MemberExpr;
class FunctionExpr;
class ListExpr;
class MapExpr;
class IndexExpr;
class SetIndexExpr;
class SetMemberExpr;
class SuperExpr;
class WaitExpr;

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
class ForStmt;
class ForOfStmt;
class SwitchStmt;
class BreakStmt;
class ContinueStmt;

using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;

// Function parameter. `default_value` is null for required params; for
// optional params it's an expression evaluated lazily on each call (in the
// function's call env, after earlier params have been bound — so a default
// can reference earlier param names).
struct Param {
    Token   name;
    ExprPtr default_value;   // may be null
    Param(Token n, ExprPtr d) : name(n), default_value(std::move(d)) {}
};

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
    virtual void visit(CompoundAssignExpr&) = 0;
    virtual void visit(CallExpr&)       = 0;
    virtual void visit(MemberExpr&)     = 0;
    virtual void visit(FunctionExpr&)   = 0;
    virtual void visit(ListExpr&)       = 0;
    virtual void visit(MapExpr&)        = 0;
    virtual void visit(IndexExpr&)      = 0;
    virtual void visit(SetIndexExpr&)   = 0;
    virtual void visit(SetMemberExpr&)  = 0;
    virtual void visit(SuperExpr&)      = 0;
    virtual void visit(WaitExpr&)       = 0;
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
    virtual void visit(ForStmt&)        = 0;
    virtual void visit(ForOfStmt&)      = 0;
    virtual void visit(SwitchStmt&)     = 0;
    virtual void visit(BreakStmt&)      = 0;
    virtual void visit(ContinueStmt&)   = 0;
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
    // Set by the parser when this literal was synthesized (e.g. an interpolated
    // string segment whose bytes don't live in the source view). When non-null
    // the interpreter uses this directly instead of decoding `value.lexeme`.
    std::unique_ptr<std::string> precomputed_string;
    explicit LiteralExpr(Token v) : value(v) {}
    LiteralExpr(Token v, std::string s)
        : value(v), precomputed_string(std::make_unique<std::string>(std::move(s))) {}
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

// x op= v       (op is Plus/Minus/Star/Slash/Percent, kept on op_token)
// x++ / ++x     (parser desugars to op=Plus, value=1; same for --)
//
// `target` is one of IdentifierExpr / MemberExpr / IndexExpr — the parser
// enforces that. The interpreter evaluates target's object and index parts
// exactly once, so `obj[fn()] += 1` calls fn() once.
class CompoundAssignExpr : public Expr {
public:
    ExprPtr target;
    Token   op;     // the underlying binary op (Plus, Minus, Star, Slash, Percent)
    ExprPtr value;
    CompoundAssignExpr(ExprPtr t, Token o, ExprPtr v)
        : target(std::move(t)), op(o), value(std::move(v)) {}
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
    std::vector<Param>   params;
    std::vector<StmtPtr> body;
    FunctionExpr(std::string n, std::vector<Param> p, std::vector<StmtPtr> b)
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

// wait expr  — suspends the enclosing async function until `expr` (a Future)
// settles. Only legal as the initializer of a top-level VarStmt, or as the
// sole expression of a top-level ExpressionStmt. The interpreter enforces
// this; nested or sub-expression uses throw at runtime.
class WaitExpr : public Expr {
public:
    Token   keyword;   // the 'wait' token, kept for diagnostics
    ExprPtr operand;
    WaitExpr(Token k, ExprPtr e) : keyword(k), operand(std::move(e)) {}
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
    Token   keyword;   // 'while' — kept for interrupt / error reporting
    ExprPtr cond;
    StmtPtr body;
    WhileStmt(Token k, ExprPtr c, StmtPtr b)
        : keyword(k), cond(std::move(c)), body(std::move(b)) {}
    void accept(StmtVisitor& v) override { v.visit(*this); }
};

// for (init; cond; update) body  — C-style. All three clauses are optional.
//   init   : a VarStmt or ExpressionStmt (or null)
//   cond   : an Expr (or null = always true)
//   update : an Expr (or null = no-op)
class ForStmt : public Stmt {
public:
    Token   keyword;   // 'for' — kept for interrupt / error reporting
    StmtPtr init;
    ExprPtr cond;
    ExprPtr update;
    StmtPtr body;
    ForStmt(Token k, StmtPtr i, ExprPtr c, ExprPtr u, StmtPtr b)
        : keyword(k), init(std::move(i)), cond(std::move(c)),
          update(std::move(u)), body(std::move(b)) {}
    void accept(StmtVisitor& v) override { v.visit(*this); }
};

// for (var X of EXPR) body  — iterates the elements of a list (other types
// throw at runtime). The var is bound fresh each iteration in a new scope.
class ForOfStmt : public Stmt {
public:
    Token   var;          // identifier introduced by `var X`
    ExprPtr iterable;
    StmtPtr body;
    ForOfStmt(Token v, ExprPtr it, StmtPtr b)
        : var(v), iterable(std::move(it)), body(std::move(b)) {}
    void accept(StmtVisitor& v) override { v.visit(*this); }
};

class FunctionStmt : public Stmt {
public:
    Token                name;
    std::vector<Param>   params;
    std::vector<StmtPtr> body;
    FunctionStmt(Token n, std::vector<Param> p, std::vector<StmtPtr> b)
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

// try { ... } catch (e) { ... } finally { ... }
//
// At least one of `catch` or `finally` must be present. The catch block runs
// only when try_block raised; `e` is bound to the thrown value, or to the
// error message string for runtime-origin errors.
//
// The finally block runs ALWAYS — after try succeeds, after catch handles a
// throw, when an unhandled throw is propagating out, and when `return` is
// unwinding. If finally itself throws or returns, that overrides any pending
// exception/return.
class TryStmt : public Stmt {
public:
    Token                keyword;       // 'try' / 'চেষ্টা' — for diagnostics
    std::vector<StmtPtr> try_block;
    bool                 has_catch;     // false for `try { } finally { }`
    Token                catch_var;     // identifier bound inside catch_block
    std::vector<StmtPtr> catch_block;
    bool                 has_finally;
    std::vector<StmtPtr> finally_block;
    TryStmt(Token k, std::vector<StmtPtr> t,
            bool hc, Token cv, std::vector<StmtPtr> c,
            bool hf, std::vector<StmtPtr> f)
        : keyword(k), try_block(std::move(t)),
          has_catch(hc), catch_var(cv), catch_block(std::move(c)),
          has_finally(hf), finally_block(std::move(f)) {}
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

// switch (subject) { case X: { ... } case Y: case Z: { ... } default: { ... } }
//
// No fall-through: each block runs at most once. Multi-value matching is
// expressed by stacking `case`s before a block — a SwitchCase with multiple
// `values` and one shared `body`. If no case matches and a `default_body` is
// present, it runs.
struct SwitchCase {
    std::vector<ExprPtr> values;   // one or more case expressions sharing the body
    std::vector<StmtPtr> body;     // statements inside the `{ ... }` block
};

class SwitchStmt : public Stmt {
public:
    Token                    keyword;          // 'switch' / 'বিকল্প'
    ExprPtr                  subject;
    std::vector<SwitchCase>  cases;
    bool                     has_default;
    std::vector<StmtPtr>     default_body;
    SwitchStmt(Token k, ExprPtr s, std::vector<SwitchCase> cs,
               bool hd, std::vector<StmtPtr> db)
        : keyword(k), subject(std::move(s)), cases(std::move(cs)),
          has_default(hd), default_body(std::move(db)) {}
    void accept(StmtVisitor& v) override { v.visit(*this); }
};

// break;   — exits the nearest enclosing while / for / for-of / switch.
class BreakStmt : public Stmt {
public:
    Token keyword;   // 'break' / 'থামুন' — for error reporting
    explicit BreakStmt(Token k) : keyword(k) {}
    void accept(StmtVisitor& v) override { v.visit(*this); }
};

// continue;  — skips to the next iteration of the nearest enclosing loop.
// (Does NOT match a switch — continue in a switch propagates to an outer loop.)
class ContinueStmt : public Stmt {
public:
    Token keyword;   // 'continue' / 'চলুন'
    explicit ContinueStmt(Token k) : keyword(k) {}
    void accept(StmtVisitor& v) override { v.visit(*this); }
};

}  // namespace bnl
