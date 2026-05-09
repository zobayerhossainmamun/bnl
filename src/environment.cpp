#include "environment.h"

#include <utility>

namespace bnl {

void Environment::define(const std::string& name, Value value) {
    values_[name] = std::move(value);
}

bool Environment::assign(const std::string& name, Value value) {
    auto it = values_.find(name);
    if (it != values_.end()) {
        it->second = std::move(value);
        return true;
    }
    if (parent_) return parent_->assign(name, std::move(value));
    return false;
}

const Value* Environment::lookup(const std::string& name) const {
    auto it = values_.find(name);
    if (it != values_.end()) return &it->second;
    if (parent_) return parent_->lookup(name);
    return nullptr;
}

}  // namespace bnl
