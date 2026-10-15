add_library(
    shared-data
    STATIC
        directory_entry.cpp
        file_operations/compared_state.cpp
        ignore_rules.cpp
        sync/case_fold.cpp
        sync/diff.cpp
        sync/diff_tree_store.cpp
        sync/enqueue_minimizer.cpp
)

target_include_directories(shared-data PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../../include" "${CMAKE_CURRENT_SOURCE_DIR}/../../../ssh/include")

target_link_libraries(shared-data PRIVATE core-target PUBLIC utility ids paths fmt::fmt)

if (EMSCRIPTEN)
    target_link_libraries(shared-data PUBLIC nui-frontend)
else()
    target_link_libraries(shared-data PUBLIC nui-backend)
endif()