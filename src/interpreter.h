#pragma once

#include <uv.h>

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "ast.h"
#include "environment.h"
#include "module.h"
#include "module_loader.h"
#include "token.h"
#include "value.h"

namespace bnl {

class RuntimeError : public std::runtime_error {
public:
    Token token;
    RuntimeError(Token t, std::string msg)
        : std::runtime_error(std::move(msg)), token(t) {}
};

// Used to unwind out of a function body when a `return` statement runs.
struct ReturnSignal {
    Value value;
};

class Interpreter : public ExprVisitor, public StmtVisitor {
public:
    Interpreter();
    ~Interpreter();

    Interpreter(const Interpreter&)            = delete;
    Interpreter& operator=(const Interpreter&) = delete;

    uv_loop_t* loop() { return &loop_; }

    // Called from libuv callbacks when a bnl callback throws — flips run()
    // into a non-zero exit. Stops the loop too so uv_run returns promptly.
    void mark_loop_failed();

    // Run an entry-point program. `entry_path` is used as the starting point
    // for resolving relative imports (empty = current working directory).
    // Returns true on success, false if a runtime/module error occurred.
    bool run(const std::vector<StmtPtr>& program,
             const std::filesystem::path& entry_path = {});

    // Used by UserFunction::call.
    void execute_block(const std::vector<StmtPtr>& stmts,
                       std::shared_ptr<Environment> env);

    // Used by ModuleLoader once a module has been parsed: evaluates its
    // program inside its own exports environment, with current_file_ pointing
    // at the module path so relative re-imports resolve against the module's
    // directory.
    void run_module(Module& m);

    std::shared_ptr<Environment> globals() const { return globals_; }

    // Native module registry. ModuleLoader consults this for bare-name imports
    // before falling back to embedded bnl stdlib.
    void      register_native_module(const std::string& name, ModulePtr m);
    ModulePtr native_module(const std::string& name) const;

    // Args passed to the bnl program after the script path.
    void                            set_program_args(std::vector<std::string> args) { program_args_ = std::move(args); }
    const std::vector<std::string>& program_args() const { return program_args_; }

private:
    Value evaluate(Expr& e);
    void  execute(Stmt& s);

    // ---- expressions ------------------------------------------------------
    void visit(LiteralExpr&)    override;
    void visit(IdentifierExpr&) override;
    void visit(GroupingExpr&)   override;
    void visit(UnaryExpr&)      override;
    void visit(BinaryExpr&)     override;
    void visit(LogicalExpr&)    override;
    void visit(AssignExpr&)     override;
    void visit(CallExpr&)       override;
    void visit(MemberExpr&)     override;
    void visit(FunctionExpr&)   override;
    void visit(ListExpr&)       override;
    void visit(MapExpr&)        override;
    void visit(IndexExpr&)      override;
    void visit(SetIndexExpr&)   override;
    void visit(SetMemberExpr&)  override;

    // ---- statements -------------------------------------------------------
    void visit(ExpressionStmt&) override;
    void visit(VarStmt&)        override;
    void visit(BlockStmt&)      override;
    void visit(IfStmt&)         override;
    void visit(WhileStmt&)      override;
    void visit(FunctionStmt&)   override;
    void visit(ReturnStmt&)     override;
    void visit(ImportStmt&)     override;

    void register_builtins();

    std::shared_ptr<Environment> globals_;
    std::shared_ptr<Environment> environment_;
    Value                        result_;

    std::filesystem::path        current_file_;  // for relative import resolution
    ModuleLoader                 modules_;
    std::unordered_map<std::string, ModulePtr> native_modules_;
    std::vector<std::string>     program_args_;
    uv_loop_t                    loop_{};       // event loop owned by the interpreter
    bool                         loop_failed_ = false;  // set by async callbacks on uncaught error
};

}  // namespace bnl
