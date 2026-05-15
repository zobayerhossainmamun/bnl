# Bnlang (bnl)

A bilingual programming language and runtime — write the same program in **Bangla** or **English**, side by side. Implemented as a native C++ tree-walking interpreter with a built-in stdlib (I/O, timers, networking, HTTP, TLS, JSON, regex, crypto, exec, DNS, SQLite, Postgres, MongoDB).

- **Self-contained binary**: Release builds statically link everything into a single `bnl` executable — users don't install a runtime, don't manage DLLs, just download and run.

Status: **1.0.0**. Source is currently private; binary distribution is free.

---

## Hello, world — both ways

```bnl
// English
fun add(a, b) { return a + b; }
print(add(2, 3));
```

```bnl
// Bangla — identical semantics, different keywords
ফাংশন যোগ(ক, খ) { ফেরত ক + খ; }
লিখুন(যোগ(2, 3));
```

You can mix both within a project. The lexer recognizes either keyword set; the parser produces the same AST.

---

## Features at a glance

- **Bilingual lexer**: every keyword has an English and a Bangla spelling.
- **Pythonic feel, C-style syntax**: semicolons, braces, lexical scoping, closures, first-class functions, classes with single inheritance, `try` / `catch` / `throw`.
- **Module loader**: `import "name" as alias;` resolves built-in modules, `deps/<name>/` packages (with `bnl.json` manifests), relative paths, and native plugins.
- **Async event loop**: powered by libuv. `setTimeout` / `setInterval`, async I/O, TCP / TLS / HTTP servers and clients all share one loop.
- **Built-in stdlib**: `sys`, `io`, `timers`, `regex`, `crypto`, `net`, `tls`, `http`, `json`, `exec`, `dns`, `sqlite`.
- **Embedded helper libraries** (`lib/*.bnl`, baked into the binary at build time): `url`, `web` (HTTP server framework), `request` (HTTP client), `dns`, `exec`, `sqlite`.
- **FFI plugins**: load a native shared library via `import` and call exported functions as regular bnl values.
- **C++ embedding API**: `bnl::Interpreter` in `include/bnl/interpreter.h`.

---

## Quick start

### Run a binary

```sh
bnl hello.bnl              # run a script
bnl                        # REPL
bnl -e 'print(1 + 2);'     # one-liner
bnl --version
```

### Build from source

Requirements:

- CMake ≥ 3.21
- Ninja
- A C++20 compiler (MSVC 19.3+, Clang 14+, GCC 11+)
- [vcpkg](https://github.com/microsoft/vcpkg) — set `VCPKG_ROOT` to its checkout

Dependencies (resolved by vcpkg from `vcpkg.json`): `fmt`, `libuv`, `llhttp`, `nlohmann-json`, `openssl`, `sqlite3`.

```sh
# Dev (Debug, dynamic deps on Windows)
cmake --preset windows         # or: linux / macos
cmake --build --preset windows

# Ship (Release, fully static deps; truly self-contained binary)
cmake --preset windows-release         # or: windows-x86-release
cmake --preset linux-release           # or: linux-x86-release
cmake --preset macos-release
cmake --build --preset windows-release
```

Output goes to `build/<preset>/bin/bnl{.exe}` — a single self-contained binary.

### Run the test suite

```sh
ctest --test-dir build/windows-release --output-on-failure -C Release
```

74 tests cover lexer/parser semantics, every stdlib module, and the embedded `lib/*.bnl` helpers.

---

## Project layout

```
include/bnl/         Internal C++ headers (used inside bnl_core)
src/                 Interpreter sources
  frontend/          Lexer, parser, AST printer
  runtime/           Values, environment, evaluator, classes, module loader
  stdlib/            Built-in native modules (sys, io, http, tls, sqlite, …)
lib/                 Pure-bnl stdlib helpers (baked into the binary)
tests/               ctest-driven .bnl tests
cmake/               Helper scripts (embed_stdlib.cmake, run_bnl_test.cmake)
installer/windows/   NSIS installer (x86 + x64)
resources/           Icon, version-info .rc, README/LICENSE for distribution
CMakePresets.json    Build presets (dev + ship, x64 + x86)
vcpkg.json           Pinned third-party dependencies
```

---

## Platforms

| Platform | Dev preset | Ship preset | End-user requirements |
|---|---|---|---|
| Windows x64 | `windows` | `windows-release` | None — static CRT + static deps |
| Windows x86 | `windows-x86` | `windows-x86-release` | None |
| Linux x64 | `linux` | `linux-release` | Modern glibc — libstdc++/libgcc static |
| Linux x86 | `linux-x86` | `linux-x86-release` | Modern glibc — libstdc++/libgcc static |
| macOS arm64 | `macos` | `macos-release` | macOS 12+ (system libs ship with OS) |

---

## License

Binaries are free to use for personal, educational, and commercial projects. Source remains private property of the authors. Bundled third-party components (fmt, libuv, llhttp, nlohmann-json, OpenSSL, SQLite) ship under their respective licenses — see `resources/LICENSE.txt`.

---

Website: <https://bnlang.dev>
