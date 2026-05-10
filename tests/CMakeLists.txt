set(BNL_TEST_DRIVER ${CMAKE_SOURCE_DIR}/cmake/run_bnl_test.cmake)

function(add_bnl_test name)
    add_test(
        NAME ${name}
        COMMAND ${CMAKE_COMMAND}
            -DBNL=$<TARGET_FILE:bnl>
            -DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/${name}.bnl
            -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/${name}.expected
            -P ${BNL_TEST_DRIVER}
    )
endfunction()

add_bnl_test(arithmetic)
add_bnl_test(strings)
add_bnl_test(control_flow)
add_bnl_test(closures)
add_bnl_test(recursion)
add_bnl_test(bilingual)
add_bnl_test(member_access)
add_bnl_test(import_basic)
add_bnl_test(import_stdlib)
add_bnl_test(import_bilingual)
add_bnl_test(native_sys)
add_bnl_test(native_io)
add_bnl_test(timers_basic)
add_bnl_test(timers_interval)
add_bnl_test(io_async)
add_bnl_test(native_crypto)
add_bnl_test(native_zlib)
add_bnl_test(native_httpp)
add_bnl_test(native_net)
add_bnl_test(native_exec)
add_bnl_test(lists)
add_bnl_test(maps)
add_bnl_test(strings_intrinsics)
add_bnl_test(lib_path)
add_bnl_test(lib_json)
add_bnl_test(lib_http)
add_bnl_test(lib_request)
add_bnl_test(lib_web_https)
add_bnl_test(lib_filesystem)
add_bnl_test(lib_upload)
add_bnl_test(lib_web)
add_bnl_test(lib_download)
add_bnl_test(deps_basic)
add_bnl_test(classes_basic)
add_bnl_test(classes_bilingual)
