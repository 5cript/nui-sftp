include(GoogleTest)

add_executable(
    nui-sftp-command-store-test
    command_store.test.cpp
    snippet_transfer.test.cpp
    snippet_import_plan.test.cpp
    snippet_presets.test.cpp
)

if (NOT TARGET GTest::gtest)
    find_package(GTest CONFIG REQUIRED)
endif()
include("${CMAKE_CURRENT_LIST_DIR}/../../../_cmake/dependencies/sqlite3.cmake")

target_link_libraries(
    nui-sftp-command-store-test
    PUBLIC
        command-store
        utility
        SQLite3::SQLite3
        GTest::gtest
        GTest::gtest_main
)

target_compile_definitions(
    nui-sftp-command-store-test
    PRIVATE
        SNIPPET_PRESETS_DIRECTORY="${CMAKE_SOURCE_DIR}/command-store/presets"
)

set_target_properties(
    nui-sftp-command-store-test
    PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test"
)

gtest_discover_tests(nui-sftp-command-store-test)
