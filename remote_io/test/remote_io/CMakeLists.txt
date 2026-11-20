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

# The header-only fakes, for the tests of the targets that use remote-io: <remote_io/fake_remote_file_system.hpp>.
add_library(remote-io-test-doubles INTERFACE)
target_include_directories(remote-io-test-doubles INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(remote-io-test-doubles INTERFACE remote-io)

add_remote_io_test(nui-sftp-remote-io-test remote-io "${CMAKE_BINARY_DIR}/test")
gtest_discover_tests(nui-sftp-remote-io-test)

# Builds an instrumented copy of remote-io and its tests in test/<variant>, ctest names the tests <testPrefix>.*.
function(add_sanitized_remote_io_test variant testPrefix environment)
    set(sanitizerOptions ${ARGN})
    add_remote_io_library(remote-io-${variant})
    target_compile_options(remote-io-${variant} PRIVATE ${sanitizerOptions})
    add_remote_io_test(nui-sftp-remote-io-${variant}-test remote-io-${variant} "${CMAKE_BINARY_DIR}/test/${variant}")
    target_compile_options(nui-sftp-remote-io-${variant}-test PRIVATE ${sanitizerOptions})
    target_link_options(nui-sftp-remote-io-${variant}-test PRIVATE ${sanitizerOptions})
    gtest_discover_tests(
        nui-sftp-remote-io-${variant}-test
        TEST_PREFIX "${testPrefix}."
        PROPERTIES ENVIRONMENT "${environment}"
        DISCOVERY_TIMEOUT 60
    )
endfunction()

# The processing thread and strands share state across threads, so their tests always run a second time with
# ThreadSanitizer, and a third time with AddressSanitizer and UndefinedBehaviorSanitizer for lifetime bugs. The ssh
# tests link the ThreadSanitizer copy too.
if (NOT WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_sanitized_remote_io_test(
        tsan
        Tsan
        "TSAN_OPTIONS=halt_on_error=1 second_deadlock_stack=1 suppressions=${CMAKE_SOURCE_DIR}/_cmake/tsan_suppressions.txt"
        -fsanitize=thread -fno-omit-frame-pointer
    )

    # Apple's clang has no LeakSanitizer.
    if (APPLE)
        set(detectLeaks 0)
    else()
        set(detectLeaks 1)
    endif()
    set(addressSanitizerEnvironment
        "ASAN_OPTIONS=detect_leaks=${detectLeaks}:halt_on_error=1:detect_stack_use_after_return=1"
        "UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1"
    )
    add_sanitized_remote_io_test(
        asan
        Asan
        "${addressSanitizerEnvironment}"
        -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined
    )
    # Only remote_io is instrumented. gtest and the other libraries change libc++ vectors without updating their ASan
    # annotations, which then report false overflows (gtest's test registration on macOS).
    target_compile_definitions(remote-io-asan PUBLIC __SANITIZER_DISABLE_CONTAINER_OVERFLOW__)
endif()

# If msys2, copy dynamic libraries to executable directory, visual studio does this automatically.
# And there is no need on linux.
if (DEFINED ENV{MSYSTEM})
    add_custom_command(TARGET nui-sftp-remote-io-test POST_BUILD
        COMMAND bash -c "ldd $<TARGET_FILE:nui-sftp-remote-io-test>" | "grep" "clang" | awk "NF == 4 { system(\"${CMAKE_COMMAND} -E copy \" \$3 \" $<TARGET_FILE_DIR:nui-sftp-remote-io-test>\") }"
        VERBATIM
    )
endif()
