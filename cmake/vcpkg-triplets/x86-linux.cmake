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

# Skip the *debug* variant of every vcpkg port. The port's debug build of
# SQLite 3.53 trips an i386-only assertion (`vdbeMemRenderNum`) even when
# we add -DNDEBUG to its flags, because the port's own CMakeLists resets
# CMAKE_C_FLAGS_DEBUG. Forcing release-only sidesteps the whole problem:
# bnl-in-Debug just links the assertions-stripped Release sqlite3.
#
# Trade-off: you can't step a debugger into a third-party dep. We never
# do that anyway — Linux has no separate debug CRT and bnl's own .o files
# still build in full Debug.
set(VCPKG_BUILD_TYPE release)
