# bnl guide

A practical walkthrough: install, run, organize a project, use async, classes,
imports.

## Install

You need a C++20 toolchain, CMake ≥ 3.21, and vcpkg.

### Windows

```powershell
# Once per shell — loads MSVC env and pins VCPKG_ROOT.
. .\dev.ps1

cmake --preset windows
cmake --build --preset windows
```

This produces `build/windows/bin/bnl.exe`.

### Linux / macOS

```sh
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset linux         # or macos
cmake --build --preset linux
```

### Tests

```sh
ctest --test-dir build/windows --output-on-failure
```

## Hello, world

```bnl
// hello.bnl
print("hi from bnl");
লিখুন("লিখুন works too");
```

```sh
bnl hello.bnl
```

CLI flags:

- `-e '<code>'` — run a code string instead of a file
- `-v`, `--version` — print version and exit
- `-h`, `--help` — print help and exit

```sh
bnl -e 'print(2 + 2)'
```

Compiler-debugging flags (undocumented in `--help`; for working on the interpreter itself):

- `--ast` — print the parsed AST and exit
- `--tokens` — print the lexer's token stream and exit

## Project layout

A "project" is any directory containing a `bnl.json` file (the *project root*).
Projects are detected by walking up from the script being executed.

```
my-app/
├── bnl.json              ← project root
├── main.bnl              ← entry script
├── src/
│   └── helpers.bnl
└── deps/                 ← all third-party packages live here
    └── utils/
        ├── bnl.json      ← {"main": "src/index.bnl"}
        └── src/index.bnl
```

The presence of `bnl.json` at the root is the **only** signal that turns the
directory into a project. Inside a project, dep resolution is local-only; the
global `~/.bnl/deps/` is *not* searched. This keeps builds reproducible — a
project's behavior is fully determined by what's inside it.

`bnl.json` at the root currently only acts as that marker. Optional fields
(`name`, `version`, etc.) are tolerated but not used by the runtime. A separate
package manager is being built that will use them.

## Imports and modules

```bnl
import "sys" as sys;                  // (1) built-in native module
import "utils" as u;                  // (2) dep, walked-up via deps/
import "./utils.bnl" as utils;        // (3) relative bnl source
import "C:/abs/path/lib.bnl" as lib;  // (4) absolute path
```

The resolver tries forms in this order:

1. **Direct path** if the string starts with `./`, `../`, `/`, `\`, or a drive
   letter (`C:`):
   - Ends in `.dll` / `.so` / `.dylib` → loaded as an FFI plugin.
   - Otherwise → loaded as bnl source.
2. **Bare name** — fall through to the registry chain:
   1. **Built-in** native modules (`sys`, `io`, `timers`).
   2. **Walk-up `deps/<name>/`** — from the importing file's directory upward,
      look for `<ancestor>/deps/<name>/`. The first match wins. Entry point
      resolution inside the dep dir tries:
      - `bnl.json` `"native"` field → FFI plugin
      - `bnl.json` `"main"` field → bnl source
      - `index.bnl` (Node-style fallback)
      - `<dep_name>.bnl` (single-file dep)
   3. **Global `~/.bnl/deps/<name>/`** — only when *outside* a project. Same
      entry-point resolution as walk-up.
   4. Error.

Each module is **cached by canonical path**. Re-importing the same path returns
the same module value; the source is evaluated exactly once per Interpreter.

## The async model

bnl runs each script synchronously, then drains the libuv event loop. Anything
scheduled before exit fires before the process terminates.

### Timers

```bnl
import "timers" as timers;

timers.set(100, function () {
    print("hello, 100ms later");
});

var cancel = timers.interval(50, function () {
    print("tick");
});
timers.set(180, cancel);   // cancel after ~3 ticks
```

`timers.set(ms, fn)` and `timers.interval(ms, fn)` both return a 0-arg `cancel`
callable. One-shot timers auto-release after firing.

### Async I/O

The `io` module exposes both sync and async forms:

```bnl
import "io" as io;

// Sync:
var text = io.read_file("config.txt");
io.write_file("out.txt", "hi");

// Async — Node-style (err, data) callback:
io.read_file_async("config.txt", function (err, data) {
    if (err != null) { print("read failed:", err); return; }
    print("got", data.length, "bytes");
});
```

Streaming forms (`io.open_read`, `io.open_write`) return a stream module with
`read(size, cb)` / `write(data, cb)` and `close()` for backpressure-friendly
chunk processing.

### Error handling

```bnl
try {
    var contents = io.read_file("missing.txt");
    print(contents);
} catch (e) {
    print("read failed:", e);
}
```

`throw <expr>;` raises any value; the nearest enclosing `catch (var)` binds it.
For runtime-origin errors, `e` is the error message string. See
[syntax — try/catch/throw](./syntax.md#try--catch--throw) for the full story.

A runtime error inside an *async* callback (timer, io) is logged to stderr and
flips the interpreter into a failure exit — async callbacks aren't enclosed by
the script-level `try` because they fire after the script returns. Wrap the
callback body in its own `try` if you need to recover.

## Classes

Crafting-Interpreters-flavored, with explicit `self`:

```bnl
class Stack {
    function init(self) { self.items = []; }
    function push(self, x) { self.items.push(x); }
    function pop (self)    { return self.items.pop(); }
    function size(self)    { return self.items.length; }
}

var s = Stack();
s.push("a");
s.push("b");
print(s.pop());     // "b"
print(s.size());    // 1
```

Inheritance with `extends` and `super`. See [syntax.md](./syntax.md#single-inheritance).

## Lists and maps

```bnl
var xs = [1, 2, 3];
xs.push(4);
print(xs.length);          // 4
print(xs[0]);              // 1
xs[0] = 99;

var m = {name: "alice", age: 30};
print(m.name);             // "alice"
print(m["age"]);           // 30
print(m.has("age"));       // true
print(m.size);             // 2
print(m.keys());           // ["name", "age"]
```

A bare `{` in *statement* position is a block, not a map literal. To use a map
in statement position, assign or wrap it: `var _ = {a: 1};`.

## Bangla everywhere

Identifiers, function names, field names, parameter names, and keywords can all
be in Bangla. Bilingual code in one file is fully supported:

```bnl
শ্রেণী Counter {
    ফাংশন init(self, প্রারম্ভ) { self.value = প্রারম্ভ; }
    function inc(self)         { self.value = self.value + 1; }
    ফাংশন ধরা(self)            { ফেরত self.value; }
}

চলক c = Counter(10);
c.inc();
লিখুন(c.ধরা());               // 11
```

## Where to go next

- Want to publish a pure-bnl package? → [Dep development](./deps-dev.md)
- Need the precise grammar? → [Syntax reference](./syntax.md)
