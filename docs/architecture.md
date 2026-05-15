# Architecture

A map of how bnl works — what runs, in what order, where each concern
lives in the source tree.

```
                    ┌─────────────────────────────────────────────────┐
                    │  bnl.exe  (single statically-linked binary)     │
                    └─────────────────────────────────────────────────┘
                                          │
                ┌─────────────────────────┴─────────────────────────┐
                │                                                   │
        ┌───────▼────────┐                            ┌─────────────▼────────────┐
        │   Frontend     │                            │     Runtime              │
        │ (src/frontend) │                            │  (src/runtime)           │
        ├────────────────┤                            ├──────────────────────────┤
        │ lexer.cpp      │                            │ value.cpp                │
        │ parser.cpp     │  ── AST (StmtPtr/ExprPtr) ─▶ environment.cpp           │
        │ ast_printer.cpp│                            │ interpreter.cpp          │
        └────────────────┘                            │ exprs/stmts/globals      │
                                                      │ class_type.cpp           │
                                                      │ future.cpp + async_*     │
                                                      │ module_loader.cpp        │
                                                      │ native_module.cpp        │
                                                      │ bn_aliases.cpp/h         │
                                                      └─────────┬────────────────┘
                                                                │
                                ┌───────────────────────────────┼────────────────────────┐
                                │                               │                        │
                       ┌────────▼────────┐         ┌────────────▼────────┐    ┌──────────▼──────────┐
                       │ Built-in stdlib │         │ Embedded lib/*.bnl  │    │ FFI plugin loader   │
                       │ (src/stdlib/*.cpp│         │ (lib/*.bnl baked    │    │ (src/ffi/*.cpp)     │
                       │  — 18 modules)   │         │  into binary at     │    │                     │
                       │  sys, io, net,   │         │  build time)        │    │ `import "foo.dll"`  │
                       │  http, tls,      │         │ 25 modules:         │    │ via include/bnl/    │
                       │  crypto, json,   │         │ web, request, cli,  │    │ plugin.h C ABI      │
                       │  pg, sqlite,     │         │ uuid, template, ... │    │                     │
                       │  mongo, ...      │         │                     │    │                     │
                       └─────────┬────────┘         └─────────────────────┘    └─────────────────────┘
                                 │
                       ┌─────────▼────────┐
                       │  libuv loop      │  ← TCP/HTTP/TLS/timers/fs/exec all live here
                       │  (uv_loop_t)     │
                       └──────────────────┘
```

## Execution flow — what happens when you run a script

`bnl.exe script.bnl` from the user's POV; under the hood:

1. **main.cpp** parses CLI flags, reads the script into a string, builds an
   `Interpreter`, and calls `interp.run_source(src, path)`.
2. **Lexer** (`src/frontend/lexer.cpp`) tokenizes UTF-8 source. The token
   stream understands both Bangla and English identifiers; keyword
   aliasing happens via `src/runtime/bn_aliases.cpp/h` — e.g. `চলক` and
   `var` both produce the same `VAR` token. Bilingual code in one file
   is a first-class case, not a hack.
3. **Parser** (`src/frontend/parser.cpp`) builds an AST of `Stmt` and
   `Expr` nodes (smart-pointer-owned via `StmtPtr`/`ExprPtr`, declared in
   `include/bnl/ast.h`). The parser produces both AST and the precise
   `Token` ranges for diagnostics.
4. **Interpreter** (`src/runtime/interpreter.cpp`) is the
   `ExprVisitor`+`StmtVisitor` that walks the AST node-by-node, evaluating
   expressions and executing statements. State lives in `Environment`
   chains (lexical scope) and the global `Interpreter` (modules, microtask
   queue, libuv loop).
5. When the script finishes (or hits an exception), the interpreter
   drains any **pending microtasks** then runs the **libuv loop**
   (`uv_run`) until no pending handles remain. Anything scheduled
   via `timers.set`, `io.read_file_async`, `tls.connect`, etc. fires
   during this drain phase.
6. **Exit code** = 0 if everything succeeded, non-zero if any
   RuntimeError escaped or any async callback called
   `Interpreter::mark_loop_failed`.

Use `bnl --tokens script.bnl` to dump step 2's output, `bnl --ast` for
step 3's. Both are undocumented dev flags — flagged in `main.cpp`.

## Value model

Every runtime value is a `bnl::Value` — a thin wrapper around a
`std::variant` (`include/bnl/value.h:41-52`). Ten possibilities:

