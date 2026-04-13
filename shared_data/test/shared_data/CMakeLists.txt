include(GoogleTest)

add_executable(
    nui-sftp-shared-data-test
    main.cpp
)

target_link_libraries(
    nui-sftp-shared-data-test
    PUBLIC
        shared-data
        GTest::gtest
        GTest::gmock_main
        GTest::gtest_main
)

set_target_properties(nui-sftp-shared-data-test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test")

gtest_discover_tests(nui-sftp-shared-data-test)

if (DEFINED ENV{MSYSTEM})
    add_custom_command(TARGET nui-sftp-shared-data-test POST_BUILD
        COMMAND bash -c "ldd $<TARGET_FILE:nui-sftp-shared-data-test>" | "grep" "clang" | awk "NF == 4 { system(\"${CMAKE_COMMAND} -E copy \" \$3 \" $<TARGET_FILE_DIR:nui-sftp-shared-data-test>\") }"
        VERBATIM
    )
endif()
