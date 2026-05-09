#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace bnl {

class Interpreter;
class Value;
class Module;
class Instance;

using ModulePtr   = std::shared_ptr<Module>;
using ListPtr     = std::shared_ptr<std::vector<Value>>;
using MapPtr      = std::shared_ptr<std::unordered_map<std::string, Value>>;
using InstancePtr = std::shared_ptr<Instance>;

class Callable {
public:
    virtual ~Callable() = default;
    virtual Value       call(Interpreter& interp, std::vector<Value> args) = 0;
    virtual int         arity() const = 0;        // -1 means variadic
    virtual std::string name()  const = 0;
};

using CallablePtr = std::shared_ptr<Callable>;

class Value {
public:
    using Storage = std::variant<
        std::monostate,   // Null
        bool,
        double,
        std::string,
        CallablePtr,
        ModulePtr,
        ListPtr,
        MapPtr,
        InstancePtr
    >;

    Value() = default;                        // null
    Value(std::nullptr_t)        : storage_(std::monostate{}) {}
    Value(bool b)                : storage_(b) {}
    Value(double n)              : storage_(n) {}
    Value(int n)                 : storage_(static_cast<double>(n)) {}
    Value(std::string s)         : storage_(std::move(s)) {}
    Value(const char* s)         : storage_(std::string(s)) {}
    Value(CallablePtr c)         : storage_(std::move(c)) {}
    Value(ModulePtr m)           : storage_(std::move(m)) {}
    Value(ListPtr l)             : storage_(std::move(l)) {}
    Value(MapPtr m)              : storage_(std::move(m)) {}
    Value(InstancePtr i)         : storage_(std::move(i)) {}

    bool is_null()     const { return std::holds_alternative<std::monostate>(storage_); }
    bool is_bool()     const { return std::holds_alternative<bool>(storage_); }
    bool is_number()   const { return std::holds_alternative<double>(storage_); }
    bool is_string()   const { return std::holds_alternative<std::string>(storage_); }
    bool is_callable() const { return std::holds_alternative<CallablePtr>(storage_); }
    bool is_module()   const { return std::holds_alternative<ModulePtr>(storage_); }
    bool is_list()     const { return std::holds_alternative<ListPtr>(storage_); }
    bool is_map()      const { return std::holds_alternative<MapPtr>(storage_); }
    bool is_instance() const { return std::holds_alternative<InstancePtr>(storage_); }

    bool                       as_bool()     const { return std::get<bool>(storage_); }
    double                     as_number()   const { return std::get<double>(storage_); }
    const std::string&         as_string()   const { return std::get<std::string>(storage_); }
    const CallablePtr&         as_callable() const { return std::get<CallablePtr>(storage_); }
    const ModulePtr&           as_module()   const { return std::get<ModulePtr>(storage_); }
    const ListPtr&             as_list()     const { return std::get<ListPtr>(storage_); }
    const MapPtr&              as_map()      const { return std::get<MapPtr>(storage_); }
    const InstancePtr&         as_instance() const { return std::get<InstancePtr>(storage_); }

    bool        truthy() const;
    bool        equals(const Value& other) const;
    const char* type_name() const;
    std::string to_display() const;  // user-facing (print)
    std::string to_repr() const;     // debug (string with quotes, etc.)

private:
    Storage storage_;
};

// Wrap a std::function-like into a Callable.
class NativeFunction : public Callable {
public:
    using Fn = std::function<Value(Interpreter&, std::vector<Value>)>;

    NativeFunction(std::string name, int arity, Fn fn)
        : name_(std::move(name)), arity_(arity), fn_(std::move(fn)) {}

    Value       call(Interpreter& i, std::vector<Value> args) override { return fn_(i, std::move(args)); }
    int         arity() const override { return arity_; }
    std::string name()  const override { return name_; }

private:
    std::string name_;
    int         arity_;
    Fn          fn_;
};

}  // namespace bnl
