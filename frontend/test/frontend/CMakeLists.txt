include(GoogleTest)

add_executable(
    nui-sftp-frontend-test
    main.cpp
)

target_include_directories(
    nui-sftp-frontend-test
    PRIVATE
        "${CMAKE_SOURCE_DIR}/frontend/include"
        "${CMAKE_SOURCE_DIR}/nui-file-explorer/include"
)

target_link_libraries(
    nui-sftp-frontend-test
    PUBLIC
        shared-data
        GTest::gtest
        GTest::gmock_main
        GTest::gtest_main
)

set_target_properties(nui-sftp-frontend-test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test")

gtest_discover_tests(nui-sftp-frontend-test)

if (DEFINED ENV{MSYSTEM})
    add_custom_command(TARGET nui-sftp-frontend-test POST_BUILD
        COMMAND bash -c "ldd $<TARGET_FILE:nui-sftp-frontend-test>" | "grep" "clang" | awk "NF == 4 { system(\"${CMAKE_COMMAND} -E copy \" \$3 \" $<TARGET_FILE_DIR:nui-sftp-frontend-test>\") }"
        VERBATIM
    )
endif()
