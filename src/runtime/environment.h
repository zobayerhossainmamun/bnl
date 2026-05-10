#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "bnl/value.h"

namespace bnl {

class Environment : public std::enable_shared_from_this<Environment> {
public:
    Environment() = default;
    explicit Environment(std::shared_ptr<Environment> parent) : parent_(std::move(parent)) {}

    void define(const std::string& name, Value value);

    // Returns true if found and assigned. False if name does not exist
    // anywhere in the chain (caller turns this into a runtime error).
    bool assign(const std::string& name, Value value);

    // Returns nullptr if not found in the chain.
    const Value* lookup(const std::string& name) const;

private:
    std::unordered_map<std::string, Value> values_;
    std::shared_ptr<Environment>           parent_;
};

}  // namespace bnl
