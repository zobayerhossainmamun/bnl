# Sample bnl plugin (pure C). Built as a shared library that does not link
# against bnl_core — every interaction goes through the bnl_api function-
# pointer table handed in at runtime.
#
# This is also what plugin authors do externally: copy include/bnl/plugin.h,
# compile a shared library against it, drop the .dll/.so/.dylib next to a
# bnl script.

# ---------- MSVC link-speed fixes (scoped to this subdir) -----------------
#
# A /VERBOSE:TIMES run revealed that mathx's link was almost entirely
# overhead, none of it from the plugin itself:
#
#   1. CMake's MSVC toolchain auto-appends 10 Windows system libs to every
#      link command (kernel32, user32, gdi32, winspool, shell32, ole32,
#      oleaut32, uuid, comdlg32, advapi32). link.exe scans every export
#      table even though mathx uses zero of them.
#   2. vcpkg's toolchain spawns PowerShell after every link to run
#      applocal.ps1 (copies DLL deps next to the output). mathx has zero
#      vcpkg deps, so the 300-500 ms PowerShell cold-start is pure waste.
#
# Both variables are set in this subdir scope only — they don't leak back
# up and affect bnl.exe or the rest of the build.
if(MSVC)
    set(CMAKE_C_STANDARD_LIBRARIES "")
    set(VCPKG_APPLOCAL_DEPS OFF)
endif()

add_library(mathx SHARED mathx.c)
set_target_properties(mathx PROPERTIES
    C_STANDARD                   99
    PREFIX                       ""
    OUTPUT_NAME                  "mathx"
    LIBRARY_OUTPUT_DIRECTORY     ${CMAKE_BINARY_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY     ${CMAKE_BINARY_DIR}/bin
    POSITION_INDEPENDENT_CODE    ON
)

# The plugin only needs the header. No link against bnl.
target_include_directories(mathx PRIVATE ${PROJECT_SOURCE_DIR}/include)

# Link libm on POSIX for sqrt().
if(UNIX)
    target_link_libraries(mathx PRIVATE m)
endif()

# Strip per-target Debug ceremony — separate PDB, incremental-link metadata,
# manifest XML — all overkill for a 6 KB single-source plugin.
if(MSVC)
    target_compile_options(mathx PRIVATE
        $<$<CONFIG:Debug>:/Z7>     # debug info in the .obj, no separate .pdb
    )
    target_link_options(mathx PRIVATE
        /INCREMENTAL:NO            # no incremental-link metadata
        /DEBUG:NONE                # no .pdb at link time
        /MANIFEST:NO               # no application manifest
    )
endif()
