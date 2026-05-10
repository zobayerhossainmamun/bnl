#pragma once

// Author a native plugin loaded by bnl at runtime via the dep resolver.
//
// A plugin is a shared library (.dll / .so / .dylib) that the dep loader picks
// up when its bnl.json declares a `"native"` field:
//
//     // deps/mathx/bnl.json
//     {
//         "name": "mathx",
//         "native": "build/mathx.dll"   // relative to the dep directory
//     }
//
// The plugin's main translation unit defines a single C-linkage entry point:
//
//     #include <bnl/plugin.h>
//     #include <stdexcept>
//
//     extern "C" BNL_PLUGIN_EXPORT bnl::ModulePtr bnl_load(bnl::Interpreter&) {
//         return bnl::NativeModule("mathx")
//             .add_function("cube", 1,
//                 [](bnl::Interpreter&, std::vector<bnl::Value> args) -> bnl::Value {
//                     if (!args[0].is_number())
//                         throw std::runtime_error("cube expects a number");
//                     double n = args[0].as_number();
//                     return bnl::Value{n * n * n};
//                 })
//             .build();
//     }
//
// IMPORTANT: the plugin must be built with the SAME compiler / STL / runtime
// as bnl_core. C++ types (notably std::shared_ptr in ModulePtr) cross the DLL
// boundary and require ABI compatibility. A future C ABI surface for bnl will
// remove this restriction; for now plan on shipping plugins per-toolchain.

#include "bnl/interpreter.h"
#include "bnl/module.h"
#include "bnl/native_module.h"

// bnl_load is declared `extern "C"` so the symbol name isn't mangled (the
// host's dlsym/GetProcAddress finds it by a stable spelling), but it returns
// a C++ type — bnl::ModulePtr — which clang warns about by default. The
// pattern is intentional: plugin and host must already share an STL ABI per
// the contract above. Silence the warning at the export macro so plugin
// authors don't see it.
#if defined(_WIN32)
  #define BNL_PLUGIN_EXPORT __declspec(dllexport)
#elif defined(__clang__)
  #define BNL_PLUGIN_EXPORT \
      _Pragma("clang diagnostic ignored \"-Wreturn-type-c-linkage\"") \
      __attribute__((visibility("default")))
#else
  #define BNL_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif
