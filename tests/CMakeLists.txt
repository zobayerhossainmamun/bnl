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
add_bnl_test(classes_basic        lang)
add_bnl_test(classes_bilingual    lang)
add_bnl_test(classes_inheritance  lang)
add_bnl_test(try_catch            lang)
add_bnl_test(for_loop             lang)

# ---------- built-in stdlib (sys, io, timers, regex, crypto, net, http, tls) -
add_bnl_test(native_sys           stdlib)
add_bnl_test(native_io            stdlib)
add_bnl_test(timers_basic         stdlib)
add_bnl_test(timers_interval      stdlib)
add_bnl_test(io_async             stdlib)
add_bnl_test(native_regex         stdlib)
add_bnl_test(native_crypto        stdlib)
add_bnl_test(native_net           stdlib)
add_bnl_test(native_http          stdlib)
add_bnl_test(native_tls           stdlib)
add_bnl_test(native_json          stdlib)
add_bnl_test(native_exec          stdlib)
add_bnl_test(native_dns           stdlib)

# ---------- embedded bnl stdlib (lib/*.bnl) -------------------------------
add_bnl_test(lib_url              stdlib)
add_bnl_test(lib_web              stdlib)
add_bnl_test(lib_request          stdlib)
add_bnl_test(lib_web_https        stdlib)
add_bnl_test(lib_exec             stdlib)
add_bnl_test(lib_dns              stdlib)

# ---------- integration tests that exercise FFI plugins -------------------
# Skipped when BNL_ENABLE_FFI=OFF (the mathx sample plugin isn't built).
if(BNL_ENABLE_FFI)
    # The proj_full fixture is mostly static under _fixtures/proj_full/.
    # The mathx-plugin dep is staged at build time:
    #   - bnl.json is generated with the actual per-OS plugin filename so
    #     the same fixture works on Windows (.dll), Linux (.so), macOS (.dylib).
    #   - The built plugin is copied into the dep dir so the dep loader can
    #     find it via the relative path in the manifest.
    set(_PLUGIN_DEP_DIR
        ${CMAKE_CURRENT_SOURCE_DIR}/_fixtures/proj_full/deps/mathx-plugin)

    file(GENERATE
        OUTPUT  ${_PLUGIN_DEP_DIR}/bnl.json
        CONTENT "{ \"name\": \"mathx-plugin\", \"native\": \"$<TARGET_FILE_NAME:mathx>\" }\n"
    )

    add_custom_target(proj_full_plugin_stage ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_PLUGIN_DEP_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:mathx>
                ${_PLUGIN_DEP_DIR}/$<TARGET_FILE_NAME:mathx>
        DEPENDS mathx
        COMMENT "Staging mathx into proj_full fixture"
    )

    add_bnl_test(proj_full lang)

    # direct_plugin — direct-path import of a .dll. Imports take a string
    # literal so the path can't be composed at runtime; we generate the
    # script at config time with $<TARGET_FILE_NAME:mathx> baked in.
    set(_DIRECT_PLUGIN_BNL ${CMAKE_BINARY_DIR}/test_fixtures/direct_plugin.bnl)
    file(GENERATE
        OUTPUT  ${_DIRECT_PLUGIN_BNL}
        CONTENT
"import \"${CMAKE_BINARY_DIR}/bin/$<TARGET_FILE_NAME:mathx>\" as m;
print(\"cube:\", m.cube(3));
print(\"hypot:\", m.hypot(5, 12));
print(\"greeting:\", m.greeting);
"
    )
    add_test(NAME direct_plugin
        COMMAND ${CMAKE_COMMAND}
            -DBNL=$<TARGET_FILE:bnl>
            -DSCRIPT=${_DIRECT_PLUGIN_BNL}
            -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/lang/direct_plugin.expected
            -P ${BNL_TEST_DRIVER}
    )
endif()
