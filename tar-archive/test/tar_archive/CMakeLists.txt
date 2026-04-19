include(GoogleTest)

add_executable(
    nui-sftp-tar-archive-test
    main.cpp
)

find_package(GTest CONFIG REQUIRED)

target_link_libraries(
    nui-sftp-tar-archive-test
    PUBLIC
        tar-archive
        shared-data
        utility
        GTest::gtest
        GTest::gtest_main
)

find_program(TAR_EXECUTABLE tar)
if (TAR_EXECUTABLE)
    message(STATUS "tar-archive tests: GNU tar interop enabled via ${TAR_EXECUTABLE}")
    target_compile_definitions(nui-sftp-tar-archive-test PRIVATE -DTAR_EXECUTABLE="${TAR_EXECUTABLE}")
else()
    message(WARNING "tar-archive tests: system 'tar' not found; GNU tar interop tests will fail at runtime")
    target_compile_definitions(nui-sftp-tar-archive-test PRIVATE -DTAR_EXECUTABLE="")
endif()

target_include_directories(
    nui-sftp-tar-archive-test
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
)

set_target_properties(
    nui-sftp-tar-archive-test
    PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test"
)

gtest_discover_tests(nui-sftp-tar-archive-test)
