# Extending bnl

Four ways to add functionality to bnl, ordered from least invasive to
most invasive. Pick the one that matches your scope.

| Approach | Lives in | Toolchain | When |
|---|---|---|---|
| 1. Pure bnl file | User's project (`*.bnl`) | None (just bnl) | Logic expressible in bnl + existing modules |
| 2. Embedded `lib/*.bnl` | `lib/<name>.bnl` | bnl source builds the binary | Helper module that should ship with every bnl install |
| 3. Built-in C++ stdlib module | `src/stdlib/<name>.cpp` | C++20 + bnl source tree | Needs a system library or hot-path C++ code; ships in `bnl.exe` |
| 4. External C plugin | Your own repo, single-file C/C++ | C compiler + `include/bnl/plugin.h` | Out-of-tree, distributed independently |

## 1. Pure bnl modules

The simplest case: write `.bnl` and let users import it.

```
my-app/
├── bnl.json
├── main.bnl
└── deps/
    └── greetings/
        └── index.bnl
```

`deps/greetings/index.bnl`:
```bnl
function hello(name) {
    return "Hello, " + name + "!";
}
```

`main.bnl`:
```bnl
import "greetings" as g;
print(g.hello("world"));
```

Top-level `var` / `function` / `class` declarations become exports.
There is no `export` keyword and no named-import syntax — you access
exports through the module value.

**Entry point resolution** (inside `deps/<name>/`):
1. `bnl.json` `"main"` field → that file is the entry
2. `index.bnl` (Node-style fallback)
3. `<dep_name>.bnl` (single-file dep)

Cycles between modules are detected and reported. Each module evaluates
exactly once per Interpreter — top-level side effects (timers, server
starts, file I/O) fire once.

## 2. Embedded `lib/*.bnl` modules

When the helper should ship with every bnl install — not as a user dep
but as part of the standard library — drop the file in `lib/`. The
`embed_stdlib` build step bakes it into the binary.

**Adding `lib/foo.bnl`:**

1. Write the bnl source: `lib/foo.bnl`.
2. Reconfigure CMake so the GLOB picks up the new file:
   ```
   cmake --preset windows
   ```
   (ninja will warn `GLOB mismatch!` on the first build after the add — next build is clean.)
3. Add a test: `tests/stdlib/lib_foo.bnl` + `tests/stdlib/lib_foo.expected`.
4. Register the test: `add_bnl_test(lib_foo stdlib)` in `tests/CMakeLists.txt`.
5. Rebuild + run.

The module is now importable from any bnl script — no path, no dep
directory. Just `import "foo" as foo;`.

**Convention**: if your `lib/foo.bnl` wraps a low-level native module,
the native module's public name should be `_foo` (underscored). The
public `foo` belongs to your lib module. Existing examples:

- `_sqlite` (native, in `src/stdlib/sqlite.cpp`) ↔ `sqlite` (lib, in `lib/sqlite.bnl` — adds `transaction`, `migrate`, `insert` helpers).
- `_pg` (native) ↔ `pg` (lib — same shape).
- `_math` (native libm) ↔ `math` (lib — adds `min`, `max`, `clamp`, `lerp`).
- `_exec` (native) ↔ `exec` (lib — adds `run`, output capture helpers).

This split keeps the C++ surface minimal and lets ergonomic helpers
evolve in bnl source.

## 3. Built-in C++ stdlib module

For a module that needs a system library, OS APIs, or measurable
performance, write it in C++ and link it into `bnl_core`. This is how
all 18 native modules work today.

### File layout

```
src/stdlib/foo.cpp                              ← the module
include/bnl/native_module.h                     ← (existing) builder API
src/stdlib/registry.h                           ← add declaration here
src/runtime/interpreter.cpp                     ← add register_foo(*this) here
src/CMakeLists.txt                              ← add source + any new link libs
CMakeLists.txt                                  ← add find_package for any new vcpkg dep
vcpkg.json                                      ← add the dep itself
tests/stdlib/native_foo.{bnl,expected}          ← test fixtures
tests/CMakeLists.txt                            ← add_bnl_test(native_foo stdlib)
resources/LICENSE.txt                           ← add attribution for new dep
```

### The pattern

