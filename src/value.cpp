#include "bnl/value.h"

#include <fmt/core.h>

#include <cmath>
#include <cstdint>
#include <string>

#include "bnl/module.h"
#include "class_type.h"

namespace bnl {

bool Value::truthy() const {
    if (is_null()) return false;
    if (is_bool()) return as_bool();
    return true;
}

bool Value::equals(const Value& other) const {
    if (storage_.index() != other.storage_.index()) return false;
    if (is_null())     return true;
    if (is_bool())     return as_bool()   == other.as_bool();
    if (is_number())   return as_number() == other.as_number();
    if (is_string())   return as_string() == other.as_string();
    if (is_callable()) return as_callable().get() == other.as_callable().get();
    if (is_module())   return as_module().get()   == other.as_module().get();
    if (is_list())     return as_list().get()     == other.as_list().get();    // identity
    if (is_map())      return as_map().get()      == other.as_map().get();     // identity
    if (is_instance()) return as_instance().get() == other.as_instance().get();// identity
    return false;
}

const char* Value::type_name() const {
    if (is_null())     return "null";
    if (is_bool())     return "bool";
    if (is_number())   return "number";
    if (is_string())   return "string";
    if (is_callable()) {
        // Class instances are stored as Callables (so `Foo()` calls them);
        // surface them as "class" so `type(Foo)` reads naturally.
        if (dynamic_cast<Class*>(as_callable().get())) return "class";
        return "function";
    }
    if (is_module())   return "module";
    if (is_list())     return "list";
    if (is_map())      return "map";
    if (is_instance()) return "instance";
    return "?";
}

namespace {

std::string number_to_string(double n) {
    if (std::isnan(n)) return "NaN";
    if (std::isinf(n)) return n < 0 ? "-Infinity" : "Infinity";
    // Print integer-valued doubles without a decimal point.
    if (n == std::floor(n) && std::abs(n) < 1e16) {
        return fmt::format("{:.0f}", n);
    }
    return fmt::format("{}", n);
}

}  // namespace

std::string Value::to_display() const {
    if (is_null())     return "null";
    if (is_bool())     return as_bool() ? "true" : "false";
    if (is_number())   return number_to_string(as_number());
    if (is_string())   return as_string();
    if (is_callable()) {
        if (dynamic_cast<Class*>(as_callable().get()))
            return "<class " + as_callable()->name() + ">";
        return "<function " + as_callable()->name() + ">";
    }
    if (is_module())   return "<module " + as_module()->path() + ">";
    if (is_list()) {
        std::string out = "[";
        bool first = true;
        for (const auto& v : *as_list()) {
            if (!first) out += ", ";
            out += v.to_repr();
            first = false;
        }
        out += ']';
        return out;
    }
    if (is_map()) {
        std::string out = "{";
        bool first = true;
        for (const auto& [k, v] : *as_map()) {
            if (!first) out += ", ";
            out += k;
            out += ": ";
            out += v.to_repr();
            first = false;
        }
        out += '}';
        return out;
    }
    if (is_instance()) {
        return "<instance of " + as_instance()->klass()->name() + ">";
    }
    return "?";
}

std::string Value::to_repr() const {
    if (is_string()) {
        std::string out = "\"";
        for (char c : as_string()) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\t': out += "\\t";  break;
                case '\r': out += "\\r";  break;
                default:   out += c;
            }
        }
        out += '"';
        return out;
    }
    return to_display();
}

}  // namespace bnl
