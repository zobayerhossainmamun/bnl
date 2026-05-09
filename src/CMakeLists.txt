add_executable(bnl
    main.cpp
    lexer.cpp
    parser.cpp
    ast_printer.cpp
    value.cpp
    environment.cpp
    interpreter.cpp
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

# Make sure the embedded-stdlib header is generated before bnl.cpp files compile.
add_dependencies(bnl stdlib_embedded)

target_include_directories(bnl
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_BINARY_DIR}/generated
)

target_link_libraries(bnl
    PRIVATE
        fmt::fmt
        $<IF:$<TARGET_EXISTS:libuv::uv_a>,libuv::uv_a,libuv::uv>
        OpenSSL::SSL
        OpenSSL::Crypto
        ZLIB::ZLIB
        $<IF:$<TARGET_EXISTS:llhttp::llhttp_static>,llhttp::llhttp_static,llhttp::llhttp_shared>
        nlohmann_json::nlohmann_json
)

set_target_properties(bnl PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
