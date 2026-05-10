#include "bnl/interpreter.h"
#include "interpreter/internal.h"

#include <fmt/core.h>

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include "class_type.h"
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

void Interpreter::visit(ClassStmt& s) {
    // Resolve the optional superclass at definition time. Must be a Class
    // value already in scope.
    std::shared_ptr<Class> parent;
    std::shared_ptr<Environment> method_closure = environment_;
    if (s.superclass.lexeme.size() > 0) {
        const Value* sv = environment_->lookup(std::string(s.superclass.lexeme));
        if (!sv || !sv->is_callable())
            throw RuntimeError(s.superclass,
                fmt::format("superclass '{}' is not defined", s.superclass.lexeme));
        parent = std::dynamic_pointer_cast<Class>(sv->as_callable());
        if (!parent)
            throw RuntimeError(s.superclass,
                fmt::format("superclass '{}' is not a class", s.superclass.lexeme));

        // Each method's closure gets a `super` binding pointing at the parent
        // class. SuperExpr looks it up here at evaluation time.
        method_closure = std::make_shared<Environment>(environment_);
        method_closure->define("super",
            Value{std::static_pointer_cast<Callable>(parent)});
    }

    std::unordered_map<std::string, CallablePtr> methods;
    for (auto& m : s.methods) {
        auto fn = std::make_shared<interp_detail::UserFunction>(
            std::string(m->name.lexeme), &m->params, &m->body, method_closure);
        methods.emplace(std::string(m->name.lexeme),
                        std::static_pointer_cast<Callable>(fn));
    }
    auto klass = std::make_shared<Class>(std::string(s.name.lexeme),
                                         std::move(methods), parent);
    environment_->define(std::string(s.name.lexeme),
                         Value{std::static_pointer_cast<Callable>(klass)});
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
