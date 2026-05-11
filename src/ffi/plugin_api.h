#pragma once

#include <filesystem>

#include "bnl/module.h"
#include "ffi/dynamic_library.h"

namespace bnl {

class Interpreter;

// Resolve `bnl_load` from `lib`, call it with bnl's C-API table, and return
// the resulting Module. The returned Module's callables retain pointers into
// `lib`'s text segment — caller must keep `lib` alive for the lifetime of
// any reference to the Module. Throws std::runtime_error on failure (missing
// export, plugin-side throw_error, version mismatch, etc.).
ModulePtr load_c_plugin_module(DynamicLibrary&              lib,
                                const std::filesystem::path& path,
                                Interpreter&                 interp);

}  // namespace bnl
