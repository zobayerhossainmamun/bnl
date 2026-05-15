#pragma once

// Private to src/interpreter/*.cpp. Holds declarations that need to be shared
// across the runtime/exprs/stmts/builtins translation units but should not
// leak into the public interpreter.h surface.

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "bnl/ast.h"
#include "bnl/interpreter.h"
#include "bnl/token.h"
#include "bnl/value.h"
#include "runtime/environment.h"

namespace bnl::interp_detail {

// Decode a string-literal token's lexeme (strips the surrounding quotes and
// resolves \n \t \r \\ \" \' \0 escapes). Used by LiteralExpr's String branch
// and by ImportStmt for the import path. Accepts either "..." or '...' — the
// pair must match.
inline std::string decode_string_literal(const Token& tok) {
    auto sv = tok.lexeme;
    char quote = sv.size() >= 2 ? sv.front() : '\0';
    if (sv.size() < 2 || (quote != '"' && quote != '\'') || sv.back() != quote) {
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
            case '\'': out += '\''; break;
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

// Walks an expression subtree and sets `found = true` if any WaitExpr is
// encountered. Stops descending into FunctionExpr — a nested function is its
// own async/sync scope and doesn't make its parent async.
class WaitFinder : public ExprVisitor {
public:
    bool found = false;
    void visit(LiteralExpr&)    override {}
    void visit(IdentifierExpr&) override {}
    void visit(GroupingExpr& e) override { e.inner->accept(*this); }
    void visit(UnaryExpr& e)    override { e.operand->accept(*this); }
    void visit(BinaryExpr& e)   override { e.left->accept(*this); e.right->accept(*this); }
    void visit(LogicalExpr& e)  override { e.left->accept(*this); e.right->accept(*this); }
    void visit(AssignExpr& e)   override { e.value->accept(*this); }
    void visit(CompoundAssignExpr& e) override {
        e.target->accept(*this);
        e.value->accept(*this);
    }
    void visit(CallExpr& e)     override {
        e.callee->accept(*this);
        for (auto& a : e.arguments) a->accept(*this);
    }
    void visit(MemberExpr& e)   override { e.object->accept(*this); }
    void visit(FunctionExpr&)   override { /* nested fn has its own scope */ }
    void visit(ListExpr& e)     override { for (auto& el : e.elements) el->accept(*this); }
    void visit(MapExpr& e)      override { for (auto& kv : e.entries) kv.second->accept(*this); }
    void visit(IndexExpr& e)    override { e.object->accept(*this); e.index->accept(*this); }
    void visit(SetIndexExpr& e) override {
        e.object->accept(*this);
        e.index->accept(*this);
        e.value->accept(*this);
    }
    void visit(SetMemberExpr& e) override { e.object->accept(*this); e.value->accept(*this); }
    void visit(SuperExpr&)      override {}
    void visit(WaitExpr&)       override { found = true; }
};

inline bool expr_contains_wait(Expr& e) {
    WaitFinder f;
    e.accept(f);
    return f.found;
}

bool stmts_contain_wait(const std::vector<StmtPtr>& stmts);

inline bool stmt_contains_wait(Stmt& s) {
    if (auto* es = dynamic_cast<ExpressionStmt*>(&s)) return expr_contains_wait(*es->expr);
    if (auto* vs = dynamic_cast<VarStmt*>(&s))
        return vs->initializer && expr_contains_wait(*vs->initializer);
    if (auto* bs = dynamic_cast<BlockStmt*>(&s))    return stmts_contain_wait(bs->statements);
    if (auto* is = dynamic_cast<IfStmt*>(&s)) {
        if (expr_contains_wait(*is->cond)) return true;
        if (stmt_contains_wait(*is->then_branch)) return true;
        if (is->else_branch && stmt_contains_wait(*is->else_branch)) return true;
        return false;
    }
    if (auto* ws = dynamic_cast<WhileStmt*>(&s))
        return expr_contains_wait(*ws->cond) || stmt_contains_wait(*ws->body);
    if (dynamic_cast<FunctionStmt*>(&s))    return false;   // its own scope
    if (auto* rs = dynamic_cast<ReturnStmt*>(&s))
        return rs->value && expr_contains_wait(*rs->value);
    if (dynamic_cast<ImportStmt*>(&s))      return false;
    if (dynamic_cast<ClassStmt*>(&s))       return false;
    if (auto* ts = dynamic_cast<TryStmt*>(&s)) {
        if (stmts_contain_wait(ts->try_block)) return true;
        if (ts->has_catch   && stmts_contain_wait(ts->catch_block))   return true;
        if (ts->has_finally && stmts_contain_wait(ts->finally_block)) return true;
        return false;
    }
    if (auto* th = dynamic_cast<ThrowStmt*>(&s)) return expr_contains_wait(*th->value);
    if (auto* fs = dynamic_cast<ForStmt*>(&s)) {
        if (fs->init   && stmt_contains_wait(*fs->init))   return true;
        if (fs->cond   && expr_contains_wait(*fs->cond))   return true;
        if (fs->update && expr_contains_wait(*fs->update)) return true;
        return stmt_contains_wait(*fs->body);
    }
    if (auto* fos = dynamic_cast<ForOfStmt*>(&s))
        return expr_contains_wait(*fos->iterable) || stmt_contains_wait(*fos->body);
    if (auto* ss = dynamic_cast<SwitchStmt*>(&s)) {
        if (expr_contains_wait(*ss->subject)) return true;
        for (auto& c : ss->cases) {
            for (auto& v : c.values) if (expr_contains_wait(*v)) return true;
            if (stmts_contain_wait(c.body)) return true;
        }
        if (ss->has_default && stmts_contain_wait(ss->default_body)) return true;
        return false;
    }
    return false;
}

inline bool stmts_contain_wait(const std::vector<StmtPtr>& stmts) {
    for (const auto& s : stmts) if (stmt_contains_wait(*s)) return true;
    return false;
}

// Backs both `function` declarations and anonymous `function (...) {...}`
// expressions. Holds raw pointers into the AST nodes that own the params and
// body vectors (the AST outlives every closure that references it).
//
// At construction time we scan the body for any WaitExpr — if found, the
// function is "implicitly async": calling it returns a Future, the body
// executes step-by-step through Interpreter::run_async_body, and the Future
// settles when the body returns or throws.
class UserFunction : public Callable {
public:
    UserFunction(std::string                  name,
                 const std::vector<Param>*    params,
                 const std::vector<StmtPtr>*  body,
                 std::shared_ptr<Environment> closure)
        : name_(std::move(name)), params_(params), body_(body),
          closure_(std::move(closure)),
          is_async_(stmts_contain_wait(*body)),
          min_arity_(compute_min_arity(*params)) {}

    int         arity()     const override { return static_cast<int>(params_->size()); }
    int         min_arity() const override { return min_arity_; }
    std::string name()      const override { return name_; }
    bool        is_async()  const          { return is_async_; }

    Value call(Interpreter& interp, std::vector<Value> args) override {
        auto env = std::make_shared<Environment>(closure_);
        for (std::size_t i = 0; i < params_->size(); ++i) {
            Value v;
            if (i < args.size()) {
                v = std::move(args[i]);
            } else if ((*params_)[i].default_value) {
                // Evaluate default in the function's call env so it can
                // reference earlier params (already bound below).
                v = interp.evaluate_in(*(*params_)[i].default_value, env);
            } else {
                // Shouldn't happen — the arity check in CallExpr ensures
                // at least min_arity args. Defensive.
                v = Value{};
            }
            env->define(std::string((*params_)[i].name.lexeme), std::move(v));
        }
        if (is_async_) {
            return Value{interp.run_async_body(body_, std::move(env))};
        }
        try {
            interp.execute_block(*body_, env);
        } catch (ReturnSignal& r) {
            return std::move(r.value);
        }
        return Value{};  // implicit null
    }

private:
    static int compute_min_arity(const std::vector<Param>& ps) {
        int n = 0;
        for (const auto& p : ps) {
            if (p.default_value) break;
            ++n;
        }
        return n;
    }

    std::string                  name_;
    const std::vector<Param>*    params_;
    const std::vector<StmtPtr>*  body_;
    std::shared_ptr<Environment> closure_;
    bool                         is_async_;
    int                          min_arity_;
};

}  // namespace bnl::interp_detail
