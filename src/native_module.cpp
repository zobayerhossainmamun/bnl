#include "native_module.h"

#include <utility>

#include "environment.h"

namespace bnl {

NativeModule::NativeModule(std::string name)
    : name_(std::move(name)),
      env_(std::make_shared<Environment>()) {}

NativeModule& NativeModule::add_function(std::string fn_name, int arity, NativeFunction::Fn fn) {
    auto callable = std::make_shared<NativeFunction>(fn_name, arity, std::move(fn));
    env_->define(fn_name, Value{std::static_pointer_cast<Callable>(callable)});
    return *this;
}

NativeModule& NativeModule::add_value(std::string name, Value v) {
    env_->define(name, std::move(v));
    return *this;
}

ModulePtr NativeModule::build() {
    auto m = std::make_shared<Module>();
    m->set_path("<native:" + name_ + ">");
    m->set_exports(env_);
    return m;
}

}  // namespace bnl
