#include "bnl/interpreter.h"

#include <fmt/core.h>

#include <utility>

#include "runtime/environment.h"
#include "frontend/lexer.h"
#include "runtime/module_loader.h"
#include "frontend/parser.h"
#include "stdlib/registry.h"

namespace bnl {

// ============================================================================
// Interpreter — lifecycle, control, and native module registry.
// (Visitor implementations live in exprs.cpp / stmts.cpp; built-in registration
// in builtins.cpp; private helpers and UserFunction in interpreter/internal.h.)
// ============================================================================

Interpreter::Interpreter()
    : globals_(std::make_shared<Environment>()),
      environment_(globals_),
      modules_(std::make_unique<ModuleLoader>(*this)) {
    uv_loop_init(&loop_);
    register_builtins();
    register_sys   (*this);
    register_io    (*this);
    register_timers(*this);
    register_crypto(*this);
    register_zlib  (*this);
    register_httpp (*this);
    register_net   (*this);
    register_tls   (*this);
    register_exec  (*this);
    register_json  (*this);
}

Interpreter::~Interpreter() {
    // Force-close any handles that survived (e.g. user crashed mid-loop).
    uv_walk(&loop_, [](uv_handle_t* h, void*) {
        if (!uv_is_closing(h)) uv_close(h, nullptr);
    }, nullptr);
    uv_run(&loop_, UV_RUN_DEFAULT);  // drain close callbacks
    uv_loop_close(&loop_);
}

void Interpreter::register_native_module(const std::string& name, ModulePtr m) {
    native_modules_[name] = std::move(m);
}

ModulePtr Interpreter::native_module(const std::string& name) const {
    auto it = native_modules_.find(name);
    return it != native_modules_.end() ? it->second : nullptr;
}

void Interpreter::mark_loop_failed() {
    loop_failed_ = true;
    uv_stop(&loop_);
}

bool Interpreter::run(const std::vector<StmtPtr>& program,
                      const std::filesystem::path& entry_path) {
    current_file_ = entry_path;
    try {
        for (const auto& s : program) execute(*s);
    } catch (const RuntimeError& e) {
        fmt::print(stderr, "runtime error at {}:{} (near '{}'): {}\n",
                   e.token.line, e.token.column, e.token.lexeme, e.what());
        return false;
    } catch (const ModuleError& e) {
        fmt::print(stderr, "module error at {}:{} (near '{}'): {}\n",
                   e.token.line, e.token.column, e.token.lexeme, e.what());
        return false;
    }

    // Drain the event loop. Async callbacks (timers, fs, net, ...) run here.
    // If any of them sets loop_failed_, the program exits non-zero.
    uv_run(&loop_, UV_RUN_DEFAULT);
    return !loop_failed_;
}

bool Interpreter::run_source(const std::string&            source,
                             const std::filesystem::path&  path) {
    Lexer lexer(source);
    auto  tokens = lexer.tokenize();
    if (lexer.has_errors()) {
        for (const auto& d : lexer.diagnostics()) {
            fmt::print(stderr, "lex error at {}:{}: {}\n", d.line, d.column, d.message);
        }
        return false;
    }

    Parser parser(std::move(tokens));
    auto   program = parser.parse();
    if (parser.has_errors()) {
        for (const auto& d : parser.diagnostics()) {
            fmt::print(stderr, "parse error at {}:{}: {}\n", d.line, d.column, d.message);
        }
        return false;
    }

    return run(program, path);
}

void Interpreter::run_module(Module& m) {
    auto previous_file = current_file_;
    current_file_      = m.path();
    try {
        execute_block(m.program(), m.exports());
    } catch (...) {
        current_file_ = previous_file;
        throw;
    }
    current_file_ = previous_file;
}

Value Interpreter::evaluate(Expr& e) {
    e.accept(*this);
    return std::move(result_);
}

void Interpreter::execute(Stmt& s) { s.accept(*this); }

void Interpreter::execute_block(const std::vector<StmtPtr>& stmts,
                                std::shared_ptr<Environment> env) {
    auto previous = environment_;
    environment_ = std::move(env);
    try {
        for (const auto& s : stmts) execute(*s);
    } catch (...) {
        environment_ = previous;
        throw;
    }
    environment_ = previous;
}

}  // namespace bnl
