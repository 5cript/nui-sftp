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

option(NUI_SFTP_FTP_FUZZ "Let ctest fuzz each FTP fuzz target for NUI_SFTP_FTP_FUZZ_SECONDS" OFF)
set(NUI_SFTP_FTP_FUZZ_SECONDS 30 CACHE STRING "How long ctest fuzzes each FTP fuzz target with NUI_SFTP_FTP_FUZZ")

set(fuzzSanitizers -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
add_file_transfer_protocol_library(file-transfer-protocol-fuzz remote-io-asan)
target_compile_options(file-transfer-protocol-fuzz PRIVATE -fsanitize=fuzzer-no-link ${fuzzSanitizers})

# ctest runs every seed in seedDirectories once through each target, with the same result each time. Fuzzing finds new
# inputs by chance, so a retry of a failed run can pass: only NUI_SFTP_FTP_FUZZ adds it, for NUI_SFTP_FTP_FUZZ_SECONDS
# under the label fuzz. New inputs go to a corpus in the build tree, a crashing input to test/fuzz/<target>-crash-*. A
# crash becomes a unit test and a seed.
function(add_ftp_fuzz_target name testName seedDirectories)
    set(fuzzSeeds "")
    foreach(seedDirectory IN LISTS seedDirectories)
        file(GLOB seeds CONFIGURE_DEPENDS "${seedDirectory}/*")
        list(APPEND fuzzSeeds ${seeds})
    endforeach()

    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE file-transfer-protocol-fuzz)
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer ${fuzzSanitizers})
    target_link_options(${name} PRIVATE -fsanitize=fuzzer ${fuzzSanitizers})
    set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test/fuzz")

    set(artifactPrefix "${CMAKE_BINARY_DIR}/test/fuzz/${name}-")
    add_test(NAME FtpFuzzSeeds.${testName} COMMAND ${name} -artifact_prefix=${artifactPrefix} ${fuzzSeeds})

    if (NUI_SFTP_FTP_FUZZ)
        set(corpus "${CMAKE_BINARY_DIR}/test/fuzz/${name}-corpus")
        file(MAKE_DIRECTORY "${corpus}")
        add_test(
            NAME FtpFuzz.${testName}
            COMMAND
                ${name}
                -max_total_time=${NUI_SFTP_FTP_FUZZ_SECONDS}
                -artifact_prefix=${artifactPrefix}
                "${corpus}"
                ${seedDirectories}
        )
        set_tests_properties(FtpFuzz.${testName} PROPERTIES LABELS fuzz)
    endif()
endfunction()

set(replySeeds "${CMAKE_CURRENT_SOURCE_DIR}/seeds")
set(listingSeeds "${CMAKE_CURRENT_SOURCE_DIR}/seeds;${CMAKE_CURRENT_SOURCE_DIR}/../samples/listings")
add_ftp_fuzz_target(nui-sftp-ftp-fuzz-reply-framer ReplyFramer "${replySeeds}" fuzz_reply_framer.cpp)
add_ftp_fuzz_target(nui-sftp-ftp-fuzz-reply-parsers ReplyParsers "${replySeeds}" fuzz_reply_parsers.cpp)
add_ftp_fuzz_target(nui-sftp-ftp-fuzz-listing Listing "${listingSeeds}" fuzz_listing.cpp)
