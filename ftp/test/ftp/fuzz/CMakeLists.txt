include(CheckCXXSourceCompiles)

# Some clang packages ship without the libFuzzer runtime.
set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer")
check_cxx_source_compiles(
    "#include <cstddef>
    #include <cstdint>
    extern \"C\" int LLVMFuzzerTestOneInput(std::uint8_t const*, std::size_t) { return 0; }"
    NUI_SFTP_HAS_LIBFUZZER
)
unset(CMAKE_REQUIRED_FLAGS)
if (NOT NUI_SFTP_HAS_LIBFUZZER)
    message(STATUS "FTP fuzz targets skipped, clang has no libFuzzer")
    return()
endif()

set(NUI_SFTP_FTP_FUZZ_SECONDS 30 CACHE STRING "How long ctest runs each FTP fuzz target")

set(fuzzSanitizers -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
add_file_transfer_protocol_library(file-transfer-protocol-fuzz remote-io-asan)
target_compile_options(file-transfer-protocol-fuzz PRIVATE -fsanitize=fuzzer-no-link ${fuzzSanitizers})

# ctest runs each target for NUI_SFTP_FTP_FUZZ_SECONDS from the seeds, new inputs go to a corpus in the build tree and
# a crashing input to test/fuzz/<target>-crash-*. A crash becomes a unit test.
function(add_ftp_fuzz_target name testName)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE file-transfer-protocol-fuzz)
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer ${fuzzSanitizers})
    target_link_options(${name} PRIVATE -fsanitize=fuzzer ${fuzzSanitizers})
    set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test/fuzz")

    set(corpus "${CMAKE_BINARY_DIR}/test/fuzz/${name}-corpus")
    file(MAKE_DIRECTORY "${corpus}")
    add_test(
        NAME FtpFuzz.${testName}
        COMMAND
            ${name}
            -max_total_time=${NUI_SFTP_FTP_FUZZ_SECONDS}
            -artifact_prefix=${CMAKE_BINARY_DIR}/test/fuzz/${name}-
            "${corpus}"
            "${CMAKE_CURRENT_SOURCE_DIR}/seeds"
    )
    set_tests_properties(FtpFuzz.${testName} PROPERTIES LABELS fuzz)
endfunction()

add_ftp_fuzz_target(nui-sftp-ftp-fuzz-reply-framer ReplyFramer fuzz_reply_framer.cpp)
add_ftp_fuzz_target(nui-sftp-ftp-fuzz-reply-parsers ReplyParsers fuzz_reply_parsers.cpp)
