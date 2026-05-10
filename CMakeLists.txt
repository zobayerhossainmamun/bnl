cmake_minimum_required(VERSION 3.21)

project(bnl
    VERSION 0.1.0
    DESCRIPTION "Bangla programming language interpreter"
    HOMEPAGE_URL ""
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

# Build with runtime FFI plugin support?
#   ON  (default) — bnl_core is a SHARED library so plugins (.dll/.so/.dylib)
#                   reference the same loaded runtime that bnl.exe does.
#                   `import "name"` of a native plugin loads it at runtime.
#                   Distribution: bnl.exe + bnl_core.dll.
#   OFF           — bnl_core is STATIC and links into bnl.exe directly.
#                   Combined with a static vcpkg triplet, produces a single
#                   self-contained binary. `import "name"` of a native plugin
#                   throws a clear runtime error directing the user to rebuild.
option(BNL_ENABLE_FFI "Build with runtime FFI plugin support (forces bnl_core to be shared)" ON)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion)
endif()

# Core ships only the language + the three intrinsic native modules
# (sys, io, timers). Everything else (crypto, net, tls, http, json, sqlite,
# etc.) is delivered by the separate package manager as plugins or pure-bnl
# packages, dropped into deps/<name>/ at runtime.
find_package(fmt           CONFIG REQUIRED)
find_package(libuv         CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)        # used by module loader to read bnl.json

add_subdirectory(src)

# Embedding examples (host C++ apps that link bnl_core).
add_subdirectory(examples/embed)

# FFI plugin example — only meaningful when bnl_core is a shared library.
# Added before the tests/ subdir so the integration test can stage the
# built plugin into a fixture directory.
if(BNL_ENABLE_FFI)
    add_subdirectory(examples/plugin_native)
endif()

enable_testing()
add_subdirectory(tests)
