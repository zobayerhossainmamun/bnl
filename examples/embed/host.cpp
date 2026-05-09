// Smallest possible C++ host that embeds bnl.
//
// This program demonstrates the entire embedding API surface:
//   - Construct an Interpreter
//   - Register a custom NativeModule (so user bnl code can call back into C++)
//   - Run a bnl source string with run_source()
//
// Build: linked against bnl_core (see CMakeLists.txt next to this file).

#include <bnl/interpreter.h>
#include <bnl/native_module.h>

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

int main() {
    bnl::Interpreter interp;

    // A custom native module that wraps a C++ function so bnl scripts can call
    // it. The lambda runs on the bnl call site's thread; throw std::runtime_error
    // on bad input and the interpreter wraps it with line/col context.
    auto host_module = bnl::NativeModule("host")
        .add_function("greet", 1,
            [](bnl::Interpreter&, std::vector<bnl::Value> args) -> bnl::Value {
                if (!args[0].is_string())
                    throw std::runtime_error("host.greet expects a string");
                std::cout << "[C++ host] Hello, " << args[0].as_string() << "!\n";
                return bnl::Value{};
            })
        .add_function("square", 1,
            [](bnl::Interpreter&, std::vector<bnl::Value> args) -> bnl::Value {
                if (!args[0].is_number())
                    throw std::runtime_error("host.square expects a number");
                double n = args[0].as_number();
                return bnl::Value{n * n};
            })
        .build();

    interp.register_native_module("host", host_module);

    // Run a tiny bnl program inline. Note: the "host" module imported below
    // is the C++ NativeModule we registered above — same import machinery as
    // sys, io, json, etc.
    const std::string source = R"(
        import "host" as host;

        host.greet("from bnl");
        print("3^2 =", host.square(3));
        print("bnl is", "embedded");
    )";

    return interp.run_source(source) ? 0 : 1;
}
