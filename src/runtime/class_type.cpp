#include "runtime/class_type.h"

#include <utility>

namespace bnl {

int Class::arity() const {
    // Walk the parent chain — a subclass without its own init inherits the
    // parent's signature.
    auto init = find_method("init");
    if (!init) return 0;                           // no init anywhere -> Foo() takes no args
    int init_arity = init->arity();
    if (init_arity < 0) return init_arity;         // variadic
    return init_arity > 0 ? init_arity - 1 : 0;    // hide self
}

int Class::min_arity() const {
    auto init = find_method("init");
    if (!init) return 0;
    int n = init->min_arity();
    if (n < 0) return n;
    return n > 0 ? n - 1 : 0;
}

Value Class::call(Interpreter& interp, std::vector<Value> args) {
    auto instance = std::make_shared<Instance>(shared_from_this());
    if (auto init = find_method("init"); init) {
        std::vector<Value> with_self;
        with_self.reserve(args.size() + 1);
        with_self.emplace_back(instance);
        for (auto& a : args) with_self.push_back(std::move(a));
        init->call(interp, std::move(with_self));
    }
    return Value{instance};
}

}  // namespace bnl
