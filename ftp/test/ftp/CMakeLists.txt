include(GoogleTest)

find_package(Boost CONFIG REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS SSL Crypto)
find_package(fmt CONFIG REQUIRED)
if (NOT TARGET GTest::gtest)
    find_package(GTest CONFIG REQUIRED)
endif()

# A server is a directory below servers/ with a profile.json. Adding one takes no change here, only its name in this list.
set(NUI_SFTP_FTP_TEST_SERVERS "vsftpd;proftpd" CACHE STRING "The servers the FTP conformance suites run against")
file(GLOB FTP_SERVER_PROFILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/servers/*/profile.json")
foreach(server IN LISTS NUI_SFTP_FTP_TEST_SERVERS)
    if (NOT "${CMAKE_CURRENT_SOURCE_DIR}/servers/${server}/profile.json" IN_LIST FTP_SERVER_PROFILES)
        message(FATAL_ERROR "NUI_SFTP_FTP_TEST_SERVERS: ${server} has no servers/${server}/profile.json")
    endif()
endforeach()
list(JOIN NUI_SFTP_FTP_TEST_SERVERS "," FTP_TEST_SERVER_NAMES)

# The test support shares objects with the test across threads, so every variant builds its own.
function(add_ftp_test name ftpLibrary remoteIoLibrary outputDirectory)
    add_library(
        ${name}-support
        STATIC
            support/file_system_harness.cpp
            support/ftp_script.cpp
            support/scripted_ftp_server.cpp
            support/server_fleet.cpp
            support/server_profile.cpp
            support/test_ftp_client.cpp
            support/test_pki.cpp
            support/tree_responder.cpp
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
            nlohmann_json::nlohmann_json
    )
    target_compile_definitions(
        ${name}-support
        PRIVATE
            NUI_SFTP_FTP_SERVERS_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/servers"
            NUI_SFTP_FTP_TEST_SERVERS="${FTP_TEST_SERVER_NAMES}"
    )
    if (WIN32)
        target_link_libraries(${name}-support PUBLIC ws2_32 mswsock)
    else()
        target_link_libraries(${name}-support PUBLIC docker-control)
    endif()

    add_executable(${name} main.cpp compliance/non_happy_matrix.cpp)

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

    target_compile_definitions(${name} PRIVATE NUI_SFTP_FTP_LISTING_SAMPLES="${CMAKE_CURRENT_SOURCE_DIR}/samples/listings")

    set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${outputDirectory}")
endfunction()

# The suites against real servers share one fleet of containers, a ctest fixture starts and stops it. Run directly, the
# test binary starts its own.
set(FTP_FLEET_ENDPOINTS "${CMAKE_BINARY_DIR}/test/ftp-fleet/endpoints.json")
function(discover_ftp_tests name testPrefix environment)
    gtest_discover_tests(
        ${name}
        TEST_PREFIX "${testPrefix}"
        TEST_FILTER "-Conformance/*"
        PROPERTIES ENVIRONMENT "${environment}"
        DISCOVERY_TIMEOUT 60
    )
    if (NOT WIN32)
        list(APPEND environment "NUI_SFTP_FTP_FLEET_ENDPOINTS=${FTP_FLEET_ENDPOINTS}")
        gtest_discover_tests(
            ${name}
            TEST_PREFIX "${testPrefix}"
            TEST_FILTER "Conformance/*:-Conformance/PoolLimitConformance.*"
            PROPERTIES ENVIRONMENT "${environment}" FIXTURES_REQUIRED FtpServerFleet
            DISCOVERY_TIMEOUT 60
        )
        # The limits variant counts the connections of one address, so its cases run one at a time.
        gtest_discover_tests(
            ${name}
            TEST_PREFIX "${testPrefix}"
            TEST_FILTER "Conformance/PoolLimitConformance.*"
            PROPERTIES ENVIRONMENT "${environment}" FIXTURES_REQUIRED FtpServerFleet RESOURCE_LOCK FtpLimitsVariant
            DISCOVERY_TIMEOUT 60
        )
    endif()
endfunction()

add_ftp_test(nui-sftp-ftp-test file-transfer-protocol remote-io "${CMAKE_BINARY_DIR}/test")
discover_ftp_tests(nui-sftp-ftp-test "" "")

if (NOT WIN32)
    add_executable(nui-sftp-ftp-fleet support/fleet_tool.cpp)
    target_link_libraries(nui-sftp-ftp-fleet PRIVATE nui-sftp-ftp-test-support)
    set_target_properties(nui-sftp-ftp-fleet PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test")
    add_test(NAME FtpServerFleetStart COMMAND nui-sftp-ftp-fleet start "${FTP_FLEET_ENDPOINTS}")
    add_test(NAME FtpServerFleetStop COMMAND nui-sftp-ftp-fleet stop "${FTP_FLEET_ENDPOINTS}")
    set_tests_properties(FtpServerFleetStart PROPERTIES FIXTURES_SETUP FtpServerFleet TIMEOUT 900)
    set_tests_properties(FtpServerFleetStop PROPERTIES FIXTURES_CLEANUP FtpServerFleet)
endif()

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
    discover_ftp_tests(nui-sftp-ftp-${variant}-test "${testPrefix}." "${environment}")
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

# Apple's clang has no libFuzzer, and the coverage configure measures the tests above alone.
if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND NOT WIN32 AND NOT APPLE AND NOT CODE_COVERAGE)
    add_subdirectory(fuzz)
endif()

# If msys2, copy dynamic libraries to executable directory, visual studio does this automatically.
# And there is no need on linux.
if (DEFINED ENV{MSYSTEM})
    add_custom_command(TARGET nui-sftp-ftp-test POST_BUILD
        COMMAND bash -c "ldd $<TARGET_FILE:nui-sftp-ftp-test>" | "grep" "clang" | awk "NF == 4 { system(\"${CMAKE_COMMAND} -E copy \" \$3 \" $<TARGET_FILE_DIR:nui-sftp-ftp-test>\") }"
        VERBATIM
    )
endif()
