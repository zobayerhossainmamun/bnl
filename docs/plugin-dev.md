# Plugin development

How to author a native plugin — a shared library (`.dll` / `.so` / `.dylib`) that
bnl loads at runtime and exposes as a module.

## When to write a plugin

- You need to call a C/C++ library (sqlite, openssl, zlib, libcurl, ...).
- You need raw OS APIs (sockets, file system, IPC).
- A pure-bnl implementation is too slow.

For prototyping or simple wrappers, a [pure-bnl dep](./deps-dev.md) is cheaper.

## The contract

A plugin is a shared library that exports exactly one symbol:

```cpp
extern "C" BNL_PLUGIN_EXPORT bnl::ModulePtr bnl_load(bnl::Interpreter&);
```

When a script imports the plugin, the runtime opens the library, looks up
`bnl_load`, calls it once, and registers the returned `Module` under the name
the script used. Subsequent imports return the same `Module` — `bnl_load` is
called once per process.

## Minimal plugin

Same shape as `examples/plugin_native/mathx.cpp`:

```cpp
// mathx.cpp
#include <bnl/plugin.h>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" BNL_PLUGIN_EXPORT bnl::ModulePtr bnl_load(bnl::Interpreter&) {
    return bnl::NativeModule("mathx")
        .add_function("cube", 1,
            [](bnl::Interpreter&, std::vector<bnl::Value> args) -> bnl::Value {
                if (!args[0].is_number())
                    throw std::runtime_error("mathx.cube expects a number");
                double n = args[0].as_number();
                return bnl::Value{n * n * n};
            })
        .add_function("hypot", 2,
            [](bnl::Interpreter&, std::vector<bnl::Value> args) -> bnl::Value {
                return bnl::Value{std::hypot(args[0].as_number(),
                                             args[1].as_number())};
            })
        .add_value("greeting",
                   bnl::Value{std::string{"hi from a plugin"}})
        .build();
}
```

`BNL_PLUGIN_EXPORT` resolves to `__declspec(dllexport)` on Windows and
`__attribute__((visibility("default")))` elsewhere.

## CMake setup

A plugin links `bnl_core` (for the `bnl::*` types it uses) and produces a
shared library. Today there is **no installed package** for `bnl_core`, so
the plugin must include bnl's source as a subdirectory:

```cmake
cmake_minimum_required(VERSION 3.21)
project(mathx CXX)

set(CMAKE_CXX_STANDARD 20)

# Bring bnl_core into the build. Adjust the path to wherever your local
# bnl checkout lives. EXCLUDE_FROM_ALL keeps `make` from also building bnl.
add_subdirectory(/path/to/bnl bnl_build EXCLUDE_FROM_ALL)

add_library(mathx SHARED mathx.cpp)
target_link_libraries(mathx PRIVATE bnl_core)

# Plugins are looked up by exact filename — no "lib" prefix.
set_target_properties(mathx PROPERTIES
    PREFIX                       ""
    POSITION_INDEPENDENT_CODE    ON
)

# C4190: shared_ptr returned through extern "C". Documented v1 constraint.
if(MSVC)
    target_compile_options(mathx PRIVATE /wd4190)
endif()
```

A simpler path during development: drop your plugin's source under bnl's
`examples/plugin_native/` directory and add it to that CMakeLists. That's how
the in-tree `mathx` example is wired.

A future release will ship `find_package(bnl CONFIG REQUIRED)` so plugins can
build in their own repo without a bnl source checkout.

Output filenames per platform:

| OS | File |
|---|---|
| Windows | `mathx.dll` |
| Linux | `mathx.so` |
| macOS | `mathx.dylib` |

## Loading a plugin

A script can import a plugin three ways:

```bnl
// 1. Direct relative path
import "./build/mathx.dll" as m;

// 2. Direct absolute path
import "/usr/local/lib/mathx.so" as m;

// 3. Bare name resolved via deps/<name>/bnl.json with a "native" field
import "mathx" as m;
```

Form 3 is the project-friendly distribution shape. The dep's `bnl.json` looks
like this:

```json
{
    "name": "mathx",
    "native": "mathx.dll"
}
```

The `"native"` value is **relative to the dep directory**. Place the plugin file
next to the manifest:

```
my-app/
└── deps/
    └── mathx/
        ├── bnl.json
        └── mathx.dll
```

For multi-platform distribution, ship one dep dir per OS or substitute the
filename at install time. (A future package manager will handle this; for now,
the `bnl.json` is a static string.)

## Authoring API

Everything you need is in `<bnl/plugin.h>`, which transitively pulls
`<bnl/native_module.h>`, `<bnl/value.h>`, and `<bnl/interpreter.h>`.

### Building a module

