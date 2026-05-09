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

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion)
endif()

find_package(fmt            CONFIG REQUIRED)
find_package(libuv          CONFIG REQUIRED)
find_package(OpenSSL        REQUIRED)
find_package(ZLIB           REQUIRED)
find_package(llhttp         CONFIG REQUIRED)
find_package(nlohmann_json  CONFIG REQUIRED)

# ---- generate stdlib_embedded.h from lib/*.bnl ----
set(STDLIB_HEADER ${CMAKE_BINARY_DIR}/generated/stdlib_embedded.h)
file(GLOB STDLIB_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/lib/*.bnl)

add_custom_command(
    OUTPUT  ${STDLIB_HEADER}
    COMMAND ${CMAKE_COMMAND}
        -DOUT=${STDLIB_HEADER}
        -DLIB=${CMAKE_SOURCE_DIR}/lib
        -P ${CMAKE_SOURCE_DIR}/cmake/embed_stdlib.cmake
    DEPENDS ${STDLIB_SOURCES} ${CMAKE_SOURCE_DIR}/cmake/embed_stdlib.cmake
    COMMENT "Embedding bnl stdlib (${STDLIB_HEADER})"
    VERBATIM
)
add_custom_target(stdlib_embedded DEPENDS ${STDLIB_HEADER})

add_subdirectory(src)

enable_testing()
add_subdirectory(tests)

# Embedding examples (host C++ apps that link bnl_core).
add_subdirectory(examples/embed)

# FFI plugin example (built as a shared library, loaded at runtime by bnl).
add_subdirectory(examples/plugin_native)
