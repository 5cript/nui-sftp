include(GoogleTest)

add_executable(
    nui-sftp-command-store-test
    command_store.test.cpp
)

find_package(GTest CONFIG REQUIRED)

target_link_libraries(
    nui-sftp-command-store-test
    PUBLIC
        command-store
        utility
        GTest::gtest
        GTest::gtest_main
)

set_target_properties(
    nui-sftp-command-store-test
    PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test"
)

gtest_discover_tests(nui-sftp-command-store-test)
