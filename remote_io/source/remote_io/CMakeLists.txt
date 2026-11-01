find_package(fmt CONFIG REQUIRED)

# The remote_io and ssh tests build copies instrumented with ThreadSanitizer.
function(add_remote_io_library name)
    set(REMOTE_IO_SOURCE_DIRECTORY "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
    # Imported targets are only visible in the directory that found them, the test directories call this too.
    find_package(fmt CONFIG REQUIRED)
    add_library(
        ${name}
        STATIC
            ${REMOTE_IO_SOURCE_DIRECTORY}/async/buffer_provider.cpp
            ${REMOTE_IO_SOURCE_DIRECTORY}/async/processing_thread.cpp
            ${REMOTE_IO_SOURCE_DIRECTORY}/async/socket_wait.cpp
            ${REMOTE_IO_SOURCE_DIRECTORY}/async/transfer_context.cpp
    )

    target_include_directories(
        ${name}
        PUBLIC
            ${REMOTE_IO_SOURCE_DIRECTORY}/../../include
    )

    target_link_libraries(${name} PUBLIC utility PRIVATE fmt::fmt)
    if (WIN32)
        target_link_libraries(${name} PUBLIC ws2_32)
    endif()
endfunction()

add_remote_io_library(remote-io)
