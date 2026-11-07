# The ftp tests build copies instrumented with the sanitizers, linked to an instrumented remote_io.
function(add_file_transfer_protocol_library name remoteIo)
    set(FILE_TRANSFER_PROTOCOL_SOURCE_DIRECTORY "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
    # Imported targets are only visible in the directory that found them, the test directory calls this too.
    find_package(fmt CONFIG REQUIRED)
    add_library(
        ${name}
        STATIC
            ${FILE_TRANSFER_PROTOCOL_SOURCE_DIRECTORY}/protocol/command.cpp
            ${FILE_TRANSFER_PROTOCOL_SOURCE_DIRECTORY}/protocol/features.cpp
            ${FILE_TRANSFER_PROTOCOL_SOURCE_DIRECTORY}/protocol/list_line.cpp
            ${FILE_TRANSFER_PROTOCOL_SOURCE_DIRECTORY}/protocol/listing.cpp
            ${FILE_TRANSFER_PROTOCOL_SOURCE_DIRECTORY}/protocol/machine_listing_line.cpp
            ${FILE_TRANSFER_PROTOCOL_SOURCE_DIRECTORY}/protocol/protocol_error.cpp
            ${FILE_TRANSFER_PROTOCOL_SOURCE_DIRECTORY}/protocol/reply.cpp
            ${FILE_TRANSFER_PROTOCOL_SOURCE_DIRECTORY}/protocol/reply_code.cpp
            ${FILE_TRANSFER_PROTOCOL_SOURCE_DIRECTORY}/protocol/reply_error.cpp
            ${FILE_TRANSFER_PROTOCOL_SOURCE_DIRECTORY}/protocol/reply_parsers.cpp
    )

    target_include_directories(
        ${name}
        PUBLIC
            ${FILE_TRANSFER_PROTOCOL_SOURCE_DIRECTORY}/../../include
    )

    target_link_libraries(${name} PUBLIC ${remoteIo} fmt::fmt)
endfunction()

add_file_transfer_protocol_library(file-transfer-protocol remote-io)
