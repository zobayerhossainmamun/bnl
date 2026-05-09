#pragma once

#include <memory>
#include <string>
#include <utility>

#include "bnl/module.h"
#include "bnl/value.h"

namespace bnl {

class Environment;

// Builder for a native module. Produces a ModulePtr that user bnl code
// reaches via `import "name" as alias;` (the same mechanism used for
// user files and embedded stdlib). Internally a native module is just a
// Module whose exports environment was populated by C++ instead of by
// evaluating bnl source.
class NativeModule {
public:
    explicit NativeModule(std::string name);

    // Bind a callable native function as a member of the module.
    NativeModule& add_function(std::string fn_name, int arity, NativeFunction::Fn fn);

    // Bind a constant value (string / number / bool / etc.) as a member.
    NativeModule& add_value(std::string name, Value v);

    // Finalize and return the Module. The same NativeModule should not be
    // build()-ed twice.
    ModulePtr build();

private:
    std::string                  name_;
    std::shared_ptr<Environment> env_;
};

}  // namespace bnl
