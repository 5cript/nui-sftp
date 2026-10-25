include("${CMAKE_CURRENT_LIST_DIR}/../../../_cmake/dependencies/libssh.cmake")
find_package(fmt CONFIG REQUIRED)

# The ssh tests build a second copy instrumented with ThreadSanitizer.
function(add_secure_shell_library name)
    set(SECURE_SHELL_SOURCE_DIRECTORY "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
    add_library(
        ${name}
        STATIC
            ${SECURE_SHELL_SOURCE_DIRECTORY}/async/buffer_provider.cpp
            ${SECURE_SHELL_SOURCE_DIRECTORY}/async/processing_thread.cpp
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
            ids
            persistence
            utility
            fmt::fmt
            shared-data
    )
endfunction()

add_secure_shell_library(secure-shell)
