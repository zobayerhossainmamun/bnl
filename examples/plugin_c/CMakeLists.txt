# Sample bnl plugin (pure C). Built as a shared library that does not link
# against bnl_core — every interaction goes through the bnl_api function-
# pointer table handed in at runtime.
#
# This is also what plugin authors do externally: copy include/bnl/plugin.h,
# compile a shared library against it, drop the .dll/.so/.dylib next to a
# bnl script.

add_library(mathx SHARED mathx.c)
set_target_properties(mathx PROPERTIES
    C_STANDARD                   99
    PREFIX                       ""
    OUTPUT_NAME                  "mathx"
    LIBRARY_OUTPUT_DIRECTORY     ${CMAKE_BINARY_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY     ${CMAKE_BINARY_DIR}/bin
    POSITION_INDEPENDENT_CODE    ON
)

# The plugin only needs the header. No link against bnl.
target_include_directories(mathx PRIVATE ${PROJECT_SOURCE_DIR}/include)

# Link libm on POSIX for sqrt().
if(UNIX)
    target_link_libraries(mathx PRIVATE m)
endif()
