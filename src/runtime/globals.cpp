#include "bnl/interpreter.h"
#include "runtime/internal.h"

#include "runtime/environment.h"

#include <fmt/core.h>

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bnl {

void Interpreter::register_builtins() {
    using interp_detail::is_ascii_space;

    // print(...): variadic; values displayed space-separated, then newline.
    auto print_fn = std::make_shared<NativeFunction>(
        "print", -1,
        [](Interpreter&, std::vector<Value> args) -> Value {
            for (std::size_t i = 0; i < args.size(); ++i) {
                if (i) fmt::print(" ");
                fmt::print("{}", args[i].to_display());
            }
            fmt::print("\n");
            return Value{};
        });

    // Both names point at the same Callable instance.
    globals_->define("print",  Value{print_fn});
    globals_->define("\xe0\xa6\xa6\xe0\xa7\x87\xe0\xa6\x96\xe0\xa6\xbe\xe0\xa6\x93", Value{print_fn});  // দেখাও

    // str(x): convert any value to its display string.
    auto str_fn = std::make_shared<NativeFunction>(
        "str", 1,
        [](Interpreter&, std::vector<Value> args) -> Value {
            return Value{args[0].to_display()};
        });
    globals_->define("str", Value{str_fn});
    globals_->define("\xe0\xa6\xb2\xe0\xa7\x87\xe0\xa6\x96", Value{str_fn});  // লেখ

    // type(x): return the type name as a string.
    auto type_fn = std::make_shared<NativeFunction>(
        "type", 1,
        [](Interpreter&, std::vector<Value> args) -> Value {
            return Value{std::string(args[0].type_name())};
        });
    globals_->define("type", Value{type_fn});
    globals_->define("\xe0\xa6\xa7\xe0\xa6\xb0\xe0\xa6\xa3", Value{type_fn});  // ধরণ

    // to_number(s): parse a string to a number, returning null on failure.
    auto to_number_fn = std::make_shared<NativeFunction>(
        "to_number", 1,
        [](Interpreter&, std::vector<Value> args) -> Value {
            if (args[0].is_number()) return args[0];
            if (!args[0].is_string()) return Value{};
            const std::string& s = args[0].as_string();
            try {
                std::size_t consumed = 0;
                double n = std::stod(s, &consumed);
                // Reject if there's trailing non-whitespace.
                while (consumed < s.size() && is_ascii_space(s[consumed])) ++consumed;
                if (consumed != s.size()) return Value{};
                return Value{n};
            } catch (...) {
                return Value{};
            }
        });
    globals_->define("to_number", Value{to_number_fn});

    // chr(n): single-byte string from byte value 0..255.
    auto chr_fn = std::make_shared<NativeFunction>(
        "chr", 1,
        [](Interpreter&, std::vector<Value> args) -> Value {
            if (!args[0].is_number())
                throw std::runtime_error("chr(n): n must be a number");
            int n = static_cast<int>(args[0].as_number());
            if (n < 0 || n > 255)
                throw std::runtime_error("chr(n): n must be in 0..255");
            return Value{std::string(1, static_cast<char>(n))};
        });
    globals_->define("chr", Value{chr_fn});

    // try_call(thunk, on_err): call thunk() with no args; if it throws, call
    // on_err(message_string). Returns thunk()'s return value, or on_err's
    // return value when the thunk threw. bnl has no try/catch syntax — this
    // is the escape hatch for code (like web.serve) that must turn handler
    // errors into responses instead of letting them crash the loop.
    auto try_call_fn = std::make_shared<NativeFunction>(
        "try_call", 2,
        [](Interpreter& interp, std::vector<Value> args) -> Value {
            if (!args[0].is_callable())
                throw std::runtime_error("try_call: first arg must be a function");
            if (!args[1].is_callable())
                throw std::runtime_error("try_call: second arg must be a function");
            try {
                return args[0].as_callable()->call(interp, {});
            } catch (const RuntimeError& e) {
                return args[1].as_callable()->call(interp, { Value{std::string(e.what())} });
            } catch (const std::exception& e) {
                return args[1].as_callable()->call(interp, { Value{std::string(e.what())} });
            }
        });
    globals_->define("try_call", Value{try_call_fn});
}

}  // namespace bnl
