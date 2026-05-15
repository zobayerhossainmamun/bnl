#include "stdlib/registry.h"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <unistd.h>
#endif

#ifdef __APPLE__
#  include <mach-o/dyld.h>
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

// fs::path → UTF-8 std::string, independent of the active codepage on Windows.
std::string path_to_utf8(const std::filesystem::path& p) {
    auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

// Best-effort architecture name, picked at compile time.
constexpr const char* kArch =
#if defined(_M_X64) || defined(__x86_64__)
    "x86_64"
#elif defined(_M_ARM64) || defined(__aarch64__)
    "arm64"
#elif defined(_M_IX86) || defined(__i386__)
    "x86"
#elif defined(_M_ARM) || defined(__arm__)
    "arm"
#else
    "unknown"
#endif
    ;
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
        .add_value("version",  Value{std::string(kVersion)})
        .add_value("arch",     Value{std::string(kArch)})

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

        // sys.argv() — all CLI args as a list of strings.
        .add_function("argv", 0,
            [&interp](Interpreter&, std::vector<Value>) -> Value {
                const auto& a = interp.program_args();
                auto out = std::make_shared<std::vector<Value>>();
                out->reserve(a.size());
                for (const auto& s : a) out->emplace_back(s);
                return Value{out};
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

        // sys.setenv(name, value) — set / overwrite an env var.
        .add_function("setenv", 2,
            [](Interpreter&, std::vector<Value> args) -> Value {
                if (!args[0].is_string() || !args[1].is_string())
                    throw std::runtime_error("sys.setenv(name, value): both args must be strings");
                const auto& name  = args[0].as_string();
                const auto& value = args[1].as_string();
                if (name.empty() || name.find('=') != std::string::npos)
                    throw std::runtime_error("sys.setenv: invalid name");
#ifdef _WIN32
                if (_putenv_s(name.c_str(), value.c_str()) != 0)
                    throw std::runtime_error("sys.setenv: failed");
#else
                if (::setenv(name.c_str(), value.c_str(), 1) != 0)
                    throw std::runtime_error("sys.setenv: failed");
#endif
                return Value{};
            })

        // sys.unsetenv(name) — remove an env var. No-op if it doesn't exist.
        .add_function("unsetenv", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                if (!args[0].is_string())
                    throw std::runtime_error("sys.unsetenv(name): name must be a string");
                const auto& name = args[0].as_string();
                if (name.empty() || name.find('=') != std::string::npos)
                    throw std::runtime_error("sys.unsetenv: invalid name");
#ifdef _WIN32
                // Per MSDN: empty value removes the variable from the env block.
                _putenv_s(name.c_str(), "");
#else
                ::unsetenv(name.c_str());
#endif
                return Value{};
            })

        // sys.exit(code) — terminates the process. Never returns.
        .add_function("exit", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                int code = args[0].is_number() ? static_cast<int>(args[0].as_number()) : 0;
                std::exit(code);
            })

        // sys.abort() — abnormal termination (no atexit handlers, dumps core
        // on Unix). For "something is unrecoverable" cases distinct from exit.
        .add_function("abort", 0,
            [](Interpreter&, std::vector<Value>) -> Value {
                std::abort();
            })

        // sys.cwd() — current working directory as a UTF-8 string.
        .add_function("cwd", 0,
            [](Interpreter&, std::vector<Value>) -> Value {
                std::error_code ec;
                auto p = std::filesystem::current_path(ec);
                if (ec) throw std::runtime_error("sys.cwd: " + ec.message());
                return Value{path_to_utf8(p)};
            })

        // sys.chdir(path) — change working directory.
        .add_function("chdir", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                if (!args[0].is_string())
                    throw std::runtime_error("sys.chdir(path): path must be a string");
                const auto& s = args[0].as_string();
                std::filesystem::path p(
                    reinterpret_cast<const char8_t*>(s.data()),
                    reinterpret_cast<const char8_t*>(s.data() + s.size()));
                std::error_code ec;
                std::filesystem::current_path(p, ec);
                if (ec) throw std::runtime_error("sys.chdir: " + ec.message());
                return Value{};
            })

        // sys.tempdir() — OS temp directory as a UTF-8 string.
        .add_function("tempdir", 0,
            [](Interpreter&, std::vector<Value>) -> Value {
                std::error_code ec;
                auto p = std::filesystem::temp_directory_path(ec);
                if (ec) throw std::runtime_error("sys.tempdir: " + ec.message());
                return Value{path_to_utf8(p)};
            })

        // sys.home() — user home directory, or null if not derivable from env.
        .add_function("home", 0,
            [](Interpreter&, std::vector<Value>) -> Value {
#ifdef _WIN32
                if (const char* v = std::getenv("USERPROFILE")) return Value{std::string(v)};
#else
                if (const char* v = std::getenv("HOME")) return Value{std::string(v)};
#endif
                return Value{};
            })

        // sys.executable() — full path to the running bnl interpreter binary.
        .add_function("executable", 0,
            [](Interpreter&, std::vector<Value>) -> Value {
#ifdef _WIN32
                std::wstring buf(MAX_PATH, L'\0');
                for (;;) {
                    DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
                    if (n == 0) throw std::runtime_error("sys.executable: GetModuleFileNameW failed");
                    if (n < buf.size()) { buf.resize(n); break; }
                    buf.resize(buf.size() * 2);  // truncation -> grow and retry.
                }
                return Value{wide_to_utf8(buf.data(), buf.size())};
#elif defined(__APPLE__)
                uint32_t n = 0;
                _NSGetExecutablePath(nullptr, &n);
                std::string buf(n, '\0');
                if (_NSGetExecutablePath(buf.data(), &n) != 0)
                    throw std::runtime_error("sys.executable: _NSGetExecutablePath failed");
                if (!buf.empty() && buf.back() == '\0') buf.pop_back();
                return Value{std::move(buf)};
#else
                std::error_code ec;
                auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
                if (ec) throw std::runtime_error("sys.executable: " + ec.message());
                return Value{path_to_utf8(p)};
#endif
            })

        // sys.script() — path of the running .bnl entry script, or null for
        // REPL / -e inline code.
        .add_function("script", 0,
            [&interp](Interpreter&, std::vector<Value>) -> Value {
                const auto& p = interp.entry_path();
                if (p.empty()) return Value{};
                return Value{path_to_utf8(p)};
            })

        // sys.pid() — current process id.
        .add_function("pid", 0,
            [](Interpreter&, std::vector<Value>) -> Value {
#ifdef _WIN32
                return Value{static_cast<double>(GetCurrentProcessId())};
#else
                return Value{static_cast<double>(::getpid())};
#endif
            })

        // sys.cpu_count() — number of logical CPUs, or 0 if unknown.
        .add_function("cpu_count", 0,
            [](Interpreter&, std::vector<Value>) -> Value {
                return Value{static_cast<double>(std::thread::hardware_concurrency())};
            })

        // sys.hostname() — machine hostname.
        .add_function("hostname", 0,
            [](Interpreter&, std::vector<Value>) -> Value {
#ifdef _WIN32
                wchar_t buf[256];
                DWORD   n = sizeof(buf) / sizeof(buf[0]);
                if (!GetComputerNameExW(ComputerNameDnsHostname, buf, &n))
                    throw std::runtime_error("sys.hostname: GetComputerNameExW failed");
                return Value{wide_to_utf8(buf, n)};
#else
                char buf[256];
                if (::gethostname(buf, sizeof(buf)) != 0)
                    throw std::runtime_error("sys.hostname: gethostname failed");
                buf[sizeof(buf) - 1] = '\0';
                return Value{std::string(buf)};
#endif
            })

        // sys.username() — current user name, or null if unknown.
        .add_function("username", 0,
            [](Interpreter&, std::vector<Value>) -> Value {
#ifdef _WIN32
                if (const char* v = std::getenv("USERNAME")) return Value{std::string(v)};
#else
                if (const char* v = std::getenv("USER"))    return Value{std::string(v)};
                if (const char* v = std::getenv("LOGNAME")) return Value{std::string(v)};
#endif
                return Value{};
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
