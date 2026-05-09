# bnl_core: the static library that hosts and the bnl CLI both link against.
# Public headers live in <repo>/include/bnl/. Internal headers stay here in src/
# and are PRIVATE to the library.
add_library(bnl_core STATIC
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
    native/sys.cpp
    native/io.cpp
    native/timers.cpp
    native/crypto.cpp
    native/zlib_mod.cpp
    native/httpp.cpp
    native/net.cpp
    native/json.cpp
)

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
