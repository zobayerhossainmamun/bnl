#include "bnl/interpreter.h"
#include "interpreter/internal.h"

#include <filesystem>
#include <memory>
#include <string>
#include <utility>

#include "module_loader.h"

namespace bnl {

void Interpreter::visit(ExpressionStmt& s) { evaluate(*s.expr); }

void Interpreter::visit(VarStmt& s) {
    Value v = s.initializer ? evaluate(*s.initializer) : Value{};
    environment_->define(std::string(s.name.lexeme), std::move(v));
}

void Interpreter::visit(BlockStmt& s) {
    execute_block(s.statements, std::make_shared<Environment>(environment_));
}

void Interpreter::visit(IfStmt& s) {
    if (evaluate(*s.cond).truthy()) execute(*s.then_branch);
    else if (s.else_branch)         execute(*s.else_branch);
}

void Interpreter::visit(WhileStmt& s) {
    while (evaluate(*s.cond).truthy()) execute(*s.body);
}

void Interpreter::visit(FunctionStmt& s) {
    auto fn = std::make_shared<interp_detail::UserFunction>(
        std::string(s.name.lexeme), &s.params, &s.body, environment_);
    environment_->define(std::string(s.name.lexeme),
                         Value{std::static_pointer_cast<Callable>(fn)});
}

void Interpreter::visit(ReturnStmt& s) {
    Value v = s.value ? evaluate(*s.value) : Value{};
    throw ReturnSignal{std::move(v)};
}

void Interpreter::visit(ImportStmt& s) {
    // Decode the path string literal (strip quotes, process escapes).
    std::string path = interp_detail::decode_string_literal(s.path_token);

    // Where to resolve relative paths from: the importing file's directory,
    // or the process CWD if there is no current file.
    std::filesystem::path requesting_dir = current_file_.has_parent_path()
        ? current_file_.parent_path()
        : std::filesystem::current_path();

    auto m = modules_->load(path, requesting_dir, s.keyword);
    environment_->define(std::string(s.alias.lexeme), Value{m});
}

}  // namespace bnl
