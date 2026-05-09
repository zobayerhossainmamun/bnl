#include "native/builtins.h"

#include <cstdlib>
#include <stdexcept>
#include <string>

#include "bnl/version.h"
#include "bnl/interpreter.h"
#include "bnl/native_module.h"

namespace bnl {

void register_sys(Interpreter& interp) {
    auto m = NativeModule("sys")
        .add_value("platform", Value{std::string(kPlatform)})

        // sys.argc() — number of args passed after the script path.
        .add_function("argc", 0,
            [&interp](Interpreter&, std::vector<Value>) -> Value {
                return Value{static_cast<double>(interp.program_args().size())};
            })

        // sys.arg(i) — i-th arg as a string, or null if out of range.
        .add_function("arg", 1,
            [&interp](Interpreter&, std::vector<Value> args) -> Value {
                if (!args[0].is_number())
                    throw std::runtime_error("sys.arg(i): index must be a number");
                const auto& a = interp.program_args();
                std::size_t i = static_cast<std::size_t>(args[0].as_number());
                if (i >= a.size()) return Value{};
                return Value{a[i]};
            })

        // sys.env(name) — env-var value as a string, or null if unset.
        .add_function("env", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                if (!args[0].is_string())
                    throw std::runtime_error("sys.env(name): name must be a string");
                const char* v = std::getenv(args[0].as_string().c_str());
                if (!v) return Value{};
                return Value{std::string(v)};
            })

        // sys.exit(code) — terminates the process. Never returns.
        .add_function("exit", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                int code = args[0].is_number() ? static_cast<int>(args[0].as_number()) : 0;
                std::exit(code);
            })

        .build();

    interp.register_native_module("sys", m);
}

}  // namespace bnl
