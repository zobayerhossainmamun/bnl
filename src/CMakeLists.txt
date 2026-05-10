# bnl_core: the runtime library that hosts, the bnl CLI, and FFI plugins all
# link against. Built SHARED when BNL_ENABLE_FFI=ON (the default) — otherwise
# each FFI plugin DLL would end up with its own static copy of Environment /
# Module / Value, and types crossing the plugin <-> exe boundary land at
# different vtables → crash. With FFI off, bnl_core is STATIC and links right
# into bnl.exe, yielding a single-binary distribution.
#
# Public headers live in <repo>/include/bnl/. Internal headers stay here in
# src/ and are PRIVATE to the library.
if(BNL_ENABLE_FFI)
    set(_BNL_CORE_TYPE SHARED)
else()
    set(_BNL_CORE_TYPE STATIC)
endif()

add_library(bnl_core ${_BNL_CORE_TYPE}
    lexer.cpp
    parser.cpp
    ast_printer.cpp
    value.cpp
    environment.cpp
    interpreter/runtime.cpp
    interpreter/exprs.cpp
    interpreter/stmts.cpp
    interpreter/builtins.cpp
    module_loader.cpp
    native_module.cpp
    class_type.cpp
    native/sys.cpp
    native/io.cpp
    native/timers.cpp
    native/crypto.cpp
    native/zlib_mod.cpp
    native/httpp.cpp
    native/net.cpp
    native/tls.cpp
    native/exec.cpp
    native/json.cpp
)

# dynamic_library.cpp is only needed when FFI is enabled — and on Linux the
# `dl` link is only needed in that mode too.
if(BNL_ENABLE_FFI)
    target_sources(bnl_core PRIVATE dynamic_library.cpp)
endif()

# Make sure the embedded-stdlib header is generated before any source compiles.
add_dependencies(bnl_core stdlib_embedded)

target_include_directories(bnl_core
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include       # bnl/*.h — visible to hosts
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}       # src/*.h — internal headers
        ${CMAKE_BINARY_DIR}/generated     # stdlib_embedded.h
)

# libuv is in the public header surface (uv_loop_t* exposed via Interpreter).
# Hosts that link bnl_core get libuv transitively. Other deps stay PRIVATE.
target_link_libraries(bnl_core
    PUBLIC
        $<IF:$<TARGET_EXISTS:libuv::uv_a>,libuv::uv_a,libuv::uv>
    PRIVATE
        fmt::fmt
        OpenSSL::SSL
        OpenSSL::Crypto
        ZLIB::ZLIB
        $<IF:$<TARGET_EXISTS:llhttp::llhttp_static>,llhttp::llhttp_static,llhttp::llhttp_shared>
        nlohmann_json::nlohmann_json
)

if(BNL_ENABLE_FFI)
    target_link_libraries(bnl_core PRIVATE ${CMAKE_DL_LIBS})  # dl on Linux
endif()

# tls.cpp pulls Windows root certs from the system store; needs crypt32.
if(WIN32)
    target_link_libraries(bnl_core PRIVATE crypt32)
endif()

# bnl: thin CLI driver around bnl_core.
add_executable(bnl main.cpp)

target_link_libraries(bnl
    PRIVATE
        bnl_core
        fmt::fmt
)

target_include_directories(bnl
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}       # main.cpp uses internal lexer.h/parser.h/ast_printer.h
)

set_target_properties(bnl PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# PIC + auto-export are only meaningful when bnl_core is shared. Static-mode
# bnl_core links right into bnl.exe and skips both.
if(BNL_ENABLE_FFI)
    set_target_properties(bnl_core PROPERTIES
        POSITION_INDEPENDENT_CODE  ON
        WINDOWS_EXPORT_ALL_SYMBOLS ON
        RUNTIME_OUTPUT_DIRECTORY   ${CMAKE_BINARY_DIR}/bin
        LIBRARY_OUTPUT_DIRECTORY   ${CMAKE_BINARY_DIR}/bin
    )
    target_compile_definitions(bnl_core PUBLIC BNL_FFI_ENABLED=1)
endif()