```cpp
// src/stdlib/foo.cpp
#include "stdlib/registry.h"
#include "bnl/interpreter.h"
#include "bnl/native_module.h"
#include "bnl/value.h"

namespace bnl {

void register_foo(Interpreter& interp) {
    auto m = NativeModule("foo")
        .add_function("greet", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                if (!args[0].is_string())
                    throw std::runtime_error("foo.greet: name must be a string");
                return Value{std::string("Hello, ") + args[0].as_string()};
            })
        .add_function("add", 2,
            [](Interpreter&, std::vector<Value> args) -> Value {
                return Value{args[0].as_number() + args[1].as_number()};
            })
        .build();

    interp.register_native_module("foo", m);
}

}  // namespace bnl
```

Then declare in `src/stdlib/registry.h`:
```cpp
void register_foo(Interpreter& interp);
```

And call from `src/runtime/interpreter.cpp` ctor:
```cpp
register_foo(*this);
```

Source file gets added to `bnl_core`'s sources in `src/CMakeLists.txt`:
```cmake
add_library(bnl_core STATIC
    ...
    stdlib/foo.cpp      # NEW
    ...
)
```

### Arity conventions

- Fixed arity: `add_function("name", N, ...)`.
- Variadic: `add_function("name", -1, ...)`; check `args.size()` yourself
  and throw a clear error if it's wrong.
- Defaults: not supported on native functions. If you need optional args,
  use `-1` and inspect `args.size()`.

### Lifetimes inside native callbacks

If your function captures resources that may outlive the call (a libuv
handle, a libffi closure, anything async), use the `shared_ptr<bool> alive`
flag idiom — capture it in your callback, set it to false when the resource
closes, and check `*alive` before invoking any user-provided bnl callback.

Example pattern (from `src/stdlib/net.cpp`):

```cpp
struct WriteReq {
    uv_write_t            req{};
    std::string           data;
    CallablePtr           on_done;
    Interpreter*          interp = nullptr;
    std::shared_ptr<bool> alive;    // ← conn's "still here?" flag
};

void on_write_done(uv_write_t* req, int status) {
    auto* wr = static_cast<WriteReq*>(req->data);
    if (wr->on_done && wr->alive && *wr->alive) {
        Value err = (status < 0)
            ? Value{std::string(uv_strerror(status))}
            : Value{};
        invoke_cb(*wr->interp, wr->on_done, { std::move(err) });
    }
    delete wr;
}
```

The flag is shared between the conn's close hook (which flips it false)
and every pending callback (which checks before firing). Native modules
that wrap async resources should follow this pattern — it's the
established idiom.

### Throwing errors

`throw std::runtime_error("foo.bar: clear message");` from inside a
native function. The interpreter wraps it into a bnl runtime error that
user code can `try/catch`. Always prefix with the API name (`foo.bar:`)
so users can locate the source.

### When you add a new vcpkg dependency

1. Add to `vcpkg.json`:
   ```json
   { "name": "mylib", "version>=": "1.2.3" }
   ```
2. Add `find_package` to top-level `CMakeLists.txt`.
3. Add the link target to `src/CMakeLists.txt` under `bnl_core`'s
   `PRIVATE` block.
4. **Add license attribution to `resources/LICENSE.txt`.** Copy the
   relevant `share/<pkg>/copyright` from
   `build/<preset>/vcpkg_installed/x64-windows/share/` — both the
   metadata block (top section) and the full license body. Required for
   any dep that ends up statically linked into the release binary.

## 4. External C plugin

For functionality distributed **outside** the bnl repo — a third-party
library binding, an in-house C library, anything you don't want baked
into the bnl binary.

The contract is `include/bnl/plugin.h` — a single C header. No C++
link required; the plugin compiles to a shared library with one export
named `bnl_load`. See `examples/plugin_c/` for a complete working example.

### Minimal plugin

`my_plugin.c`:
```c
#include "bnl/plugin.h"
#include <string.h>

static bnl_value* hello(const bnl_api* api, int argc, bnl_value** argv, void* userdata) {
    (void)userdata;
    if (argc != 1 || api->get_type(argv[0]) != BNL_TYPE_STRING) {
        api->throw_error(api, "my_plugin.hello: expected one string arg");
        return NULL;
    }
    size_t len = 0;
    const char* name = api->get_string(argv[0], &len);

    char buf[256];
    int n = snprintf(buf, sizeof(buf), "Hello, %.*s!", (int)len, name);
    return api->make_string(api, buf, (size_t)n);
}

BNL_EXPORT bnl_module* bnl_load(const bnl_api* api) {
    bnl_module* m = api->module_new(api, "my_plugin");
    api->module_add_function(m, "hello", 1, hello, NULL);
    return m;
}
```

