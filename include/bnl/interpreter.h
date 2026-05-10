#pragma once

// The bnl embedding API.
//
// A C++ host that wants to run bnl code creates an Interpreter, optionally
// registers custom NativeModules via the builder in <bnl/native_module.h>,
// and calls `run_source(...)` (or `run(...)` if it has a parsed AST already).
//
// Internal types (Environment, ModuleLoader, lexer, parser, AST visitors)
// live under src/ and are not part of this surface; hosts should NOT include
// anything outside <bnl/...>.

#include <uv.h>

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "bnl/ast.h"
#include "bnl/module.h"
#include "bnl/token.h"
#include "bnl/value.h"

namespace bnl {

// Forward-declared internals — opaque to hosts.
class Environment;
class ModuleLoader;

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

// Used to unwind out to the nearest `try { ... } catch (e) { ... }` when a
// `throw <expr>;` runs. `value` is whatever the script threw (any bnl value).
// Distinct from RuntimeError: a RuntimeError is a runtime-origin failure with
// only a message; ThrowSignal carries a user-supplied value. TryStmt catches
// both — for RuntimeError it binds `e` to the message string.
struct ThrowSignal {
    Value value;
};

class Interpreter : public ExprVisitor, public StmtVisitor {
public:
    Interpreter();
    ~Interpreter();

    Interpreter(const Interpreter&)            = delete;
    Interpreter& operator=(const Interpreter&) = delete;

    // ---- public entry points -----------------------------------------------

    // Lex + parse + run a source string. The Lua-style "do this script" entry
    // point most embedders want. `path` is used as the starting point for
    // resolving relative imports inside the source (empty = cwd).
    bool run_source(const std::string&            source,
                    const std::filesystem::path&  path = {});

    // Run an already-parsed program. run_source() is the higher-level path.
    bool run(const std::vector<StmtPtr>&     program,
             const std::filesystem::path&    entry_path = {});

    // Register a custom native module so user bnl code can `import "name"`.
    void      register_native_module(const std::string& name, ModulePtr m);
    ModulePtr native_module(const std::string& name) const;

    // CLI args forwarded into `sys.arg(i)` etc.
    void                            set_program_args(std::vector<std::string> args) { program_args_ = std::move(args); }
    const std::vector<std::string>& program_args() const { return program_args_; }

    // libuv loop owned by this interpreter. Native modules register handles
    // here. Most hosts do not need to touch this directly.
    uv_loop_t* loop() { return &loop_; }

    // Called from libuv callbacks when a bnl callback throws — flips run()
    // into a non-zero exit and stops the loop so uv_run returns promptly.
    void mark_loop_failed();

    // ---- internal: visitor dispatch + module engine ------------------------
    // These are public only because the visitor base classes dispatch into
    // them and ModuleLoader calls back into them. Hosts should NOT call these
    // directly — they are not part of the embedding contract.

    void                         execute_block(const std::vector<StmtPtr>& stmts,
                                               std::shared_ptr<Environment> env);
    void                         run_module(Module& m);
    std::shared_ptr<Environment> globals() const { return globals_; }

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
    void visit(SuperExpr&)      override;

    void visit(ExpressionStmt&) override;
    void visit(VarStmt&)        override;
    void visit(BlockStmt&)      override;
    void visit(IfStmt&)         override;
    void visit(WhileStmt&)      override;
    void visit(FunctionStmt&)   override;
    void visit(ReturnStmt&)     override;
    void visit(ImportStmt&)     override;
    void visit(ClassStmt&)      override;
    void visit(TryStmt&)        override;
    void visit(ThrowStmt&)      override;

private:
    Value evaluate(Expr& e);
    void  execute(Stmt& s);
    void  register_builtins();

    std::shared_ptr<Environment> globals_;
    std::shared_ptr<Environment> environment_;
    Value                        result_;

    std::filesystem::path                          current_file_;
    std::unique_ptr<ModuleLoader>                  modules_;
    std::unordered_map<std::string, ModulePtr>     native_modules_;
    std::vector<std::string>                       program_args_;
    uv_loop_t                                      loop_{};
    bool                                           loop_failed_ = false;
};

}  // namespace bnl
