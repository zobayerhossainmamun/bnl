# Building Bnlang from source

Bnlang uses CMake (Ninja generator) and vcpkg in manifest mode. Build configuration is driven by [`CMakePresets.json`](CMakePresets.json); dependencies are pinned in [`vcpkg.json`](vcpkg.json).

This document is the detailed reference. For a 30-second overview, see [`README.md`](README.md).

---

## Prerequisites

You need:

| Tool | Min version | Why |
|---|---|---|
| CMake | 3.21 | preset support |
| Ninja | any | the generator the presets target |
| C++20 compiler | MSVC 19.3 / Clang 14 / GCC 11 | language level |
| Git | any | vcpkg checkout and manifest baseline |
| vcpkg | current | dependency resolution from `vcpkg.json` |
| Perl | any | needed by vcpkg to build OpenSSL |

The interpreter's own runtime dependencies (fmt, libuv, llhttp, nlohmann-json, OpenSSL, SQLite) are installed by vcpkg per-build — you do **not** install them from your distro.

### Windows

Install **Visual Studio 2022** (or 2026 — VS 18) with the "Desktop development with C++" workload. That brings MSVC, the Windows SDK, CMake, and Ninja in one shot.

- Configure / build from a **Developer Command Prompt** so `cl.exe` is on `PATH`. For x86 builds use the "x86 Native Tools" variant.
- Alternatively call `vcvarsall.bat x64` (or `x86`) in your own shell before running CMake.

### Ubuntu 24.04

```sh
sudo apt update && sudo apt install -y build-essential cmake ninja-build git curl zip unzip tar pkg-config perl
```

For x86 (32-bit) cross builds, additionally:

```sh
sudo apt install -y gcc-multilib g++-multilib
```

Ubuntu 24.04 defaults are new enough: g++ 13, cmake 3.28.

### Ubuntu 22.04 / older

Same packages, but the default `g++` may be 9.4 which doesn't support enough C++20. Install a newer toolchain:

```sh
sudo apt install -y g++-12
export CXX=g++-12 CC=gcc-12
```

### Fedora / RHEL

```sh
sudo dnf install -y gcc gcc-c++ cmake ninja-build git curl zip unzip tar pkgconfig perl
# x86: sudo dnf install glibc-devel.i686 libstdc++-devel.i686
```

### Arch

```sh
sudo pacman -S --needed base-devel cmake ninja git curl zip unzip tar perl
# x86: enable [multilib] in /etc/pacman.conf, then: sudo pacman -S lib32-glibc lib32-gcc-libs
```

### macOS

```sh
xcode-select --install                                 # command-line tools (clang, headers)
brew install cmake ninja git                           # if not already present
```

Both **arm64 (Apple Silicon)** and **x64 (Intel)** are supported via separate presets. Cross-compiling from arm64 to x64 (or vice versa) works with the standard Apple toolchain — vcpkg will build all deps for the target triplet.

---

## vcpkg

Clone once, anywhere — point `VCPKG_ROOT` at it.

### Windows (PowerShell)

```powershell
git clone https://github.com/microsoft/vcpkg C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
[Environment]::SetEnvironmentVariable('VCPKG_ROOT', 'C:\vcpkg', 'User')
# open a new shell so the variable is visible
```

### Linux / macOS

```sh
git clone https://github.com/microsoft/vcpkg ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh
echo 'export VCPKG_ROOT=$HOME/vcpkg' >> ~/.bashrc   # or ~/.zshrc on macOS
source ~/.bashrc
```

vcpkg uses manifest mode here: it reads `vcpkg.json` from the project root and installs the listed packages into `build/<preset>/vcpkg_installed/`. There's no `vcpkg install` step — CMake runs it for you.

---

## Build presets

`CMakePresets.json` defines two flavors (`dev` — Debug, dynamic deps; `release` — Release, static deps) for each supported platform/arch.

| Preset | Build type | Triplet | Notes |
|---|---|---|---|
| `windows` | Debug | `x64-windows` | dev — dynamic CRT |
| `windows-x86` | Debug | `x86-windows` | dev — dynamic CRT |
| `linux` | Debug | `x64-linux` | dev |
| `linux-x86` | Debug | `x86-linux` | dev — adds `-m32` |
| `macos` | Debug | `arm64-osx` | dev — Apple Silicon |
| `macos-x64` | Debug | `x64-osx` | dev — Intel Mac |
| `windows-release` | Release | `x64-windows-static` | static CRT + static deps |
| `windows-x86-release` | Release | `x86-windows-static` | static CRT + static deps |
| `linux-release` | Release | `x64-linux` | + static libstdc++/libgcc |
| `linux-x86-release` | Release | `x86-linux` | + `-m32` + static libstdc++/libgcc |
| `macos-release` | Release | `arm64-osx` | Apple Silicon + dead-code stripping |
| `macos-x64-release` | Release | `x64-osx` | Intel Mac + dead-code stripping |

