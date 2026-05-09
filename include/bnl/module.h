#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "bnl/ast.h"

namespace bnl {

class Environment;

// A loaded bnl module. Owns its source bytes, its parsed AST, and its
// top-level environment. Heap-allocated and held via shared_ptr because
// closures defined in the module hold raw pointers into its AST.
class Module {
public:
    Module() = default;

    void set_path   (std::string p)                       { path_    = std::move(p); }
    void set_source (std::string s)                       { source_  = std::move(s); }
    void set_program(std::vector<StmtPtr> p)              { program_ = std::move(p); }
    void set_exports(std::shared_ptr<Environment> env)    { exports_ = std::move(env); }

    const std::string&                  path()    const { return path_;    }
    const std::string&                  source()  const { return source_;  }
    const std::vector<StmtPtr>&         program() const { return program_; }
    const std::shared_ptr<Environment>& exports() const { return exports_; }

private:
    std::string                  path_;
    std::string                  source_;
    std::vector<StmtPtr>         program_;
    std::shared_ptr<Environment> exports_;
};

using ModulePtr = std::shared_ptr<Module>;

}  // namespace bnl
