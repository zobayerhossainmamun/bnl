set(BNL_TEST_DRIVER ${CMAKE_SOURCE_DIR}/cmake/run_bnl_test.cmake)

# add_bnl_test(name dir) — looks for ${dir}/${name}.bnl + .expected.
function(add_bnl_test name dir)
    add_test(
        NAME ${name}
        COMMAND ${CMAKE_COMMAND}
            -DBNL=$<TARGET_FILE:bnl>
            -DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/${dir}/${name}.bnl
            -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/${dir}/${name}.expected
            -P ${BNL_TEST_DRIVER}
    )
endfunction()

# ---------- language semantics --------------------------------------------
add_bnl_test(arithmetic           lang)
add_bnl_test(strings              lang)
add_bnl_test(control_flow         lang)
add_bnl_test(closures             lang)
add_bnl_test(recursion            lang)
add_bnl_test(bilingual            lang)
add_bnl_test(member_access        lang)
add_bnl_test(lists                lang)
add_bnl_test(maps                 lang)
add_bnl_test(strings_intrinsics   lang)
add_bnl_test(import_basic         lang)
add_bnl_test(import_stdlib        lang)
add_bnl_test(import_bilingual     lang)
add_bnl_test(deps_basic           lang)
add_bnl_test(classes_basic        lang)
add_bnl_test(classes_bilingual    lang)
add_bnl_test(classes_inheritance  lang)

# ---------- module behaviors (native + lib/) ------------------------------
add_bnl_test(native_sys           stdlib)
add_bnl_test(native_io            stdlib)
add_bnl_test(native_httpp         stdlib)
add_bnl_test(native_net           stdlib)
add_bnl_test(native_crypto        stdlib)
add_bnl_test(native_zlib          stdlib)
add_bnl_test(native_exec          stdlib)
add_bnl_test(native_sqlite        stdlib)
add_bnl_test(timers_basic         stdlib)
add_bnl_test(timers_interval      stdlib)
add_bnl_test(io_async             stdlib)
add_bnl_test(lib_path             stdlib)
add_bnl_test(lib_json             stdlib)
add_bnl_test(lib_http             stdlib)
add_bnl_test(lib_request          stdlib)
add_bnl_test(lib_web_https        stdlib)
add_bnl_test(lib_filesystem       stdlib)
add_bnl_test(lib_upload           stdlib)
add_bnl_test(lib_web              stdlib)
add_bnl_test(lib_download         stdlib)