| Variant | C++ storage | Notes |
|---|---|---|
| `null` | `std::monostate` | The only "no value" |
| `bool` | `bool` | |
| `number` | `double` | All numbers are doubles (no int distinction) |
| `string` | `std::string` | UTF-8 bytes; not enforced |
| `callable` | `shared_ptr<Callable>` | User functions, native functions, classes (callable as constructors), bound methods |
| `module` | `shared_ptr<Module>` | The result of `import "x" as x;` — a frozen environment exposing named exports |
| `list` | `shared_ptr<vector<Value>>` | Mutable, by-reference |
| `map` | `shared_ptr<unordered_map<string,Value>>` | Mutable, by-reference, **preserves insertion order** for keys[¹] |
| `instance` | `shared_ptr<Instance>` | A `class` instance with its field map |
| `future` | `shared_ptr<Future>` | Async-result handle; resolves/rejects |

[¹] Insertion order preservation is verified by `tests/stdlib/native_ffi.bnl`
(now removed) and `tests/lang/maps.bnl`. The implementation uses a custom
order-preserving map; the API surface is hash-map-like.

### Callable

Anything you can `()` is a `Callable` (`include/bnl/value.h:24-35`):

- `UserFunction` — a function defined in bnl source, holds raw pointers
  into the parsed AST (so the AST must outlive the function — handled
  via `Interpreter::kept_programs_`).
- `NativeFunction` — wraps a `std::function<Value(Interpreter&, vector<Value>)>`,
  the primary way C++ modules expose entry points.
- `BoundMethod` — `instance.method` returns this; pre-binds `self`.
- `ClassType` — calling a class invokes the constructor.

Variadic / optional-args is signaled by `arity() == -1` plus `min_arity()`.
User functions support defaults on the tail (`function foo(x, y = 10)`); the
parser captures the default expr, and `UserFunction::call` evaluates it
in the call's environment if the arg is missing.

## Environment / scope chain

`Environment` (internal: `src/runtime/environment.cpp/h`) is a singly-linked
parent chain. `var x = 1;` defines in the current env; identifier lookup
walks up. Assignment walks up too — closures can mutate outer-scope vars,
which is what the `done` flag pattern in the web responder relies on.

The `Interpreter` holds `globals_` (the root env, where built-in modules
and top-level user names live) plus `environment_` (the current scope
pointer, swapped during function calls / blocks).

## Module system

Four layers, ordered from closest-to-runtime to farthest:

### 1. NativeModule builder (the foundation)

`src/runtime/native_module.cpp` + `include/bnl/native_module.h`. A fluent
builder used by every C++ module:

```cpp
auto m = NativeModule("sqlite")
    .add_function("open",     1, [](Interpreter&, std::vector<Value> args) { ... })
    .add_function("version",  0, [](Interpreter&, std::vector<Value>)      { ... })
    .add_value   ("MAX_PARAMS", Value{static_cast<double>(SQLITE_LIMIT_VARIABLE_NUMBER)})
    .build();
interp.register_native_module("sqlite", m);
```

The result is a `Module` (`shared_ptr<Module>`) — a name-keyed bag of
`Value`s. `import "sqlite" as s;` binds `s` to this module.

### 2. Built-in stdlib modules (in `src/stdlib/`)

Each `src/stdlib/<name>.cpp` exposes one `register_<name>(Interpreter&)`
function. They are declared in `src/stdlib/registry.h` and called from
`Interpreter`'s constructor (`src/runtime/interpreter.cpp`). At process
start, every built-in module is registered before the first script runs.

Currently 18 built-ins:

| Module | Module name | What it wraps |
|---|---|---|
| `sys.cpp`     | `sys`     | process meta (args, env, exit, platform) |
| `io.cpp`      | `io`      | sync + async file I/O, streaming via libuv |
| `timers.cpp`  | `timers`  | `set`, `interval`, both via `uv_timer_t` |
| `regex.cpp`   | `regex`   | std::regex wrapper |
| `crypto.cpp`  | `crypto`  | OpenSSL: hashes, HMAC, base64, hex, random |
| `net.cpp`     | `net`     | TCP listen/connect + IPv4 DNS via libuv |
| `http.cpp`    | `http`    | llhttp parser (request/response, no sockets) |
| `tls.cpp`     | `tls`     | TLS client+server (OpenSSL BIO-pair + libuv) |
| `json.cpp`    | `json`    | nlohmann/json parse/stringify |
| `exec.cpp`    | `_exec`   | child process via libuv (underscored; public name lives in `lib/exec.bnl`) |
| `dns.cpp`     | `_dns`    | DNS lookup / reverse via libuv |
| `sqlite.cpp`  | `_sqlite` | sqlite3 open / exec / query / prepared stmts |
| `pg.cpp`      | `_pg`     | libpq client |
| `mongo.cpp`   | `_mongo`  | mongo-c-driver client |
| `math.cpp`    | `_math`   | libm wrapper |
| `random.cpp`  | `_random` | Mersenne-Twister primitives |
| `time.cpp`    | `_time`   | wall + monotonic clock, tm-struct |
| `zlib.cpp`    | `_zlib`   | gzip + raw deflate/inflate |

