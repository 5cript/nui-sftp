add_library(
    shared-data
    STATIC
        directory_entry.cpp
        ignore_rules.cpp
)

target_include_directories(shared-data PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../../include" "${CMAKE_CURRENT_SOURCE_DIR}/../../../ssh/include")

target_link_libraries(shared-data PRIVATE core-target PUBLIC utility ids fmt)

if (EMSCRIPTEN)
    target_link_libraries(shared-data PUBLIC nui-frontend)
else()
    target_link_libraries(shared-data PUBLIC nui-backend)
endif()