Build it as a shared library (any toolchain that produces a
`.dll`/`.so`/`.dylib` works). Then import:

```bnl
import "./my_plugin.dll" as p;     // direct path import
print(p.hello("world"));
```

Or distribute it as a dep:
```
my-app/deps/my-plugin/
├── bnl.json     {"name": "my-plugin", "native": "my_plugin.dll"}
└── my_plugin.dll
```

Then in bnl:
```bnl
import "my-plugin" as p;
print(p.hello("world"));
```

### Plugin API surface

All of these come off the `bnl_api*` table passed to `bnl_load` and
to every native function call. Allocated values are owned by bnl and
live until the current call returns.

| Category | Functions |
|---|---|
| Type tags | `BNL_TYPE_NULL`, `_BOOL`, `_NUMBER`, `_STRING`, `_LIST`, `_MAP`, `_OTHER` |
| Constructors | `make_null`, `make_bool`, `make_number`, `make_string`, `make_list`, `make_map` |
| Accessors | `get_type`, `get_bool`, `get_number`, `get_string` (returns ptr + len, NOT NUL-terminated) |
| List ops | `list_length`, `list_get`, `list_push` |
| Map ops | `map_size`, `map_has`, `map_get`, `map_set`, `map_key_at` |
| Module construction | `module_new`, `module_add_function`, `module_add_value` |
| Error reporting | `throw_error` (call, return NULL) |

Things the plugin API explicitly **doesn't** provide:

- Custom value types (no opaque-handle slot). If you need to hand back
  a "handle", encode it as a number (address) or string (id) and look it
  up in your own static map.
- Callback registration into bnl. You can't pass a bnl function pointer
  to C and have C call back. (This is the "C callbacks" gap mentioned
  in the FFI Phase 3b discussion — not blocked by plugins, but not
  shipped either.)
- Direct access to lists/maps as views (everything goes through `_get`/`_set`).

### When to choose a plugin vs. a built-in stdlib module

**Built-in** (`src/stdlib/<name>.cpp`):
- Always available with every bnl install.
- Tied to bnl's release cadence (ships when bnl ships).
- Goes through code review with the bnl project.
- Cleanly tests against bnl's exact build configuration.

**External plugin**:
- Distributed independently from bnl releases.
- Ideal for proprietary code or third-party library wrappers.
- Plugin author owns the build, distribution, and versioning.
- Pure C ABI — works with any plugin toolchain (C, C++, Rust with
  `cdylib`, Zig, even Go via `c-shared`).

If you're writing a binding to a widely-used library that lots of bnl
users will want, propose it as a built-in. If it's specific to your app
or proprietary, ship it as a plugin.

## Embedding bnl in another C++ host

If you want to run bnl scripts from a C++ application (vs. running them
through `bnl.exe`), the embedding surface is `<bnl/interpreter.h>`:

```cpp
#include "bnl/interpreter.h"
#include "bnl/native_module.h"

int main() {
    bnl::Interpreter::install_signal_handlers();
    bnl::Interpreter interp;

    // Optional: register a custom native module.
    auto m = bnl::NativeModule("host")
        .add_function("ping", 0,
            [](bnl::Interpreter&, std::vector<bnl::Value>) -> bnl::Value {
                return bnl::Value{std::string("pong")};
            })
        .build();
    interp.register_native_module("host", m);

    bool ok = interp.run_source(
        "import \"host\" as host; print(host.ping());");
    return ok ? 0 : 1;
}
```

Link against `bnl_core` (the static lib produced by the bnl build). The
embedding API is everything in `include/bnl/*.h`. Anything in `src/` is
internal and not part of the contract.

## Testing your extension

For built-ins and embedded lib modules, the test pattern is:

1. Write `tests/stdlib/<name>.bnl` exercising the API.
2. Write `tests/stdlib/<name>.expected` with the exact stdout it should produce.
3. Register: `add_bnl_test(<name> stdlib)` in `tests/CMakeLists.txt`.
4. Run: `ctest --test-dir build/windows -R <name> --output-on-failure`.

`run_bnl_test.cmake` does the comparison — exact stdout match. If your
test prints something non-deterministic (timestamps, addresses, etc.),
normalize it inside the bnl script before printing.

For external plugins, write tests in your own repo against your local
build of bnl + the plugin's `.dll`.