**Underscore convention:** `_foo` is the low-level native module; the
public name `foo` resolves to `lib/foo.bnl`, which wraps the native API
with ergonomic helpers. E.g. `_sqlite.exec(db, sql)` exists but you
normally do `import "sqlite" as sqlite; sqlite.transaction(db, fn);`.

### 3. Embedded `lib/*.bnl` modules

Pure-bnl helper modules under `lib/` are **baked into the binary at
build time**. The mechanism:

- `cmake/embed_stdlib.cmake` globs `lib/*.bnl` and generates a header at
  `build/<preset>/generated/stdlib_embedded.h` containing a single function
  `embedded_stdlib()` that returns a `unordered_map<string,string>`
  (module name → source text).
- `bnl_core` depends on a custom CMake target that regenerates this header
  when any `lib/*.bnl` changes (`CONFIGURE_DEPENDS` triggers re-glob).
- `ModuleLoader` (in `src/runtime/module_loader.cpp`) consults the
  embedded table as **resolution step 2** — after native modules, before
  the `deps/` walk.
- Cache key for embedded modules is `<embedded:<name>>` so they dedupe
  correctly across imports and never collide with on-disk files.

25 modules currently embedded: web, request, sqlite, pg, mongo, math,
random, time, zlib, exec, dns, log, dotenv, cli, uuid, csv, multipart,
template, test, url, io, path, ws, cookie, session.

After adding/removing a `lib/*.bnl`, **reconfigure** (`cmake --preset windows`).
Ninja may warn on first build with "GLOB mismatch!" — the next build will
be clean.

### 4. External plugins (FFI loader)

`src/ffi/dynamic_library.cpp` is the cross-platform `LoadLibrary`/`dlopen`
wrapper. `src/ffi/plugin_api.cpp` exposes the `bnl_api` function table
that plugin authors call back into to construct values, build modules, and
throw errors.

The contract is **pure C ABI** (`include/bnl/plugin.h`):

```c
BNL_EXPORT bnl_module* bnl_load(const bnl_api* api) {
    bnl_module* m = api->module_new(api, "mymod");
    api->module_add_function(m, "greet", 1, my_greet, NULL);
    return m;
}
```

Plugins are loaded by writing `import "./mymod.dll" as mymod;` (direct
path) or by putting them under a `deps/<name>/` with a `bnl.json` carrying
`"native": "mymod.dll"`. The loader calls `bnl_load`, wraps the returned
`bnl_module*` into a `Module`, and the import returns it. See
`examples/plugin_c/` for a complete example (a `mathx` plugin in C).

