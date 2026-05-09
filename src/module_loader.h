#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "bnl/module.h"
#include "bnl/token.h"

namespace bnl {

class Interpreter;

class ModuleError : public std::runtime_error {
public:
    Token token;
    ModuleError(Token t, std::string msg)
        : std::runtime_error(std::move(msg)), token(t) {}
};

class ModuleLoader {
public:
    explicit ModuleLoader(Interpreter& interp) : interp_(interp) {}

    // Resolves `path_string` relative to `requesting_dir` (for ./ and ../) and
    // returns the loaded module. `import_token` is used for error reporting.
    ModulePtr load(const std::string&             path_string,
                   const std::filesystem::path&   requesting_dir,
                   const Token&                   import_token);

private:
    std::filesystem::path resolve_file(const std::string&           path_string,
                                       const std::filesystem::path& requesting_dir,
                                       const Token&                 import_token);

    // Load a file by canonical path: cache hit, cycle check, read, evaluate.
    // Used by both the relative-path branch and the deps/ resolver.
    ModulePtr load_canonical_file(const std::filesystem::path& canonical,
                                  const Token&                 import_token);

    // Evaluate from raw source. `display_path` is what shows up in errors and
    // module.path; `cache_key` is what we put in the cache map.
    ModulePtr evaluate_source(const std::string& cache_key,
                              const std::string& display_path,
                              std::string        source,
                              const Token&       import_token);

    Interpreter& interp_;
    std::unordered_map<std::string, ModulePtr> cache_;        // canonical path -> Module
    std::unordered_set<std::string>            in_progress_;  // for cycle detection
};

}  // namespace bnl
