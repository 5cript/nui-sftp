include(GoogleTest)

add_executable(
    nui-sftp-backend-test
    main.cpp
)

find_package(GTest CONFIG REQUIRED)

target_link_libraries(
    nui-sftp-backend-test
    PUBLIC
        secure-shell
        utility
        backend
        GTest::GTest
        GTest::gmock_main
        GTest::Main
)

if (WIN32)
    target_link_libraries(nui-sftp-backend-test PUBLIC ws2_32)
endif()

# For sftp_server.mjs bundled
add_dependencies(nui-sftp-backend-test nui-sftp-ssh-test)

set_target_properties(nui-sftp-backend-test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test")

gtest_discover_tests(nui-sftp-backend-test)

# If msys2, copy dynamic libraries to executable directory, visual studio does this automatically.
# And there is no need on linux.
if (DEFINED ENV{MSYSTEM})
    add_custom_command(TARGET nui-sftp-backend-test POST_BUILD
        COMMAND bash -c "ldd $<TARGET_FILE:nui-sftp-backend-test>" | "grep" "clang" | awk "NF == 4 { system(\"${CMAKE_COMMAND} -E copy \" \$3 \" $<TARGET_FILE_DIR:nui-sftp-backend-test>\") }"
        VERBATIM
    )
endif()