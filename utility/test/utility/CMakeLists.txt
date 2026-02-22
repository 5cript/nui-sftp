include(GoogleTest)

add_executable(
    nui-sftp-utility-test
    main.cpp
)

find_package(Boost CONFIG REQUIRED COMPONENTS filesystem process)

target_link_libraries(
    nui-sftp-utility-test
    PUBLIC
        utility
        shared-data
        GTest::GTest
        GTest::gmock_main
        GTest::Main
        Boost::filesystem
)

set_target_properties(nui-sftp-utility-test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test")

gtest_discover_tests(nui-sftp-utility-test)

# If msys2, copy dynamic libraries to executable directory, visual studio does this automatically.
# And there is no need on linux.
if (DEFINED ENV{MSYSTEM})
    add_custom_command(TARGET nui-sftp-utility-test POST_BUILD
        COMMAND bash -c "ldd $<TARGET_FILE:nui-sftp-utility-test>" | "grep" "clang" | awk "NF == 4 { system(\"${CMAKE_COMMAND} -E copy \" \$3 \" $<TARGET_FILE_DIR:nui-sftp-utility-test>\") }"
        VERBATIM
    )
endif()