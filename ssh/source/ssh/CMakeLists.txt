find_package(fmt CONFIG REQUIRED)

# The ssh tests build a second copy instrumented with ThreadSanitizer, linked to an instrumented remote_io.
function(add_secure_shell_library name remoteIo)
    set(SECURE_SHELL_SOURCE_DIRECTORY "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
    # Imported targets are only visible in the directory that found them, the test directory calls this too.
    include("${SECURE_SHELL_SOURCE_DIRECTORY}/../../../_cmake/dependencies/libssh.cmake")
    add_library(
        ${name}
        STATIC
            ${SECURE_SHELL_SOURCE_DIRECTORY}/async/transfer_context.cpp
            ${SECURE_SHELL_SOURCE_DIRECTORY}/session.cpp
            ${SECURE_SHELL_SOURCE_DIRECTORY}/channel.cpp
            ${SECURE_SHELL_SOURCE_DIRECTORY}/sftp_session.cpp
            ${SECURE_SHELL_SOURCE_DIRECTORY}/file_stream.cpp
            ${SECURE_SHELL_SOURCE_DIRECTORY}/file_information.cpp
            ${SECURE_SHELL_SOURCE_DIRECTORY}/key_file.cpp
    )

    target_include_directories(
        ${name}
        PUBLIC
            ${SECURE_SHELL_SOURCE_DIRECTORY}/../../include
    )

    target_compile_definitions(${name} PRIVATE -DSSH_NO_CPP_EXCEPTIONS)

    target_link_libraries(
        ${name}
        PUBLIC
            ssh
            ${remoteIo}
            ids
            persistence
            utility
            fmt::fmt
            shared-data
    )
    if (WIN32)
        target_link_libraries(${name} PUBLIC ws2_32)
    endif()
endfunction()

add_secure_shell_library(secure-shell remote-io)