Note: this **plugin loader** is unrelated to the **generic FFI module**
(which doesn't currently ship). The plugin loader is "the library was
written for bnl"; a generic FFI would be "call any C function in any DLL".
The latter was prototyped and removed during 2.0.1 development.

## Import resolution (`module_loader.cpp`)

When the interpreter hits `import "X" as alias;`, this is the chain:

1. **Path-like** (`./`, `../`, `/`, `\`, drive letter):
   - Ends in `.dll`/`.so`/`.dylib` → load as plugin.
   - Otherwise → load as bnl source.
2. **Bare name** — try in order:
   1. **Built-in native module** — `Interpreter::native_modules_` lookup.
   2. **Embedded `lib/*.bnl`** — `embedded_stdlib()` table.
   3. **Walk-up `deps/<name>/`** — from the importing file's directory
      upward, looking for `<ancestor>/deps/<name>/`. First match wins.
      Inside the dep dir, entry point resolution tries:
      - `bnl.json` `"native"` field → plugin
      - `bnl.json` `"main"` field → bnl source
      - `index.bnl` (Node-style fallback)
      - `<dep_name>.bnl` (single-file dep)
   4. **Global `~/.bnl/deps/<name>/`** — only when **outside** a project
      (a project is a directory containing `bnl.json` at its root).
   5. Error: unresolved module.

Each module is **cached by canonical path**. Re-importing returns the same
Value; the source evaluates exactly once per Interpreter lifetime. Cycles
are detected (the loader maintains an in-flight set) and reported.

## Async model

bnl exposes async via two complementary mechanisms:

### Callback-style (the libuv idiom)

I/O modules (`io`, `net`, `tls`, `timers`, `_exec`, `_dns`) take callbacks
following the Node convention `(err, value)`:

```bnl
io.read_file_async("a.txt", function (err, data) {
    if (err != null) { print("err:", err); return; }
    print("got", data.length, "bytes");
});
```

Under the hood: the native module wraps a libuv handle, the bnl callback
is stored in a heap-allocated request struct. When libuv fires the
completion, the request struct's callback is invoked via `Interpreter::call`.

**Lifetime hazard**: the connection may close before the libuv callback
fires. The idiom is a `shared_ptr<bool> alive` flag — captured in the
request struct, set to `false` in the conn's close hook. Native modules
check `*alive` before invoking the bnl callback. See `src/stdlib/net.cpp:155`
and `src/stdlib/tls.cpp:on_socket_write_done` for the canonical pattern.

### Future + `wait` (the structured form)

`Future` (`src/runtime/future.cpp`, `include/bnl/value.h:22`) is a value
type representing pending async work, with `.then` / `.fail` / `.always`
methods. The `wait` keyword is statement+expression syntax:

```bnl
function load_user(id) {
    var user = wait db.query("SELECT * FROM users WHERE id = ?", [id]);
    return user;
}
```

`wait` suspends the current function body and re-enters via a microtask
when the Future resolves. The compiler doesn't see "this function is
async" — the body's traversal in `Interpreter::run_async_body` decides
at runtime whether the body needs the async walker or can be run
synchronously. There is no `async` marker.

**Implementation** (`interpreter.cpp:async_step*`): The body is walked
statement-by-statement. Pure-sync statements run via `execute()`. A
`wait` (or any statement that contains a `wait` somewhere — nested in an
`if`, `while`, `try`) routes through `async_step_one`, which dispatches
to the right helper, hooks `.then`/`.fail` into the outer Future, and
defers continuation via the microtask queue.

**Microtask queue**: `Interpreter::microtasks_` is a `deque<function<void()>>`.
It drains:
- Once explicitly between the main script and `uv_run`.
- At every loop iteration boundary via a `uv_prepare_t` handle.
- Continues until empty in each drain pass; tasks queued during a drain
  run in the same pass (matches JS Promise semantics).

The result: `await`-shape code looks synchronous in bnl, runs on a single
thread, and integrates with libuv's other primitives transparently.

## Class system

Crafting-Interpreters-style with explicit `self`:

```bnl
class Stack {
    function init(self) { self.items = []; }
    function push(self, x) { self.items.push(x); }
}
```

`init` is the constructor. `self` is the first parameter by convention —
the parser doesn't special-case it (no implicit `this`).

`extends` for single inheritance; `super.method(self, ...)` to call the
parent's version. `class_type.cpp` resolves methods by walking the
inheritance chain.

## Bangla aliasing

`src/runtime/bn_aliases.cpp/h` maps Bangla tokens to their English
equivalents — both keywords (`চলক` ↔ `var`, `ফাংশন` ↔ `function`,
`শ্রেণী` ↔ `class`, `যদি` ↔ `if`, `যতক্ষণ` ↔ `while`, `থামুন` ↔ `break`,
`চলুন` ↔ `continue`, `ফেরত` ↔ `return`) and module names
(`এসকিউলাইট` ↔ `sqlite`, `পোস্টগ্রেস` ↔ `pg`, etc.).

The mapping is applied **at the lexer level** — Bangla and English keyword
forms produce identical tokens. The rest of the pipeline never sees
language variants. Bilingual files (English keywords + Bangla identifiers,
or vice versa) work because the alias table only handles keywords; arbitrary
Bangla identifiers are valid UTF-8 identifiers and pass through unchanged.

## Diagnostics and errors

Three signal types unwind the AST walker:

- `RuntimeError` (`include/bnl/interpreter.h:36`) — runtime-origin failure.
  Carries the source `Token` for "near line X, column Y, near '...'" hints.
  Caught by `try/catch` (the `e` binds to the message string).
- `ThrowSignal` — user-thrown via `throw <expr>;`. Carries any bnl `Value`.
  Caught by `try/catch` (the `e` binds to the thrown value verbatim).
- `BreakSignal` / `ContinueSignal` — loop control. Caught by the enclosing
  loop walker. Token is preserved so a stray `break` outside a loop
  produces a clean diagnostic.

`ReturnSignal` is the same idiom for function unwinding — the function's
`call` catches it and produces the return Value.

## Interrupt + recursion guards

`max_call_depth` (40 in Debug, 200 in Release) is preemptively checked
every call. The tree-walker burns several KB of C++ stack per bnl frame;
hitting `max_call_depth` produces a RuntimeError instead of a segfault.

Ctrl-C / SIGTERM: `Interpreter::install_signal_handlers()` (called once
from `main.cpp`) wires both to an atomic interrupt flag. The flag is
checked on every call AND on every loop iteration. To wake an idle
libuv loop (no I/O activity), the handler also fires a `uv_async_send`
on `stop_async_`.

## Build system

`CMakePresets.json` drives configuration. Presets per (OS, arch,
debug/release) combination. The Windows dev preset uses dynamic linking
to vcpkg; the `windows-release` preset uses `x64-windows-static` for
single-binary distribution (everything statically linked into bnl.exe).

`vcpkg.json` is the dependency manifest. Manifest mode with a pinned
`builtin-baseline` — reproducible across machines. Current direct deps:
fmt, libuv, llhttp, nlohmann-json, openssl, sqlite3, libpq, mongo-c-driver.
Transitive deps statically linked: utf8proc, lz4, zlib (via libpq's lz4
feature; zlib via PostgreSQL).

The build produces:

| Target | What it is |
|---|---|
| `bnl_core` (static lib) | All language + stdlib + plugin loader. Reusable for embedding. |
| `bnl` (executable) | The CLI. Links `bnl_core` + the `bnl.rc` Windows resource (app icon + version info). |
| Plugin DLLs | Built separately by plugin authors using the C ABI in `include/bnl/plugin.h`. |

`cmake/embed_stdlib.cmake` and `cmake/embed_cacert.cmake` are the two
build-time codegen steps: they read `lib/*.bnl` and `resources/cacert.pem`
respectively and emit headers under `build/<preset>/generated/`. Both are
wired as dependencies of `bnl_core` so changes trigger rebuilds.

## Testing

`tests/CMakeLists.txt` registers every test via `add_bnl_test(<name> <subdir>)`,
implemented in `cmake/run_bnl_test.cmake`. Each test runs the bnl binary
against `tests/<subdir>/<name>.bnl` and diffs the output against
`tests/<subdir>/<name>.expected`. ctest reports any mismatch as a failure.

| Subdir | What's in it |
|---|---|
| `tests/lang/` | Language semantics (arithmetic, classes, closures, loops, …) |
| `tests/stdlib/native_*.bnl` | Native module surface tests |
| `tests/stdlib/lib_*.bnl` | Embedded lib module tests |
| `tests/_fixtures/` | Test fixtures (TLS cert, HTML templates, …) shared across tests |
| `tests/_imports/` | Fixture modules for import-resolution tests |

Current count: 82 tests, all green on Windows x64 Debug.

## Single-threaded by design

bnl runs **one user-code thread, period.** Native modules can do
background work via libuv's threadpool (`uv_queue_work`), but the
completion always lands on the main loop. The interpreter has no
mutex protection because it never sees concurrent access.

This is a deliberate constraint — it makes the value model, environment
chain, and module cache trivially correct, and matches the JS / single-
threaded-event-loop mental model that the async API uses.

## Release distribution

The shipped artifact is `bnl.exe` (Windows) or `bnl` (Linux/macOS).
Statically linked — no DLLs to redistribute, no runtime to install. The
binary plus a `LICENSE.txt` (third-party attribution, mirrored from
`resources/LICENSE.txt`) is the entire release.

`scripts/package.ps1` (Windows) and `scripts/package.sh` (Unix) handle
artifact packaging. They expect the release-preset binary to already
exist; they bundle `bnl.exe` + `LICENSE.txt` + `README.txt` into a zip.

NSIS-based installer for Windows lives at `installer/windows/bnl_installer.nsi`.
