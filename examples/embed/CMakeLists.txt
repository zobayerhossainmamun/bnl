# Minimal C++ host that embeds bnl. Linked against bnl_core; relies on its
# PUBLIC include path so <bnl/...> headers resolve.
add_executable(embed_demo host.cpp)

target_link_libraries(embed_demo
    PRIVATE bnl_core
)

set_target_properties(embed_demo PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
