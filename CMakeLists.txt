cmake_minimum_required(VERSION 3.21)

project(bnl
    VERSION 0.1.0
    DESCRIPTION "Bangla programming language (Bnlang)"
    HOMEPAGE_URL "https://bnlang.dev"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

option(BNL_ENABLE_FFI "Build with runtime FFI plugin support (forces bnl_core to be shared)" ON)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor)
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
find_package(unofficial-libmariadb CONFIG REQUIRED) # stdlib/mysql.cpp
find_package(mongoc             CONFIG REQUIRED)   # stdlib/mongo.cpp

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

add_subdirectory(src)

add_subdirectory(examples/embed)

if(BNL_ENABLE_FFI)
    add_subdirectory(examples/plugin_native)
endif()

enable_testing()
add_subdirectory(tests)