```cpp
auto m = bnl::NativeModule("name")
    .add_function("fname", arity,
        [](bnl::Interpreter& interp, std::vector<bnl::Value> args) -> bnl::Value {
            // ...
        })
    .add_value("constant", bnl::Value{42.0})
    .build();
```

`arity` is the parameter count the runtime will enforce at the call site.

### Working with values

`bnl::Value` is a tagged union. Type predicates and accessors:

```cpp
v.is_null()      // bool
v.is_bool()
v.is_number()
v.is_string()
v.is_callable()  // function or class or bound method
v.is_module()
v.is_list()
v.is_map()
v.is_instance()  // user-defined class instance

v.as_bool()      // bool
v.as_number()    // double
v.as_string()    // const std::string&
v.as_list()      // ListPtr (shared_ptr<vector<Value>>)
v.as_map()       // MapPtr  (shared_ptr<unordered_map<string, Value>>)
v.as_callable()  // CallablePtr
v.as_module()    // ModulePtr
v.as_instance()  // InstancePtr

v.type_name()    // const char*: "null", "number", "string", ...
```

Constructing values:

```cpp
bnl::Value{};              // null
bnl::Value{true};          // bool
bnl::Value{3.14};          // number
bnl::Value{std::string{"hi"}};  // string

auto xs = std::make_shared<std::vector<bnl::Value>>();
xs->push_back(bnl::Value{1.0});
return bnl::Value{xs};

auto m = std::make_shared<std::unordered_map<std::string, bnl::Value>>();
(*m)["key"] = bnl::Value{std::string{"v"}};
return bnl::Value{m};
```

### Calling user-supplied callbacks

Many APIs take a bnl function (timer fires, async I/O completion, request
handlers). A `bnl::Value` wrapping a function is callable via:

```cpp
if (!args[0].is_callable())
    throw std::runtime_error("expected a function");
auto cb = args[0].as_callable();

std::vector<bnl::Value> argv;
argv.push_back(bnl::Value{42.0});
cb->call(interp, std::move(argv));
```

For **async** completion (libuv worker callbacks), see the patterns in
`src/stdlib/io.cpp` (`AsyncRead` / `on_read_done`) — the gist:

1. Heap-allocate a request struct holding the bnl callback + your output.
2. Submit a `uv_queue_work` (or any libuv handle) referencing it.
3. In the after-callback (main loop), construct the `(err, data)` arg vector
   and `cb->call(...)`. Wrap that call in a try/catch — uncaught errors should
   call `interp.mark_loop_failed()` to flip the run into a failure exit.

### Errors

Throw `std::runtime_error` (or any subclass of `std::exception`). The runtime
wraps it with the call site's line and column:

```cpp
.add_function("must_be_positive", 1,
    [](bnl::Interpreter&, std::vector<bnl::Value> args) -> bnl::Value {
        if (!args[0].is_number() || args[0].as_number() <= 0)
            throw std::runtime_error("expected a positive number");
        return bnl::Value{std::sqrt(args[0].as_number())};
    })
```

The script sees:

```
runtime error at <line>:<col> (near 'must_be_positive'): expected a positive number
```

## ABI rules — read this

The plugin and `bnl_core` exchange `std::shared_ptr`, `std::function`, and other
C++ standard-library types. There is **no C ABI surface yet**. Therefore:

> **Build the plugin with the same compiler, STL, runtime library, and bnl_core
> version as the bnl runtime that will load it.**

Concretely:

- Same major MSVC version (release-to-release `bnl_core` ABI is not stable).
- Same `_ITERATOR_DEBUG_LEVEL` / Debug vs Release runtime on Windows.
- Same libstdc++ / libc++ across processes on Linux/macOS.

If any of these mismatch you'll see crashes during plugin destruction, or
silently miscompiled `Value` / `Module` / `Environment` interactions.

A future C ABI surface will lift this, but it's not built today.

## Lifecycle and shutdown

`bnl_load` runs once per process. The runtime keeps the loaded library alive
in a process-lifetime singleton (`module_loader.cpp::process_libraries()`),
because lambdas captured by `std::function` inside the plugin's `Module` hold
function pointers into the plugin's text segment — unloading early would crash
during `Module` destruction.

Practical consequence: there is no plugin "unload". Side effects done in
`bnl_load` (registering atexit handlers, opening files, etc.) live for the
process lifetime.

## Distribution

For now, the user installs your plugin by hand:

1. Build the shared library on the target platform.
2. Drop it into `<their-project>/deps/<your-name>/`.
3. Add a `bnl.json` next to it with `{"name": "<your-name>", "native": "<filename>"}`.

A package manager will automate this in a future release.

## Reference example

The full working example is at `examples/plugin_native/mathx.cpp` and its
CMakeLists. The integration test at `tests/_fixtures/proj_full/` consumes it
through `deps/mathx-plugin/`.
