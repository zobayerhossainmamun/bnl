cmake_minimum_required(VERSION 3.21)

project(bnl
    VERSION 2.0.0
    DESCRIPTION "Bangla programming language (Bnlang)"
    HOMEPAGE_URL "https://bnlang.dev"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

if(MSVC)
    # /EHsc enables synchronous C++ exception handling — without it, MSVC
    # warns C4530 and catch blocks silently fail to unwind, so any thrown
    # exception (e.g. from nlohmann::json::parse on malformed input) escapes
    # main() and triggers the CRT abort popup instead of being handled.
    add_compile_options(/EHsc /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion)
endif()

find_package(fmt                CONFIG REQUIRED)
find_package(libuv              CONFIG REQUIRED)
find_package(llhttp             CONFIG REQUIRED)   # stdlib/http.cpp
find_package(nlohmann_json      CONFIG REQUIRED)   # used by module loader to read bnl.json
find_package(OpenSSL            REQUIRED)          # stdlib/crypto.cpp + tls.cpp
find_package(unofficial-sqlite3 CONFIG REQUIRED)   # stdlib/sqlite.cpp
find_package(PostgreSQL         REQUIRED)          # stdlib/pg.cpp
find_package(mongoc             CONFIG REQUIRED)   # stdlib/mongo.cpp
find_package(ZLIB               REQUIRED)          # stdlib/zlib.cpp (already a transitive dep of libpq)
find_package(unofficial-libffi  CONFIG REQUIRED)   # stdlib/ffi.cpp — call arbitrary C ABI

set(BNL_STDLIB_EMBEDDED_HEADER ${CMAKE_BINARY_DIR}/generated/stdlib_embedded.h)
file(GLOB BNL_LIB_FILES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/lib/*.bnl)
add_custom_command(
    OUTPUT  ${BNL_STDLIB_EMBEDDED_HEADER}
    COMMAND ${CMAKE_COMMAND}
        -DLIB_DIR=${CMAKE_SOURCE_DIR}/lib
        -DOUT_FILE=${BNL_STDLIB_EMBEDDED_HEADER}
        -P ${CMAKE_SOURCE_DIR}/cmake/embed_stdlib.cmake
    DEPENDS ${BNL_LIB_FILES} ${CMAKE_SOURCE_DIR}/cmake/embed_stdlib.cmake
    COMMENT "Baking lib/*.bnl into stdlib_embedded.h"
    VERBATIM
)
add_custom_target(bnl_stdlib_embedded ALL DEPENDS ${BNL_STDLIB_EMBEDDED_HEADER})

# Mozilla CA root bundle, embedded into bnl_core so HTTPS clients (request,
# web client, tls, TLS-enabled DB drivers) have a trust store on platforms
# where OpenSSL's default verify paths don't resolve anywhere useful — i.e.
# Windows. Refresh by re-downloading resources/cacert.pem from curl.se and
# rebuilding.
set(BNL_CACERT_HEADER ${CMAKE_BINARY_DIR}/generated/bnl/cacert_pem.inc)
set(BNL_CACERT_PEM    ${CMAKE_SOURCE_DIR}/resources/cacert.pem)
add_custom_command(
    OUTPUT  ${BNL_CACERT_HEADER}
    COMMAND ${CMAKE_COMMAND}
        -DINPUT=${BNL_CACERT_PEM}
        -DOUTPUT=${BNL_CACERT_HEADER}
        -P ${CMAKE_SOURCE_DIR}/cmake/embed_cacert.cmake
    DEPENDS ${BNL_CACERT_PEM} ${CMAKE_SOURCE_DIR}/cmake/embed_cacert.cmake
    COMMENT "Baking resources/cacert.pem into cacert_pem.inc"
    VERBATIM
)
add_custom_target(bnl_cacert_embedded ALL DEPENDS ${BNL_CACERT_HEADER})

add_subdirectory(src)

# Sample C plugin — gated so a release distribution can skip it.
option(BNL_BUILD_SAMPLE_PLUGIN "Build the sample C plugin under examples/" ON)
if(BNL_BUILD_SAMPLE_PLUGIN)
    add_subdirectory(examples/plugin_c)
endif()

enable_testing()
add_subdirectory(tests)
