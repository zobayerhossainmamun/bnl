// mathx — sample bnl native plugin.
//
// Built as a shared library; loaded by bnl at runtime when a script does
//     import "mathx" as m;
// and the dep manifest at deps/mathx/bnl.json declares
//     { "name": "mathx", "native": "<path-to-this-built-shared-library>" }
//
// The plugin must be built with the same compiler/STL as bnl_core.

#include <bnl/plugin.h>

#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" BNL_PLUGIN_EXPORT bnl::ModulePtr bnl_load(bnl::Interpreter&) {
    return bnl::NativeModule("mathx")
        .add_function("cube", 1,
            [](bnl::Interpreter&, std::vector<bnl::Value> args) -> bnl::Value {
                if (!args[0].is_number())
                    throw std::runtime_error("mathx.cube expects a number");
                double n = args[0].as_number();
                return bnl::Value{n * n * n};
            })
        .add_function("hypot", 2,
            [](bnl::Interpreter&, std::vector<bnl::Value> args) -> bnl::Value {
                if (!args[0].is_number() || !args[1].is_number())
                    throw std::runtime_error("mathx.hypot expects two numbers");
                return bnl::Value{std::hypot(args[0].as_number(), args[1].as_number())};
            })
        .add_value("greeting", bnl::Value{std::string{"Hi from a native plugin!"}})
        .build();
}
