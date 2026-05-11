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
//
// `token` is the `throw` keyword from the source — used to render a clang-
// style diagnostic when an uncaught throw escapes the program.
struct ThrowSignal {
    Value value;
    Token token;
};

// Unwind out of the nearest enclosing while / for / for-of / switch when a
// `break;` runs. `token` is the keyword, used to render a clean diagnostic
// if a `break` escapes its enclosing construct.
struct BreakSignal {
    Token token;
};

// Unwind out of one iteration of the nearest enclosing loop when `continue;`
// runs. Does not unwind switch — a switch inside a loop lets `continue` pass
// through to the loop.
struct ContinueSignal {
    Token token;
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

    // Run an already-parsed program. The Interpreter takes ownership of the
    // AST (via std::move) — UserFunctions and other closures hold raw
    // pointers into it, so the AST must outlive any function value defined
    // during the run. The Interpreter retains every program until its
    // destruction. Pass an rvalue:  interp.run(std::move(program), entry).
    bool run(std::vector<StmtPtr>            program,
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

    // Signal-handler-safe wake. Sets the loop's stop flag AND triggers a
    // uv_async_send so the loop wakes from any IOCP / epoll / kqueue wait.
    // Called from the Ctrl-C handler — without the async wake, an idle
    // server (no I/O activity) sleeps through the stop request indefinitely.
    void notify_stop();

    // ---- recursion + interrupt hardening ----------------------------------

    // Maximum bnl call-stack depth before the runtime preemptively unwinds
    // with a RuntimeError. Tracked in visit(CallExpr); covers UserFunction +
    // any callable invoked via a bnl call expression. Default 100 — sized
    // to clear Windows' 1 MB default native stack with margin, since the
    // tree-walker uses several C++ frames per bnl frame. Hosts on bigger
    // stacks (Linux 8 MB) can safely raise this.
    int  max_call_depth() const          { return max_call_depth_; }
    void set_max_call_depth(int n)       { max_call_depth_ = n; }

    // Process-wide install — call once at startup. Wires Ctrl-C / SIGTERM so
    // a running script unwinds with a "interrupted" RuntimeError instead of
    // tearing the process down mid-state. The handler also calls uv_stop on
    // the active interpreter's loop so async waits return promptly.
    static void install_signal_handlers();

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
    void visit(ForStmt&)        override;
    void visit(ForOfStmt&)      override;
    void visit(SwitchStmt&)     override;
    void visit(BreakStmt&)      override;
    void visit(ContinueStmt&)   override;

private:
    Value evaluate(Expr& e);
    void  execute(Stmt& s);
    void  register_builtins();

    // Throws a RuntimeError if the bnl call stack has hit max_call_depth_.
    void  check_call_depth(const Token& site);
    // Throws a RuntimeError if a Ctrl-C / SIGTERM has been delivered.
    void  check_interrupt (const Token& site);

    std::shared_ptr<Environment> globals_;
    std::shared_ptr<Environment> environment_;
    Value                        result_;

    std::filesystem::path                          current_file_;
    std::unique_ptr<ModuleLoader>                  modules_;
    std::unordered_map<std::string, ModulePtr>     native_modules_;
    std::vector<std::string>                       program_args_;

    // Programs accumulated across successive run() calls. Owns the AST so
    // UserFunctions defined in earlier calls (REPL state, async timer
    // closures, ...) keep valid pointers to their params/body.
    std::vector<std::vector<StmtPtr>>              kept_programs_;
    uv_loop_t                                      loop_{};
    uv_async_t                                     stop_async_{};   // wakes the loop on Ctrl-C
    bool                                           loop_failed_ = false;

    int                                            call_depth_     = 0;
    // The tree-walker burns several KB of C++ stack per bnl frame. Debug
    // builds use ~3-4× more stack than Release, so we ship a conservative
    // default that's safe in Debug on Windows' 1 MB default stack and
    // bump it up for Release. Hosts can override via set_max_call_depth.
#if defined(_DEBUG) || !defined(NDEBUG)
    int                                            max_call_depth_ = 40;
#else
    int                                            max_call_depth_ = 200;
#endif
};

}  // namespace bnl
