include(GoogleTest)

add_executable(
    nui-scp-backend-test
    main.cpp
)

find_package(GTest CONFIG REQUIRED)

target_link_libraries(
    nui-scp-backend-test
    PUBLIC
        secure-shell
        utility
        backend
        GTest::GTest
        GTest::gmock_main
        GTest::Main
)

if (WIN32)
    target_link_libraries(nui-scp-backend-test PUBLIC ws2_32)
endif()

# For sftp_server.mjs bundled
add_dependencies(nui-scp-backend-test nui-scp-ssh-test)

set_target_properties(nui-scp-backend-test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test")

gtest_discover_tests(nui-scp-backend-test)

# If msys2, copy dynamic libraries to executable directory, visual studio does this automatically.
# And there is no need on linux.
if (DEFINED ENV{MSYSTEM})
    add_custom_command(TARGET nui-scp-backend-test POST_BUILD
        COMMAND bash -c "ldd $<TARGET_FILE:nui-scp-backend-test>" | "grep" "clang" | awk "NF == 4 { system(\"${CMAKE_COMMAND} -E copy \" \$3 \" $<TARGET_FILE_DIR:nui-scp-backend-test>\") }"
        VERBATIM
    )
endif()