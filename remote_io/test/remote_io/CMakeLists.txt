include(GoogleTest)

if (NOT TARGET GTest::gtest)
    find_package(GTest CONFIG REQUIRED)
endif()

function(add_remote_io_test name remoteIo outputDirectory)
    add_executable(${name} main.cpp)

    target_link_libraries(
        ${name}
        PUBLIC
            utility
            ${remoteIo}
            GTest::gtest
            GTest::gtest_main
    )

    set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${outputDirectory}")
endfunction()

add_remote_io_test(nui-sftp-remote-io-test remote-io "${CMAKE_BINARY_DIR}/test")
gtest_discover_tests(nui-sftp-remote-io-test)

# The processing thread and strands share state across threads, so their tests always run a second time with
# ThreadSanitizer. The ssh tests link this copy too.
if (NOT WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    set(THREAD_SANITIZER_OPTIONS -fsanitize=thread -fno-omit-frame-pointer)
    add_remote_io_library(remote-io-tsan)
    target_compile_options(remote-io-tsan PRIVATE ${THREAD_SANITIZER_OPTIONS})
    add_remote_io_test(nui-sftp-remote-io-tsan-test remote-io-tsan "${CMAKE_BINARY_DIR}/test/tsan")
    target_compile_options(nui-sftp-remote-io-tsan-test PRIVATE ${THREAD_SANITIZER_OPTIONS})
    target_link_options(nui-sftp-remote-io-tsan-test PRIVATE -fsanitize=thread)
    gtest_discover_tests(
        nui-sftp-remote-io-tsan-test
        TEST_PREFIX "Tsan."
        PROPERTIES ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1 second_deadlock_stack=1"
        DISCOVERY_TIMEOUT 60
    )
endif()

# If msys2, copy dynamic libraries to executable directory, visual studio does this automatically.
# And there is no need on linux.
if (DEFINED ENV{MSYSTEM})
    add_custom_command(TARGET nui-sftp-remote-io-test POST_BUILD
        COMMAND bash -c "ldd $<TARGET_FILE:nui-sftp-remote-io-test>" | "grep" "clang" | awk "NF == 4 { system(\"${CMAKE_COMMAND} -E copy \" \$3 \" $<TARGET_FILE_DIR:nui-sftp-remote-io-test>\") }"
        VERBATIM
    )
endif()
