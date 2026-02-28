include(GoogleTest)

add_executable(
    nui-sftp-ssh-test
    main.cpp
)

find_package(Boost CONFIG REQUIRED COMPONENTS filesystem process)
find_package(GTest CONFIG REQUIRED)

target_link_libraries(
    nui-sftp-ssh-test
    PUBLIC
        utility
        secure-shell
        GTest::gtest
        GTest::gtest_main
        Boost::filesystem
        Boost::process
)

if (WIN32)
    target_link_libraries(nui-sftp-ssh-test PUBLIC ws2_32)
    find_program(CMD cmd)
    target_compile_definitions(nui-sftp-ssh-test PRIVATE -DCMD_EXECUTABLE="${CMD}" -DMSYS2_BASH="${MSYS2_BASH}")
endif()

set_target_properties(nui-sftp-ssh-test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test")

find_program(NODE node)
find_program(NPM npm)

target_compile_definitions(nui-sftp-ssh-test PRIVATE -DNODE_EXECUTABLE="${NODE}" -DNPM_EXECUTABLE="${NPM}")

add_custom_command(
    OUTPUT
        "${CMAKE_CURRENT_LIST_DIR}/test_ssh2_servers/dist/shell.mjs"
        "${CMAKE_CURRENT_LIST_DIR}/test_ssh2_servers/dist/sftp.mjs"
        "${CMAKE_CURRENT_BUILD_DIR}/ssh_server/sftp.mjs"
        "${CMAKE_CURRENT_BUILD_DIR}/ssh_server/shell.mjs"
        "${CMAKE_CURRENT_BUILD_DIR}/ssh_server/key.private"
        "${CMAKE_CURRENT_BUILD_DIR}/ssh_server/key.public"
    COMMAND ${NPM} run build
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BUILD_DIR}/ssh_server"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_LIST_DIR}/test_ssh2_servers/dist/sftp.mjs" "${CMAKE_CURRENT_BUILD_DIR}/ssh_server/sftp.mjs"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_LIST_DIR}/test_ssh2_servers/dist/shell.mjs" "${CMAKE_CURRENT_BUILD_DIR}/ssh_server/shell.mjs"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_LIST_DIR}/test_ssh2_servers/key.private" "${CMAKE_CURRENT_BUILD_DIR}/ssh_server/key.private"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_LIST_DIR}/test_ssh2_servers/key.public" "${CMAKE_CURRENT_BUILD_DIR}/ssh_server/key.public"
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/test_ssh2_servers"
    DEPENDS
        "${CMAKE_CURRENT_LIST_DIR}/test_ssh2_servers/package.json"
        "${CMAKE_CURRENT_LIST_DIR}/test_ssh2_servers/sftp_server.mjs"
        "${CMAKE_CURRENT_LIST_DIR}/test_ssh2_servers/shell_emulator.mjs"
        "${CMAKE_CURRENT_LIST_DIR}/test_ssh2_servers/package-lock.json"
)
add_custom_target(
    build_test_ssh2_servers
    DEPENDS
        "${CMAKE_CURRENT_LIST_DIR}/test_ssh2_servers/dist/shell.mjs"
        "${CMAKE_CURRENT_LIST_DIR}/test_ssh2_servers/dist/sftp.mjs"
)
add_dependencies(nui-sftp-ssh-test build_test_ssh2_servers)

gtest_discover_tests(nui-sftp-ssh-test)

# If msys2, copy dynamic libraries to executable directory, visual studio does this automatically.
# And there is no need on linux.
if (DEFINED ENV{MSYSTEM})
    add_custom_command(TARGET nui-sftp-ssh-test POST_BUILD
        COMMAND bash -c "ldd $<TARGET_FILE:nui-sftp-ssh-test>" | "grep" "clang" | awk "NF == 4 { system(\"${CMAKE_COMMAND} -E copy \" \$3 \" $<TARGET_FILE_DIR:nui-sftp-ssh-test>\") }"
        VERBATIM
    )
endif()