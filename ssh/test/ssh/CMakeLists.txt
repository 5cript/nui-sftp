include(GoogleTest)

find_package(Boost CONFIG REQUIRED COMPONENTS filesystem process)
if (NOT TARGET GTest::gtest)
    find_package(GTest CONFIG REQUIRED)
endif()

find_program(NODE node)
find_program(NPM npm)

function(add_ssh_test name secureShell outputDirectory)
    add_executable(${name} main.cpp)

    target_link_libraries(
        ${name}
        PUBLIC
            utility
            ${secureShell}
            GTest::gtest
            GTest::gtest_main
            Boost::filesystem
            Boost::process
    )

    if (WIN32)
        target_link_libraries(${name} PUBLIC ws2_32)
        find_program(CMD cmd)
        target_compile_definitions(${name} PRIVATE -DCMD_EXECUTABLE="${CMD}" -DMSYS2_BASH="${MSYS2_BASH}")
    endif()

    target_compile_definitions(${name} PRIVATE -DNODE_EXECUTABLE="${NODE}" -DNPM_EXECUTABLE="${NPM}")
    set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${outputDirectory}")
    add_dependencies(${name} build_test_ssh2_servers)
endfunction()

file(GLOB TEST_SSH2_SERVER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/test_ssh2_servers/source/*.mjs")

add_custom_command(
    OUTPUT
        "${CMAKE_CURRENT_LIST_DIR}/test_ssh2_servers/node_modules/.package-lock.json"
    COMMAND ${NPM} install
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/test_ssh2_servers"
    DEPENDS
        "${CMAKE_CURRENT_LIST_DIR}/test_ssh2_servers/package.json"
        "${CMAKE_CURRENT_LIST_DIR}/test_ssh2_servers/package-lock.json"
    COMMENT "Installing test_ssh2_servers npm dependencies"
)

add_custom_command(
    OUTPUT
        "${CMAKE_CURRENT_LIST_DIR}/test_ssh2_servers/dist/shell.mjs"
        "${CMAKE_CURRENT_LIST_DIR}/test_ssh2_servers/dist/sftp.mjs"
        "${CMAKE_CURRENT_BINARY_DIR}/ssh_server/sftp.mjs"
        "${CMAKE_CURRENT_BINARY_DIR}/ssh_server/shell.mjs"
        "${CMAKE_CURRENT_BINARY_DIR}/ssh_server/key.private"
        "${CMAKE_CURRENT_BINARY_DIR}/ssh_server/key.public"
    COMMAND ${NPM} run build
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/ssh_server"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_LIST_DIR}/test_ssh2_servers/dist/sftp.mjs" "${CMAKE_CURRENT_BINARY_DIR}/ssh_server/sftp.mjs"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_LIST_DIR}/test_ssh2_servers/dist/shell.mjs" "${CMAKE_CURRENT_BINARY_DIR}/ssh_server/shell.mjs"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_LIST_DIR}/test_ssh2_servers/key.private" "${CMAKE_CURRENT_BINARY_DIR}/ssh_server/key.private"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_LIST_DIR}/test_ssh2_servers/key.public" "${CMAKE_CURRENT_BINARY_DIR}/ssh_server/key.public"
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/test_ssh2_servers"
    DEPENDS
        "${CMAKE_CURRENT_LIST_DIR}/test_ssh2_servers/node_modules/.package-lock.json"
        "${CMAKE_CURRENT_LIST_DIR}/test_ssh2_servers/sftp_server.mjs"
        "${CMAKE_CURRENT_LIST_DIR}/test_ssh2_servers/shell_emulator.mjs"
        ${TEST_SSH2_SERVER_SOURCES}
    COMMENT "Building test_ssh2_servers with webpack"
)
add_custom_target(
    build_test_ssh2_servers
    DEPENDS
        "${CMAKE_CURRENT_LIST_DIR}/test_ssh2_servers/dist/shell.mjs"
        "${CMAKE_CURRENT_LIST_DIR}/test_ssh2_servers/dist/sftp.mjs"
)

add_ssh_test(nui-sftp-ssh-test secure-shell "${CMAKE_BINARY_DIR}/test")
gtest_discover_tests(nui-sftp-ssh-test)

# The processing thread, strands and transfers share state across threads, so their tests always run a second
# time with ThreadSanitizer. libssh and the other libraries stay uninstrumented.
if (NOT WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    set(THREAD_SANITIZER_OPTIONS -fsanitize=thread -fno-omit-frame-pointer)
    add_secure_shell_library(secure-shell-tsan)
    target_compile_options(secure-shell-tsan PRIVATE ${THREAD_SANITIZER_OPTIONS})
    add_ssh_test(nui-sftp-ssh-tsan-test secure-shell-tsan "${CMAKE_BINARY_DIR}/test/tsan")
    # The mock server helper hands objects between the test and its asio thread, which TSAN only follows instrumented.
    target_sources(nui-sftp-ssh-tsan-test PRIVATE "${CMAKE_SOURCE_DIR}/utility/source/utility/node/node.cpp")
    target_compile_options(nui-sftp-ssh-tsan-test PRIVATE ${THREAD_SANITIZER_OPTIONS})
    target_link_options(nui-sftp-ssh-tsan-test PRIVATE -fsanitize=thread)
    gtest_discover_tests(
        nui-sftp-ssh-tsan-test
        TEST_PREFIX "Tsan."
        PROPERTIES ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1 second_deadlock_stack=1"
        DISCOVERY_TIMEOUT 60
    )
endif()

# If msys2, copy dynamic libraries to executable directory, visual studio does this automatically.
# And there is no need on linux.
if (DEFINED ENV{MSYSTEM})
    add_custom_command(TARGET nui-sftp-ssh-test POST_BUILD
        COMMAND bash -c "ldd $<TARGET_FILE:nui-sftp-ssh-test>" | "grep" "clang" | awk "NF == 4 { system(\"${CMAKE_COMMAND} -E copy \" \$3 \" $<TARGET_FILE_DIR:nui-sftp-ssh-test>\") }"
        VERBATIM
    )
endif()