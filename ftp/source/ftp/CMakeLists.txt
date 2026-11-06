# The ftp tests build copies instrumented with the sanitizers, linked to an instrumented remote_io.
function(add_file_transfer_protocol_library name remoteIo)
    set(FILE_TRANSFER_PROTOCOL_SOURCE_DIRECTORY "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
    add_library(
        ${name}
        STATIC
            ${FILE_TRANSFER_PROTOCOL_SOURCE_DIRECTORY}/protocol/reply_code.cpp
    )

    target_include_directories(
        ${name}
        PUBLIC
            ${FILE_TRANSFER_PROTOCOL_SOURCE_DIRECTORY}/../../include
    )

    target_link_libraries(${name} PUBLIC ${remoteIo})
endfunction()

add_file_transfer_protocol_library(file-transfer-protocol remote-io)
