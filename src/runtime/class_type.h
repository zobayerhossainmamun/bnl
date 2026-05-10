#pragma once

// Internal — defines the runtime types behind `class Foo { ... }`:
//
//   Class        — a Callable. Calling it (`Foo(args)`) creates an Instance,
//                  invokes `init` if the class declared one, returns the
//                  Instance.
//   Instance     — opaque object with a class pointer and a field map.
//                  Field reads/writes go through MemberExpr / SetMemberExpr.
//   BoundMethod  — wrapper Callable returned when reading a method off an
//                  Instance. Captures the receiver and prepends it as `self`
//                  when called, so `c.method(x)` desugars to `Foo.method(c, x)`.
//
// Methods are user-defined functions stored in the class's method dictionary.
// They are regular Callables (typically UserFunction); the first parameter is
// conventionally `self`.

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "bnl/value.h"

namespace bnl {

class Class : public Callable, public std::enable_shared_from_this<Class> {
public:
    Class(std::string                                     name,
          std::unordered_map<std::string, CallablePtr>    methods,
          std::shared_ptr<Class>                          parent = nullptr)
        : name_(std::move(name)), methods_(std::move(methods)),
          parent_(std::move(parent)) {}

    // Calling a class instantiates it. Arity matches the user-visible side of
    // `init` — i.e. init's declared arity minus 1 for the implicit `self`.
    // For inherited classes with no own init, falls back to the parent's init.
    int         arity() const override;
    std::string name()  const override { return name_; }
    Value       call(Interpreter& interp, std::vector<Value> args) override;

    // Method-resolution order: own methods first, then walk up the parent
    // chain. Single inheritance, no MRO complications.
    CallablePtr find_method(const std::string& method_name) const {
        auto it = methods_.find(method_name);
        if (it != methods_.end()) return it->second;
        if (parent_) return parent_->find_method(method_name);
        return nullptr;
    }

    const std::shared_ptr<Class>& parent() const { return parent_; }

private:
    std::string                                  name_;
    std::unordered_map<std::string, CallablePtr> methods_;
    std::shared_ptr<Class>                       parent_;
};

class Instance {
public:
    explicit Instance(std::shared_ptr<Class> klass) : klass_(std::move(klass)) {}

    const std::shared_ptr<Class>&                  klass()  const { return klass_; }
    std::unordered_map<std::string, Value>&        fields()       { return fields_; }
    const std::unordered_map<std::string, Value>&  fields() const { return fields_; }

private:
    std::shared_ptr<Class>                  klass_;
    std::unordered_map<std::string, Value>  fields_;
};

// Returned when reading a method off an Instance. Wraps the underlying
// CallablePtr and prepends the receiver as the first argument when called.
class BoundMethod : public Callable {
public:
    BoundMethod(InstancePtr self, CallablePtr fn)
        : self_(std::move(self)), fn_(std::move(fn)) {}

    int arity() const override {
        // Hide `self` from the caller. Variadic functions stay variadic.
        int a = fn_->arity();
        return a < 0 ? a : a - 1;
    }
    std::string name() const override { return fn_->name(); }

    Value call(Interpreter& interp, std::vector<Value> args) override {
        std::vector<Value> with_self;
        with_self.reserve(args.size() + 1);
        with_self.emplace_back(self_);
        for (auto& a : args) with_self.push_back(std::move(a));
        return fn_->call(interp, std::move(with_self));
    }

private:
    InstancePtr self_;
    CallablePtr fn_;
};

}  // namespace bnl
