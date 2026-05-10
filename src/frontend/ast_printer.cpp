#include "frontend/ast_printer.h"

#include <fmt/core.h>

#include <string>
#include <string_view>

#include "bnl/ast.h"

namespace bnl {

namespace {

class AstPrinter : public ExprVisitor, public StmtVisitor {
public:
    void print_program(const std::vector<StmtPtr>& stmts) {
        for (const auto& s : stmts) {
            s->accept(*this);
            out_ += '\n';
        }
    }

    std::string take() { return std::move(out_); }

    // ---- expression visits -------------------------------------------------

    void visit(LiteralExpr& e) override {
        switch (e.value.type) {
            case TokenType::Null:   out_ += "null"; break;
            case TokenType::True:   out_ += "true"; break;
            case TokenType::False:  out_ += "false"; break;
            default:                out_ += e.value.lexeme;  // raw lexeme for Number/String
        }
    }

    void visit(IdentifierExpr& e) override {
        out_ += e.name.lexeme;
    }

    void visit(GroupingExpr& e) override {
        out_ += "(group ";
        e.inner->accept(*this);
        out_ += ')';
    }

    void visit(UnaryExpr& e) override {
        out_ += '(';
        out_ += e.op.lexeme;
        out_ += ' ';
        e.operand->accept(*this);
        out_ += ')';
    }

    void visit(BinaryExpr& e) override {
        out_ += '(';
        out_ += e.op.lexeme;
        out_ += ' ';
        e.left->accept(*this);
        out_ += ' ';
        e.right->accept(*this);
        out_ += ')';
    }

    void visit(LogicalExpr& e) override {
        out_ += '(';
        out_ += e.op.lexeme;
        out_ += ' ';
        e.left->accept(*this);
        out_ += ' ';
        e.right->accept(*this);
        out_ += ')';
    }

    void visit(AssignExpr& e) override {
        out_ += "(assign ";
        out_ += e.name.lexeme;
        out_ += ' ';
        e.value->accept(*this);
        out_ += ')';
    }

    void visit(CallExpr& e) override {
        out_ += "(call ";
        e.callee->accept(*this);
        for (const auto& a : e.arguments) {
            out_ += ' ';
            a->accept(*this);
        }
        out_ += ')';
    }

    void visit(MemberExpr& e) override {
        out_ += "(. ";
        e.object->accept(*this);
        out_ += ' ';
        out_ += e.name.lexeme;
        out_ += ')';
    }

    void visit(FunctionExpr& e) override {
        out_ += fmt::format("(fn {} (params", e.name.empty() ? "<anon>" : e.name);
        for (const auto& p : e.params) { out_ += ' '; out_ += p.lexeme; }
        out_ += ")";
        depth_++;
        for (const auto& st : e.body) { out_ += '\n'; st->accept(*this); }
        depth_--;
        out_ += ')';
    }

    void visit(ListExpr& e) override {
        out_ += "(list";
        for (auto& el : e.elements) { out_ += ' '; el->accept(*this); }
        out_ += ')';
    }

    void visit(MapExpr& e) override {
        out_ += "(map";
        for (auto& [k, v] : e.entries) {
            out_ += ' ';
            out_ += k;
            out_ += ':';
            v->accept(*this);
        }
        out_ += ')';
    }

    void visit(IndexExpr& e) override {
        out_ += "(index ";
        e.object->accept(*this);
        out_ += ' ';
        e.index->accept(*this);
        out_ += ')';
    }

    void visit(SetIndexExpr& e) override {
        out_ += "(set-index ";
        e.object->accept(*this);
        out_ += ' ';
        e.index->accept(*this);
        out_ += ' ';
        e.value->accept(*this);
        out_ += ')';
    }

    void visit(SetMemberExpr& e) override {
        out_ += "(set-member ";
        e.object->accept(*this);
        out_ += ' ';
        out_ += e.name.lexeme;
        out_ += ' ';
        e.value->accept(*this);
        out_ += ')';
    }

    void visit(SuperExpr& e) override {
        out_ += "(super .";
        out_ += e.method.lexeme;
        out_ += ')';
    }

    // ---- statement visits --------------------------------------------------

    void visit(ExpressionStmt& s) override {
        indent();
        out_ += "(expr ";
        s.expr->accept(*this);
        out_ += ')';
    }

    void visit(VarStmt& s) override {
        indent();
        out_ += "(var ";
        out_ += s.name.lexeme;
        if (s.initializer) {
            out_ += " = ";
            s.initializer->accept(*this);
        }
        out_ += ')';
    }

    void visit(BlockStmt& s) override {
        indent();
        out_ += "(block";
        depth_++;
        for (const auto& st : s.statements) {
            out_ += '\n';
            st->accept(*this);
        }
        depth_--;
        out_ += ')';
    }

    void visit(IfStmt& s) override {
        indent();
        out_ += "(if ";
        s.cond->accept(*this);
        depth_++;
        out_ += '\n';
        s.then_branch->accept(*this);
        if (s.else_branch) {
            out_ += '\n';
            s.else_branch->accept(*this);
        }
        depth_--;
        out_ += ')';
    }

    void visit(WhileStmt& s) override {
        indent();
        out_ += "(while ";
        s.cond->accept(*this);
        depth_++;
        out_ += '\n';
        s.body->accept(*this);
        depth_--;
        out_ += ')';
    }

    void visit(FunctionStmt& s) override {
        indent();
        out_ += fmt::format("(function {} (params", s.name.lexeme);
        for (const auto& p : s.params) {
            out_ += ' ';
            out_ += p.lexeme;
        }
        out_ += ')';
        depth_++;
        for (const auto& st : s.body) {
            out_ += '\n';
            st->accept(*this);
        }
        depth_--;
        out_ += ')';
    }

    void visit(ReturnStmt& s) override {
        indent();
        out_ += "(return";
        if (s.value) {
            out_ += ' ';
            s.value->accept(*this);
        }
        out_ += ')';
    }

    void visit(ImportStmt& s) override {
        indent();
        out_ += fmt::format("(import {} as {})", s.path_token.lexeme, s.alias.lexeme);
    }

    void visit(ClassStmt& s) override {
        indent();
        if (s.superclass.lexeme.size() > 0) {
            out_ += fmt::format("(class {} extends {}", s.name.lexeme, s.superclass.lexeme);
        } else {
            out_ += fmt::format("(class {}", s.name.lexeme);
        }
        depth_++;
        for (auto& m : s.methods) {
            out_ += '\n';
            visit(*m);
        }
        depth_--;
        out_ += ')';
    }

private:
    void indent() {
        for (int i = 0; i < depth_; ++i) out_ += "  ";
    }

    std::string out_;
    int         depth_ = 0;
};

}  // namespace

std::string ast_to_string(const std::vector<StmtPtr>& stmts) {
    AstPrinter p;
    p.print_program(stmts);
    return p.take();
}

}  // namespace bnl
