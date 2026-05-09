#include "module_loader.h"

#include <fmt/core.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <utility>

#include "environment.h"
#include "bnl/interpreter.h"
#include "lexer.h"
#include "parser.h"
#include "stdlib_embedded.h"

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

// Read `dep_dir/bnl.json` and return its `main` field, if any. Lenient: any
// failure (missing file, malformed json, no field) returns nullopt so we fall
// through to filename-based defaults.
std::optional<std::string> read_main_field(const fs::path& dep_dir) {
    fs::path manifest = dep_dir / "bnl.json";
    std::error_code ec;
    if (!fs::exists(manifest, ec) || ec) return std::nullopt;

    std::ifstream in(manifest, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream buf;
    buf << in.rdbuf();

    try {
        auto j = nlohmann::json::parse(buf.str());
        if (j.is_object()) {
            auto it = j.find("main");
            if (it != j.end() && it->is_string()) {
                return it->get<std::string>();
            }
        }
    } catch (const nlohmann::json::parse_error& e) {
        fmt::print(stderr, "warning: bnl.json at {} is invalid: {}\n",
                   manifest.string(), e.what());
    }
    return std::nullopt;
}

// Given a dep dir, resolve its entry point file. Tries:
//   1. bnl.json `main` field
//   2. index.bnl
//   3. <dep_name>.bnl  (e.g. deps/my-lib/my-lib.bnl)
std::optional<fs::path> resolve_dep_entry(const fs::path&    dep_dir,
                                          const std::string& dep_name) {
    std::error_code ec;
    if (auto main_field = read_main_field(dep_dir)) {
        fs::path entry = (dep_dir / *main_field).lexically_normal();
        if (fs::exists(entry, ec)) return fs::weakly_canonical(entry, ec);
    }
    fs::path index = dep_dir / "index.bnl";
    if (fs::exists(index, ec))  return fs::weakly_canonical(index, ec);
    fs::path named = dep_dir / (dep_name + ".bnl");
    if (fs::exists(named, ec))  return fs::weakly_canonical(named, ec);
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

ModulePtr ModuleLoader::load(const std::string&            path_string,
                              const fs::path&               requesting_dir,
                              const Token&                  import_token) {
    // Relative or absolute path -> straight to file resolution.
    if (starts_with_relative_or_absolute(path_string)) {
        fs::path canonical = resolve_file(path_string, requesting_dir, import_token);
        return load_canonical_file(canonical, import_token);
    }

    // ---- Bare-name resolution chain --------------------------------------
    //
    // 1. Native modules (sys, io, json, ...)         <- src/native/*.cpp
    // 2. Embedded bnl stdlib (prelude, path, http)   <- lib/*.bnl baked in
    // 3. Walk up: <ancestor>/deps/<name>/            <- project-local deps
    // 4. Global:  $BNL_HOME/deps/<name>/ or
    //             ~/.bnl/deps/<name>/                <- only outside a project
    // 5. Error.

    // 1. Native modules.
    if (auto m = interp_.native_module(path_string); m) {
        return m;
    }

    // 2. Embedded bnl stdlib.
    {
        const auto& stdlib = embedded_stdlib();
        if (auto it = stdlib.find(path_string); it != stdlib.end()) {
            std::string key          = "<stdlib:" + path_string + ">";
            std::string display_path = "<stdlib:" + path_string + ">";

            if (auto cached = cache_.find(key); cached != cache_.end()) return cached->second;
            if (in_progress_.count(key)) {
                throw ModuleError(import_token,
                    fmt::format("circular import detected: {}", display_path));
            }
            in_progress_.insert(key);
            ModulePtr m;
            try {
                m = evaluate_source(key, display_path, std::string(it->second), import_token);
            } catch (...) {
                in_progress_.erase(key);
                throw;
            }
            in_progress_.erase(key);
            cache_[key] = m;
            return m;
        }
    }

    // 3. Walk up looking for deps/<name>/.
    if (auto dep_dir = find_dep_in_walk(requesting_dir, path_string)) {
        if (auto entry = resolve_dep_entry(*dep_dir, path_string)) {
            return load_canonical_file(*entry, import_token);
        }
        throw ModuleError(import_token, fmt::format(
            "dep '{}' found at {} but has no entry point "
            "(no bnl.json with 'main', no index.bnl, no {}.bnl)",
            path_string, dep_dir->string(), path_string));
    }

    // 4. Global, gated on "are we outside a project?". Inside a project, deps
    //    must be declared in deps/ for reproducibility — global never silently
    //    fills the gap.
    bool in_project = find_project_root(requesting_dir).has_value();
    if (!in_project) {
        if (auto dep_dir = find_global_dep(path_string)) {
            if (auto entry = resolve_dep_entry(*dep_dir, path_string)) {
                return load_canonical_file(*entry, import_token);
            }
            throw ModuleError(import_token, fmt::format(
                "global dep '{}' at {} has no entry point",
                path_string, dep_dir->string()));
        }
    }

    // 5. Error — exhausted every layer.
    throw ModuleError(import_token, fmt::format(
        "no module named '{}' (not a native module, not in embedded stdlib, "
        "not in any deps/ ancestor of {}{})",
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
