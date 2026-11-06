include(GoogleTest)

find_package(Boost CONFIG REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS SSL Crypto)
find_package(fmt CONFIG REQUIRED)
if (NOT TARGET GTest::gtest)
    find_package(GTest CONFIG REQUIRED)
endif()

# The test support shares objects with the test across threads, so every variant builds its own.
function(add_ftp_test name ftpLibrary remoteIoLibrary outputDirectory)
    add_library(
        ${name}-support
        STATIC
            support/ftp_script.cpp
            support/scripted_ftp_server.cpp
            support/test_ftp_client.cpp
            support/test_pki.cpp
    )
    target_include_directories(${name}-support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
    target_link_libraries(
        ${name}-support
        PUBLIC
            ${ftpLibrary}
            Boost::headers
            OpenSSL::SSL
            OpenSSL::Crypto
            fmt::fmt
    )
    if (WIN32)
        target_link_libraries(${name}-support PUBLIC ws2_32 mswsock)
    endif()

    add_executable(${name} main.cpp)

    target_link_libraries(
        ${name}
        PUBLIC
            utility
            ${name}-support
            ${ftpLibrary}
            ${remoteIoLibrary}
            GTest::gtest
            GTest::gtest_main
    )

    set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${outputDirectory}")
endfunction()

add_ftp_test(nui-sftp-ftp-test file-transfer-protocol remote-io "${CMAKE_BINARY_DIR}/test")
gtest_discover_tests(nui-sftp-ftp-test)

# A configure of its own with -DCODE_COVERAGE=ON, a report to look at that no test fails on. The instrumented libraries
# need the instrumented link of the tests named here, so build only the ccov targets there.
if (CODE_COVERAGE)
    target_code_coverage(remote-io)
    target_code_coverage(file-transfer-protocol)
    target_code_coverage(nui-sftp-ftp-test AUTO ALL LLVM_EXCLUDE ${NUI_SFTP_COVERAGE_EXCLUDES})
    target_code_coverage(nui-sftp-remote-io-test AUTO ALL LLVM_EXCLUDE ${NUI_SFTP_COVERAGE_EXCLUDES})
endif()

# Builds instrumented copies of file-transfer-protocol and its tests in test/<variant>, on the instrumented remote_io
# of its tests. ctest names the tests <testPrefix>.*. OpenSSL and the other prebuilt libraries stay uninstrumented.
function(add_sanitized_ftp_test variant testPrefix environment)
    set(sanitizerOptions ${ARGN})
    add_file_transfer_protocol_library(file-transfer-protocol-${variant} remote-io-${variant})
    target_compile_options(file-transfer-protocol-${variant} PRIVATE ${sanitizerOptions})
    add_ftp_test(
        nui-sftp-ftp-${variant}-test
        file-transfer-protocol-${variant}
        remote-io-${variant}
        "${CMAKE_BINARY_DIR}/test/${variant}"
    )
    target_compile_options(nui-sftp-ftp-${variant}-test-support PRIVATE ${sanitizerOptions})
    target_compile_options(nui-sftp-ftp-${variant}-test PRIVATE ${sanitizerOptions})
    target_link_options(nui-sftp-ftp-${variant}-test PRIVATE ${sanitizerOptions})
    gtest_discover_tests(
        nui-sftp-ftp-${variant}-test
        TEST_PREFIX "${testPrefix}."
        PROPERTIES ENVIRONMENT "${environment}"
        DISCOVERY_TIMEOUT 60
    )
endfunction()

if (NOT WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_sanitized_ftp_test(
        tsan
        Tsan
        "TSAN_OPTIONS=halt_on_error=1 second_deadlock_stack=1"
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
    add_sanitized_ftp_test(
        asan
        Asan
        "${addressSanitizerEnvironment}"
        -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined
    )
endif()

# If msys2, copy dynamic libraries to executable directory, visual studio does this automatically.
# And there is no need on linux.
if (DEFINED ENV{MSYSTEM})
    add_custom_command(TARGET nui-sftp-ftp-test POST_BUILD
        COMMAND bash -c "ldd $<TARGET_FILE:nui-sftp-ftp-test>" | "grep" "clang" | awk "NF == 4 { system(\"${CMAKE_COMMAND} -E copy \" \$3 \" $<TARGET_FILE_DIR:nui-sftp-ftp-test>\") }"
        VERBATIM
    )
endif()
