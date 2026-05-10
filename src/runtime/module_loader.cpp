#include "runtime/module_loader.h"

#include <fmt/core.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <utility>

#include "runtime/environment.h"
#include "bnl/interpreter.h"
#include "frontend/lexer.h"
#include "frontend/parser.h"
#include "stdlib_embedded.h"

#ifdef BNL_FFI_ENABLED
#  include "ffi/dynamic_library.h"
#endif

namespace bnl {

namespace fs = std::filesystem;

namespace {

std::string read_file_to_string(const fs::path& path, const Token& import_token) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw ModuleError(import_token,
            fmt::format("cannot open module file: {}", path.string()));
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

bool starts_with_relative_or_absolute(const std::string& s) {
    if (s.empty()) return false;
    if (s.rfind("./", 0) == 0 || s.rfind("../", 0) == 0)   return true;
    if (s.rfind(".\\", 0) == 0 || s.rfind("..\\", 0) == 0) return true;
    if (s.front() == '/' || s.front() == '\\')             return true;
    if (s.size() >= 2 && std::isalpha(static_cast<unsigned char>(s[0])) && s[1] == ':') return true;
    return false;
}

// True when the path looks like a shared library on any of our target
// platforms — checked regardless of host so a script can name a `.dll`
// directly even on a non-matching OS (the actual load failure surfaces
// later from DynamicLibrary). Match is case-insensitive on the extension.
bool is_native_library_path(const fs::path& p) {
    std::string ext = p.extension().string();
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".dll" || ext == ".so" || ext == ".dylib";
}

// Result of resolving a dep dir's entry point: either a bnl source file or
// a native shared library. Resolved canonical path either way.
struct DepEntry {
    bool     is_native;
    fs::path path;
};

// Given a dep dir, resolve its entry point. Tries in order:
//   1. bnl.json `native` field   -> .dll/.so/.dylib (FFI plugin)
//   2. bnl.json `main`   field   -> .bnl source file
//   3. index.bnl                 -> .bnl source file (Node-style fallback)
//   4. <dep_name>.bnl            -> .bnl source file (single-file dep)
//
// Lenient: malformed json or missing fields are warnings, not errors —
// resolution falls through to the next form.
std::optional<DepEntry> resolve_dep_entry(const fs::path&    dep_dir,
                                          const std::string& dep_name) {
    std::error_code ec;

    fs::path manifest = dep_dir / "bnl.json";
    if (fs::exists(manifest, ec) && !ec) {
        std::ifstream in(manifest, std::ios::binary);
        if (in) {
            std::ostringstream buf;
            buf << in.rdbuf();
            try {
                auto j = nlohmann::json::parse(buf.str());
                if (j.is_object()) {
                    if (auto it = j.find("native"); it != j.end() && it->is_string()) {
                        fs::path entry = (dep_dir / it->get<std::string>()).lexically_normal();
                        if (fs::exists(entry, ec)) {
                            return DepEntry{true, fs::weakly_canonical(entry, ec)};
                        }
                    }
                    if (auto it = j.find("main"); it != j.end() && it->is_string()) {
                        fs::path entry = (dep_dir / it->get<std::string>()).lexically_normal();
                        if (fs::exists(entry, ec)) {
                            return DepEntry{false, fs::weakly_canonical(entry, ec)};
                        }
                    }
                }
            } catch (const nlohmann::json::parse_error& e) {
                fmt::print(stderr, "warning: bnl.json at {} is invalid: {}\n",
                           manifest.string(), e.what());
            }
        }
    }
    fs::path index = dep_dir / "index.bnl";
    if (fs::exists(index, ec))  return DepEntry{false, fs::weakly_canonical(index, ec)};
    fs::path named = dep_dir / (dep_name + ".bnl");
    if (fs::exists(named, ec))  return DepEntry{false, fs::weakly_canonical(named, ec)};
    return std::nullopt;
}

// Walk up from `start` looking for `<ancestor>/deps/<name>/`. Same hoisting
// semantics as Node's node_modules walk: a script under tests/ resolves the
// same dep as one under src/ as long as deps/ lives at any common ancestor.
std::optional<fs::path> find_dep_in_walk(const fs::path&    start,
                                         const std::string& name) {
    std::error_code ec;
    fs::path d = start;
    while (true) {
        fs::path candidate = d / "deps" / name;
        if (fs::exists(candidate, ec) && fs::is_directory(candidate, ec)) {
            return candidate;
        }
        if (!d.has_parent_path() || d == d.parent_path()) break;
        d = d.parent_path();
    }
    return std::nullopt;
}

// Find the project root (first ancestor with bnl.json that is NOT itself a
// dep dir under some `deps/`). Returning a path means "we are in a project,"
// which gates Layer 4 (global) — projects must declare their deps explicitly
// so resolution stays reproducible across machines.
std::optional<fs::path> find_project_root(const fs::path& start) {
    std::error_code ec;
    fs::path d = start;
    while (true) {
        bool is_dep_dir = d.has_parent_path()
                       && d.parent_path().filename() == "deps";
        if (!is_dep_dir && fs::exists(d / "bnl.json", ec)) {
            return d;
        }
        if (!d.has_parent_path() || d == d.parent_path()) break;
        d = d.parent_path();
    }
    return std::nullopt;
}

// Resolve the global deps directory: $BNL_HOME/deps if set, else
// $USERPROFILE/.bnl/deps on Windows, $HOME/.bnl/deps elsewhere.
std::optional<fs::path> bnl_global_deps_dir() {
    if (const char* h = std::getenv("BNL_HOME"); h && *h) {
        return fs::path(h) / "deps";
    }
#ifdef _WIN32
    if (const char* h = std::getenv("USERPROFILE"); h && *h) {
        return fs::path(h) / ".bnl" / "deps";
    }
#else
    if (const char* h = std::getenv("HOME"); h && *h) {
        return fs::path(h) / ".bnl" / "deps";
    }
#endif
    return std::nullopt;
}

std::optional<fs::path> find_global_dep(const std::string& name) {
    auto root = bnl_global_deps_dir();
    if (!root) return std::nullopt;
    fs::path candidate = *root / name;
    std::error_code ec;
    if (fs::exists(candidate, ec) && fs::is_directory(candidate, ec)) {
        return candidate;
    }
    return std::nullopt;
}

#ifdef BNL_FFI_ENABLED
// FFI plugin DLLs must outlive every Module/Callable that references them —
// closures inside loaded plugins hold raw function pointers into the DLL's
// text segment, and `std::function` destructors call back into that text
// segment when the captured lambda type is destroyed.
//
// A Meyer's singleton gives us process-lifetime storage. By the time it
// destructs (during static teardown after main() returns), every stack-bound
// Interpreter has already been destroyed and every Module/Environment chain
// has unwound, so the lambda destructors have already run.
//
// Also serves as a dedupe layer: multiple Interpreters in the same process
// share one HMODULE per canonical DLL path.
struct LoadedLibrary {
    std::unique_ptr<DynamicLibrary> lib;
    ModulePtr                       module;
};

std::unordered_map<std::string, LoadedLibrary>& process_libraries() {
    static std::unordered_map<std::string, LoadedLibrary> libs;
    return libs;
}
#endif  // BNL_FFI_ENABLED

}  // namespace

fs::path ModuleLoader::resolve_file(const std::string&            path_string,
                                     const fs::path&               requesting_dir,
                                     const Token&                  import_token) {
    fs::path raw(path_string);
    fs::path candidate = raw.is_absolute() ? raw : (requesting_dir / raw);

    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(candidate, ec);
    if (ec || !fs::exists(canonical)) {
        throw ModuleError(import_token,
            fmt::format("module not found: {} (resolved to {})",
                        path_string, canonical.string()));
    }
    return canonical;
}

ModulePtr ModuleLoader::load_canonical_file(const fs::path& canonical,
                                             const Token&    import_token) {
    std::string key = canonical.string();
    if (auto it = cache_.find(key); it != cache_.end()) return it->second;
    if (in_progress_.count(key)) {
        throw ModuleError(import_token,
            fmt::format("circular import detected: {}", canonical.string()));
    }

    in_progress_.insert(key);
    ModulePtr m;
    try {
        m = evaluate_source(key, canonical.string(),
                            read_file_to_string(canonical, import_token),
                            import_token);
    } catch (...) {
        in_progress_.erase(key);
        throw;
    }
    in_progress_.erase(key);
    cache_[key] = m;
    return m;
}

ModulePtr ModuleLoader::load_native_library(const fs::path& canonical,
                                             const Token&    import_token) {
#ifndef BNL_FFI_ENABLED
    throw ModuleError(import_token, fmt::format(
        "this build of bnl was compiled without FFI plugin support, so the "
        "native plugin '{}' cannot be loaded. Rebuild bnl with "
        "-DBNL_ENABLE_FFI=ON, or remove the 'native' field from the dep's "
        "bnl.json and provide a bnl 'main' instead.",
        canonical.string()));
#else
    std::string key = "<native:" + canonical.string() + ">";

    // Per-loader cache: short-circuit re-imports within this Interpreter.
    if (auto it = cache_.find(key); it != cache_.end()) return it->second;

    // Process-wide cache: if another Interpreter (or a previous run) already
    // loaded this DLL, reuse the same Module. Same DLL handle, same module.
    auto& procs = process_libraries();
    if (auto it = procs.find(key); it != procs.end()) {
        cache_[key] = it->second.module;
        return it->second.module;
    }

    std::unique_ptr<DynamicLibrary> lib;
    try {
        lib = std::make_unique<DynamicLibrary>(canonical);
    } catch (const std::exception& e) {
        throw ModuleError(import_token, e.what());
    }

    using EntryPoint = ModulePtr (*)(Interpreter&);
    auto entry = reinterpret_cast<EntryPoint>(lib->symbol("bnl_load"));
    if (!entry) {
        throw ModuleError(import_token, fmt::format(
            "native plugin '{}' has no 'bnl_load' export — see <bnl/plugin.h>",
            canonical.string()));
    }

    ModulePtr m;
    try {
        m = entry(interp_);
    } catch (const std::exception& e) {
        throw ModuleError(import_token, fmt::format(
            "native plugin '{}' bnl_load threw: {}",
            canonical.string(), e.what()));
    }
    if (!m) {
        throw ModuleError(import_token, fmt::format(
            "native plugin '{}' bnl_load returned null",
            canonical.string()));
    }

    // Stash both the DLL handle and the Module in process-lifetime storage.
    // The DLL must outlive every callable that closes over its lambdas —
    // std::function destructors call into the DLL's text segment when the
    // captured lambda type is destroyed. Static-storage duration ensures the
    // unload happens after every Interpreter's destructors have run.
    procs.emplace(key, LoadedLibrary{std::move(lib), m});
    cache_[key] = m;
    return m;
#endif  // BNL_FFI_ENABLED
}

ModulePtr ModuleLoader::load(const std::string&            path_string,
                              const fs::path&               requesting_dir,
                              const Token&                  import_token) {
    // Relative or absolute path -> straight to file resolution. A path that
    // ends in .dll / .so / .dylib is loaded as a native plugin; anything else
    // is loaded as bnl source.
    if (starts_with_relative_or_absolute(path_string)) {
        fs::path canonical = resolve_file(path_string, requesting_dir, import_token);
        return is_native_library_path(canonical)
            ? load_native_library(canonical, import_token)
            : load_canonical_file(canonical, import_token);
    }

    // ---- Bare-name resolution chain --------------------------------------
    //
    // 1. Built-in native modules (sys, io, timers, …) <- src/stdlib/*.cpp
    // 2. Embedded bnl stdlib (web, request, url, …)   <- lib/*.bnl, baked at
    //                                                    build time into
    //                                                    stdlib_embedded.h
    // 3. Walk up: <ancestor>/deps/<name>/             <- project-local deps
    //                                                    (entry: bnl.json main /
    //                                                     native, else index.bnl)
    // 4. Global:  $BNL_HOME/deps/<name>/ or
    //             ~/.bnl/deps/<name>/                 <- only outside a project
    // 5. Error.

    // 1. Native modules.
    if (auto m = interp_.native_module(path_string); m) {
        return m;
    }

    // 2. Embedded bnl stdlib — source baked into the binary at build time.
    //    Cached + cycle-protected the same way file imports are.
    if (auto eit = embedded_stdlib().find(path_string); eit != embedded_stdlib().end()) {
        std::string key = "<embedded:" + path_string + ">";
        if (auto cit = cache_.find(key); cit != cache_.end()) return cit->second;
        if (in_progress_.count(key)) {
            throw ModuleError(import_token,
                fmt::format("circular embedded import: {}", path_string));
        }
        in_progress_.insert(key);
        ModulePtr m;
        try {
            m = evaluate_source(key, key, eit->second, import_token);
        } catch (...) {
            in_progress_.erase(key);
            throw;
        }
        in_progress_.erase(key);
        cache_[key] = m;
        return m;
    }

    // 2. Walk up looking for deps/<name>/.
    if (auto dep_dir = find_dep_in_walk(requesting_dir, path_string)) {
        if (auto entry = resolve_dep_entry(*dep_dir, path_string)) {
            return entry->is_native
                ? load_native_library(entry->path, import_token)
                : load_canonical_file(entry->path, import_token);
        }
        throw ModuleError(import_token, fmt::format(
            "dep '{}' found at {} but has no entry point "
            "(no bnl.json with 'native' or 'main', no index.bnl, no {}.bnl)",
            path_string, dep_dir->string(), path_string));
    }

    // 3. Global, gated on "are we outside a project?". Inside a project, deps
    //    must be declared in deps/ for reproducibility — global never silently
    //    fills the gap.
    bool in_project = find_project_root(requesting_dir).has_value();
    if (!in_project) {
        if (auto dep_dir = find_global_dep(path_string)) {
            if (auto entry = resolve_dep_entry(*dep_dir, path_string)) {
                return entry->is_native
                    ? load_native_library(entry->path, import_token)
                    : load_canonical_file(entry->path, import_token);
            }
            throw ModuleError(import_token, fmt::format(
                "global dep '{}' at {} has no entry point",
                path_string, dep_dir->string()));
        }
    }

    // 4. Error — exhausted every layer.
    throw ModuleError(import_token, fmt::format(
        "no module named '{}' (not a built-in, not in any deps/ ancestor of {}{})",
        path_string, requesting_dir.string(),
        in_project ? "" : ", not in global ~/.bnl/deps/"));
}

ModulePtr ModuleLoader::evaluate_source(const std::string& cache_key,
                                         const std::string& display_path,
                                         std::string        source,
                                         const Token&       import_token) {
    (void)cache_key;  // currently only used as map key by caller

    auto m = std::make_shared<Module>();
    m->set_path(display_path);
    m->set_source(std::move(source));

    Lexer lexer(m->source());
    auto  tokens = lexer.tokenize();
    if (lexer.has_errors()) {
        const auto& d = lexer.diagnostics().front();
        throw ModuleError(import_token,
            fmt::format("lex error in {}:{}:{}: {}",
                        display_path, d.line, d.column, d.message));
    }

    Parser parser(std::move(tokens));
    auto   program = parser.parse();
    if (parser.has_errors()) {
        const auto& d = parser.diagnostics().front();
        throw ModuleError(import_token,
            fmt::format("parse error in {}:{}:{}: {}",
                        display_path, d.line, d.column, d.message));
    }
    m->set_program(std::move(program));

    auto env = std::make_shared<Environment>(interp_.globals());
    m->set_exports(env);

    interp_.run_module(*m);
    return m;
}

}  // namespace bnl