Release presets produce self-contained binaries — end users don't need to install a runtime (Windows: no VC++ Redistributable; Linux: works against the build host's glibc and older).

---

## Per-OS recipes

Each block below is a complete copy-paste sequence: configure → build → test → run. Each `cmake --preset` writes to its own `build/<preset>/` directory, so debug and release builds (and per-arch builds) don't collide.

### Output location

Everything lands in `build/<preset>/bin/`. File names per platform:

| Platform | CLI | Runtime (FFI=ON) | Plugin sample |
|---|---|---|---|
| Windows | `bnl.exe` | `bnl_core.dll` | `mathx.dll` |
| Linux | `bnl` | `libbnl_core.so` | `libmathx.so` |
| macOS | `bnl` | `libbnl_core.dylib` | `libmathx.dylib` |

### Windows x64

Run from a **x64 Developer Command Prompt** (or `call vcvarsall.bat x64` first):

**Dev (Debug)**

```bat
cmake --preset windows
cmake --build --preset windows
ctest --test-dir build\windows --output-on-failure -C Debug
build\windows\bin\bnl.exe tests\lang\arithmetic.bnl
```

**Release (ship)**

```bat
cmake --preset windows-release
cmake --build --preset windows-release
ctest --test-dir build\windows-release --output-on-failure -C Release
build\windows-release\bin\bnl.exe tests\lang\arithmetic.bnl
```

### Windows x86

Run from a **x86 Native Tools Command Prompt** (or `call vcvarsall.bat x86` first):

**Dev (Debug)**

```bat
cmake --preset windows-x86
cmake --build --preset windows-x86
ctest --test-dir build\windows-x86 --output-on-failure -C Debug
build\windows-x86\bin\bnl.exe tests\lang\arithmetic.bnl
```

**Release (ship)**

```bat
cmake --preset windows-x86-release
cmake --build --preset windows-x86-release
ctest --test-dir build\windows-x86-release --output-on-failure -C Release
build\windows-x86-release\bin\bnl.exe tests\lang\arithmetic.bnl
```

### Linux x64

**Dev (Debug)**

```sh
cmake --preset linux
cmake --build --preset linux
ctest --test-dir build/linux --output-on-failure
./build/linux/bin/bnl tests/lang/arithmetic.bnl
```

**Release (ship)**

```sh
cmake --preset linux-release
cmake --build --preset linux-release
ctest --test-dir build/linux-release --output-on-failure
./build/linux-release/bin/bnl tests/lang/arithmetic.bnl
```

### Linux x86

Needs `gcc-multilib g++-multilib` installed (see Ubuntu section above).

**Dev (Debug)**

```sh
cmake --preset linux-x86
cmake --build --preset linux-x86
ctest --test-dir build/linux-x86 --output-on-failure
./build/linux-x86/bin/bnl tests/lang/arithmetic.bnl
```

**Release (ship)**

```sh
cmake --preset linux-x86-release
cmake --build --preset linux-x86-release
ctest --test-dir build/linux-x86-release --output-on-failure
./build/linux-x86-release/bin/bnl tests/lang/arithmetic.bnl
```

### macOS arm64 (Apple Silicon)

**Dev (Debug)**

```sh
cmake --preset macos
cmake --build --preset macos
ctest --test-dir build/macos --output-on-failure
./build/macos/bin/bnl tests/lang/arithmetic.bnl
```

**Release (ship)**

```sh
cmake --preset macos-release
cmake --build --preset macos-release
ctest --test-dir build/macos-release --output-on-failure
./build/macos-release/bin/bnl tests/lang/arithmetic.bnl
```

### macOS x64 (Intel)

Builds cleanly on Apple Silicon as a cross-compile, and natively on Intel Macs. Note: on Apple Silicon you can build x64 binaries but won't be able to *run* them without Rosetta — `ctest` will fail to execute the tests in that case.

**Dev (Debug)**

```sh
cmake --preset macos-x64
cmake --build --preset macos-x64
ctest --test-dir build/macos-x64 --output-on-failure       # native Intel only
./build/macos-x64/bin/bnl tests/lang/arithmetic.bnl
```

**Release (ship)**

```sh
cmake --preset macos-x64-release
cmake --build --preset macos-x64-release
ctest --test-dir build/macos-x64-release --output-on-failure   # native Intel only
./build/macos-x64-release/bin/bnl tests/lang/arithmetic.bnl
```

### Clean build (any preset)

Just delete the build directory and reconfigure. vcpkg's binary cache (`~/.cache/vcpkg/archives/` on Linux/macOS, `%LOCALAPPDATA%\vcpkg\archives\` on Windows) survives, so subsequent configures don't re-compile OpenSSL.

