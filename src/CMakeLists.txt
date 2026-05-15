add_library(bnl_core STATIC
    # Lex / parse / AST printing.
    frontend/lexer.cpp
    frontend/parser.cpp
    frontend/ast_printer.cpp

    # Runtime: evaluator, value model, classes, environment, native module
    # builder, module loader.
    runtime/value.cpp
    runtime/environment.cpp
    runtime/class_type.cpp
    runtime/future.cpp
    runtime/bn_aliases.cpp
    runtime/native_module.cpp
    runtime/module_loader.cpp
    runtime/interpreter.cpp
    runtime/exprs.cpp
    runtime/stmts.cpp
    runtime/globals.cpp

    # Built-in native modules.
    stdlib/sys.cpp
    stdlib/io.cpp
    stdlib/timers.cpp
    stdlib/regex.cpp
    stdlib/crypto.cpp
    stdlib/net.cpp
    stdlib/http.cpp
    stdlib/tls.cpp
    stdlib/json.cpp
    stdlib/exec.cpp
    stdlib/dns.cpp
    stdlib/sqlite.cpp
    stdlib/pg.cpp
    stdlib/mongo.cpp
    stdlib/math.cpp
    stdlib/random.cpp
    stdlib/time.cpp
    stdlib/zlib.cpp
    stdlib/ffi.cpp

    # C-ABI plugin loader (`import "foo.dll"`).
    ffi/dynamic_library.cpp
    ffi/plugin_api.cpp
)

target_include_directories(bnl_core
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_BINARY_DIR}/generated
)

add_dependencies(bnl_core bnl_stdlib_embedded bnl_cacert_embedded)

target_link_libraries(bnl_core
    PUBLIC
        $<IF:$<TARGET_EXISTS:libuv::uv_a>,libuv::uv_a,libuv::uv>
    PRIVATE
        fmt::fmt
        nlohmann_json::nlohmann_json
        OpenSSL::SSL
        unofficial::sqlite3::sqlite3
        PostgreSQL::PostgreSQL
        $<IF:$<TARGET_EXISTS:mongoc::shared>,mongoc::shared,mongoc::static>
        ZLIB::ZLIB
        $<IF:$<TARGET_EXISTS:llhttp::llhttp_static>,llhttp::llhttp_static,llhttp::llhttp_shared>
        unofficial::libffi::libffi
)

# On Windows, compile the .rc resource file into the executable so it carries
# the bnlang.ico app icon (visible in Explorer, taskbar, alt-tab) plus the
# VS_VERSION_INFO block (CompanyName / FileDescription / version, visible in
# the file properties dialog). CMake invokes rc.exe automatically for .rc
# sources on Windows. The .rc references bnlang.ico via a relative path, so
# we also need to tell rc to look in resources/ for the .ico.
if(WIN32)
    add_executable(bnl main.cpp ${CMAKE_SOURCE_DIR}/resources/bnl.rc)
    set_source_files_properties(${CMAKE_SOURCE_DIR}/resources/bnl.rc
        PROPERTIES LANGUAGE RC)
    # /I tells rc.exe where to find bnlang.ico (referenced relatively from bnl.rc).
    set_property(SOURCE ${CMAKE_SOURCE_DIR}/resources/bnl.rc APPEND PROPERTY
        COMPILE_OPTIONS /I${CMAKE_SOURCE_DIR}/resources)
else()
    add_executable(bnl main.cpp)
endif()

target_link_libraries(bnl
    PRIVATE
        bnl_core
        fmt::fmt
)

target_include_directories(bnl
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

set_target_properties(bnl PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Plugins resolve their bnl symbols against the running bnl process. On Linux
# `dl` is needed for dlopen; bnl itself doesn't currently consume the libdl
# entry points (DynamicLibrary lives in bnl_core), but plugin authors using
# uv async hooks routinely will. Keep it linked even though some libcs roll
# dlopen into libc — explicit dep is portable.
if(UNIX AND NOT APPLE)
    target_link_libraries(bnl_core PRIVATE ${CMAKE_DL_LIBS})
endif()
