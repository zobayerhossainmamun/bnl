#include "stdlib/registry.h"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "bnl/interpreter.h"
#include "bnl/native_module.h"

namespace bnl {

namespace {

// ===========================================================================
// Thin libm wrapper. By design the native side is the absolute lowest
// level — every function is a one-line cmath call. Composed helpers
// (min/max/clamp/sign/lerp) live in lib/math.bnl.
// ===========================================================================

double require_number(const Value& v, const char* where) {
    if (!v.is_number())
        throw std::runtime_error(std::string(where) + ": expected number");
    return v.as_number();
}

// Helper that turns a unary `double -> double` cmath function into a native
// callable with the standard arity + arg-type checks.
auto unary(const char* name, double (*fn)(double)) {
    std::string n = name;
    return [n, fn](Interpreter&, std::vector<Value> args) -> Value {
        return Value{fn(require_number(args[0], n.c_str()))};
    };
}

}  // namespace

void register_math(Interpreter& interp) {
    auto m = NativeModule("_math")

        // ---- powers / roots / logs ----
        .add_function("sqrt",  1, unary("math.sqrt",  std::sqrt))
        .add_function("exp",   1, unary("math.exp",   std::exp))
        .add_function("log",   1, unary("math.log",   std::log))     // natural
        .add_function("log2",  1, unary("math.log2",  std::log2))
        .add_function("log10", 1, unary("math.log10", std::log10))

        // ---- trig ----
        .add_function("sin",   1, unary("math.sin",   std::sin))
        .add_function("cos",   1, unary("math.cos",   std::cos))
        .add_function("tan",   1, unary("math.tan",   std::tan))
        .add_function("asin",  1, unary("math.asin",  std::asin))
        .add_function("acos",  1, unary("math.acos",  std::acos))
        .add_function("atan",  1, unary("math.atan",  std::atan))

        // ---- rounding / sign ----
        .add_function("floor", 1, unary("math.floor", std::floor))
        .add_function("ceil",  1, unary("math.ceil",  std::ceil))
        .add_function("round", 1, unary("math.round", std::round))
        .add_function("trunc", 1, unary("math.trunc", std::trunc))
        .add_function("abs",   1, unary("math.abs",   std::fabs))

        // ---- two-arg ----
        .add_function("pow", 2,
            [](Interpreter&, std::vector<Value> args) -> Value {
                return Value{std::pow(require_number(args[0], "math.pow"),
                                      require_number(args[1], "math.pow"))};
            })
        .add_function("atan2", 2,
            [](Interpreter&, std::vector<Value> args) -> Value {
                return Value{std::atan2(require_number(args[0], "math.atan2"),
                                        require_number(args[1], "math.atan2"))};
            })

        // ---- classification ----
        .add_function("is_nan", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                return Value{std::isnan(require_number(args[0], "math.is_nan"))};
            })
        .add_function("is_finite", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                return Value{std::isfinite(require_number(args[0], "math.is_finite"))};
            })

        // ---- constants ----
        .add_value("pi",  Value{3.141592653589793})
        .add_value("e",   Value{2.718281828459045})
        .add_value("inf", Value{std::numeric_limits<double>::infinity()})
        .add_value("nan", Value{std::numeric_limits<double>::quiet_NaN()})

        .build();

    interp.register_native_module("_math", m);
}

}  // namespace bnl
