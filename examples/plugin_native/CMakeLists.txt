# mathx — sample bnl FFI plugin built as a shared library.
#
# Built into the same bin/ dir as bnl.exe / embed_demo, with a stable name
# (PREFIX/SUFFIX explicitly set) so cross-platform tests / demos can locate it
# without naming surprises.
add_library(mathx SHARED mathx.cpp)

target_link_libraries(mathx
    PRIVATE bnl_core   # plugin uses bnl::Value, NativeModule etc.
)

# C4190: shared_ptr returned through extern "C". Documented v1 constraint —
# plugin must be built with same toolchain as bnl_core. Future C ABI removes it.
if(MSVC)
    target_compile_options(mathx PRIVATE /wd4190)
endif()

set_target_properties(mathx PROPERTIES
    PREFIX                       ""
    OUTPUT_NAME                  "mathx"
    LIBRARY_OUTPUT_DIRECTORY     ${CMAKE_BINARY_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY     ${CMAKE_BINARY_DIR}/bin
    POSITION_INDEPENDENT_CODE    ON
)
