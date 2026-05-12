# Overlay triplet — see CMakePresets.json's `linux-x86-release` preset.
#
# The default community `x86-linux` triplet (in $VCPKG_ROOT/triplets/community/)
# is bare-bones: just sets the target arch + system name. Some Linux toolchains
# / vcpkg versions don't end up passing `-DNDEBUG` to the release builds of
# certain ports as a result, which leaves runtime asserts enabled in vcpkg-built
# libraries. SQLite 3.53 in particular has a GCC-7..14 / i386-only code path
# guarded by an overly strict `assert(...)` that fires under normal use, so the
# missing NDEBUG turns into a hard crash in lib_sqlite / native_sqlite tests.
#
# Forcing the standard release flags here makes the build deterministic and
# silences `<assert.h>` asserts in shipped libraries.
set(VCPKG_TARGET_ARCHITECTURE x86)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)

set(VCPKG_C_FLAGS_RELEASE   "-O3 -DNDEBUG")
set(VCPKG_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

# Apply NDEBUG to the *debug* variants of vcpkg ports too. We never step
# through third-party deps with a debugger anyway, and the buggy SQLite
# i386 assertion fires in vcpkg's debug build of sqlite3 just as it does
# in release. Without this, the linux-x86 (Debug) preset crashes in
# lib_sqlite / native_sqlite tests because it links vcpkg's debug sqlite3.
set(VCPKG_C_FLAGS_DEBUG   "-DNDEBUG")
set(VCPKG_CXX_FLAGS_DEBUG "-DNDEBUG")