```sh
rm -rf build/linux-release
cmake --preset linux-release
cmake --build --preset linux-release
```

On Windows: `rmdir /s /q build\windows-release` (cmd) or `Remove-Item -Recurse -Force build\windows-release` (PowerShell).

### Running a single test

`ctest -R <regex>` filters tests by name:

```sh
ctest --test-dir build/linux-release -R native_http --output-on-failure
ctest --test-dir build/linux-release -R "lib_.*" --output-on-failure
```

Expect **39 tests total** with `BNL_ENABLE_FFI=ON` (the default); **37** with `BNL_ENABLE_FFI=OFF` (the two FFI integration tests are skipped automatically).

---

## Build options

Pass these to the **configure** step with `-D`:

### `BNL_ENABLE_FFI` (default `ON`)

Controls whether `bnl_core` is built as a shared or static library.

| Value | Output shape | When to use |
|---|---|---|
| `ON` (default) | `bnl` + `bnl_core.{dll,so,dylib}` ship side by side; native plugins can be loaded at runtime via `import "name"` | normal builds, plugin development, production with a package ecosystem |
| `OFF` | `bnl` only — `bnl_core` links statically into the CLI; native plugin `import` raises a clear runtime error | single-binary distributions; minimal footprint; reproducible-build sensitive contexts |

Add it to any preset's configure step:

```sh
cmake --preset linux-release -DBNL_ENABLE_FFI=OFF
cmake --build --preset linux-release
# build/linux-release/bin/bnl is now a single static executable, no .so files
```

---

## Building the Windows installer

The NSIS script in `installer/windows/bnl_installer.nsi` packages either the x64 or x86 release build.

Prereq: install [NSIS 3](https://nsis.sourceforge.io/Download) (default location `C:\Program Files (x86)\NSIS`).

```powershell
# Build the binaries first
cmake --preset windows-release
cmake --build --preset windows-release

# Then compile the installer
& "C:\Program Files (x86)\NSIS\makensis.exe" /DARCH=x64 installer\windows\bnl_installer.nsi
# Output: installer\windows\bnlang-windows-x64-v0.1.0-installer.exe
```

For the x86 installer, substitute the `windows-x86-release` preset and `/DARCH=x86`. The installer:

- copies `bnl.exe` (and `bnl_core.dll` when FFI is ON) to `C:\Program Files\Bnlang`
- adds the install dir to system `PATH` (idempotent — safe to reinstall)
- registers Apps & Features entries for clean uninstall

It does **not** require admin elevation beyond the standard installer prompt, doesn't modify PowerShell execution policy, and doesn't run any post-install scripts.

---

## Embedding / extending — see also

- `examples/embed/host.cpp` + `examples/embed/CMakeLists.txt` — minimal C++ host linking `bnl_core`.
- `examples/plugin_native/mathx.cpp` + `examples/plugin_native/CMakeLists.txt` — minimal native plugin that `import` can load.

Both are built by the standard presets and exercised by the test suite.

---

## Troubleshooting

### "Could not find compiler" / cl.exe not on PATH (Windows)

Run CMake from a Developer Command Prompt, or `call vcvarsall.bat x64` (or `x86`) before configuring. Each build dir is tied to one architecture — don't reuse `build/windows-release` across x64 and x86 sessions.

### vcpkg fails to download a port

Network issue or a vcpkg version that predates the manifest's pinned port versions. Update your vcpkg checkout:

```sh
git -C $VCPKG_ROOT pull
$VCPKG_ROOT/bootstrap-vcpkg.sh        # or .bat on Windows
```

### "GLIBCXX_X.Y not found" when running the binary on an old Linux box

You built on a newer distro than the deployment target. Either:

- build on the **oldest** Linux distro you need to support (the release preset already statically links `libstdc++`/`libgcc`, so the only remaining floor is glibc itself), or
- use a manylinux-style build container.

### "x86_64-linux-gnu/crti.o: not found" when configuring `linux-x86`

You're missing 32-bit dev libraries:

```sh
sudo apt install -y gcc-multilib g++-multilib
```

### A test fails with "Access violation" only on a release build

If the failure is in `proj_full` or `direct_plugin`, the staged sample plugin in `tests/_fixtures/proj_full/deps/mathx-plugin/` may be stale from a different build configuration (Debug vs Release). Both configs stage to the same path. Either re-run `cmake --build` for the failing preset (it re-stages) or delete the stale `mathx.{dll,so,dylib}` under that fixture directory.

### Build cache trash / weird stale errors

Nuke the build directory and reconfigure. vcpkg's binary cache survives, so this is cheap:

```sh
rm -rf build/<preset>
cmake --preset <preset>
cmake --build --preset <preset>
```
