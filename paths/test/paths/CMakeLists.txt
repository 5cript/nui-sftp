include(GoogleTest)

add_executable(
    nui-sftp-paths-test
    main.cpp
)

target_link_libraries(
    nui-sftp-paths-test
    PUBLIC
        paths
        core-target
        GTest::gtest
        GTest::gtest_main
)

set_target_properties(nui-sftp-paths-test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test")

gtest_discover_tests(nui-sftp-paths-test)

# If msys2, copy dynamic libraries to executable directory, visual studio does this automatically.
# And there is no need on linux.
if (DEFINED ENV{MSYSTEM})
    add_custom_command(TARGET nui-sftp-paths-test POST_BUILD
        COMMAND bash -c "ldd $<TARGET_FILE:nui-sftp-paths-test>" | "grep" "clang" | awk "NF == 4 { system(\"${CMAKE_COMMAND} -E copy \" \$3 \" $<TARGET_FILE_DIR:nui-sftp-paths-test>\") }"
        VERBATIM
    )
endif()
