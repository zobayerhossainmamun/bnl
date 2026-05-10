#include "stdlib/registry.h"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

namespace {
#ifdef _WIN32
// Convert a UTF-16 buffer of length n (no trailing null required) to UTF-8.
std::string wide_to_utf8(const wchar_t* w, std::size_t n) {
    if (n == 0) return "";
    int need = WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(n),
                                   nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(n),
                        out.data(), need, nullptr, nullptr);
    return out;
}
#endif
}  // namespace

#include "bnl/version.h"
#include "bnl/interpreter.h"
#include "bnl/native_module.h"

#ifndef _WIN32
extern char **environ;
#endif

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

        // sys.cwd() — current working directory as a UTF-8 string.
        .add_function("cwd", 0,
            [](Interpreter&, std::vector<Value>) -> Value {
                std::error_code ec;
                auto p = std::filesystem::current_path(ec);
                if (ec) throw std::runtime_error("sys.cwd: " + ec.message());
                // u8string() is UTF-8 on every platform (vs .string() which
                // uses the active codepage on Windows). Reinterpret to a
                // char-based std::string for the bnl Value.
                auto u8 = p.u8string();
                return Value{std::string(reinterpret_cast<const char*>(u8.data()), u8.size())};
            })

        // sys.envs() — every environment variable as a map of name -> value.
        .add_function("envs", 0,
            [](Interpreter&, std::vector<Value>) -> Value {
                auto out = std::make_shared<std::unordered_map<std::string, Value>>();
#ifdef _WIN32
                LPWCH block = GetEnvironmentStringsW();
                if (!block) throw std::runtime_error("sys.envs: GetEnvironmentStringsW failed");
                for (LPWCH p = block; *p; ) {
                    std::size_t len = wcslen(p);
                    // Skip Windows' synthetic `=DRIVE:=...` cwd-per-drive entries.
                    if (p[0] != L'=') {
                        const wchar_t* eq = wcschr(p, L'=');
                        if (eq && eq != p) {
                            std::string name  = wide_to_utf8(p, static_cast<std::size_t>(eq - p));
                            std::string value = wide_to_utf8(eq + 1, len - static_cast<std::size_t>(eq + 1 - p));
                            (*out)[std::move(name)] = Value{std::move(value)};
                        }
                    }
                    p += len + 1;
                }
                FreeEnvironmentStringsW(block);
#else
                for (char** e = environ; e && *e; ++e) {
                    std::string entry(*e);
                    auto eq = entry.find('=');
                    if (eq != std::string::npos && eq > 0) {
                        (*out)[entry.substr(0, eq)] = Value{entry.substr(eq + 1)};
                    }
                }
#endif
                return Value{out};
            })

        .build();

    interp.register_native_module("sys", m);
}

}  // namespace bnl
