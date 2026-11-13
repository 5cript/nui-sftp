# The ftp tests build copies instrumented with the sanitizers, linked to an instrumented remote_io.
function(add_file_transfer_protocol_library name remoteIo)
    set(FILE_TRANSFER_PROTOCOL_SOURCE_DIRECTORY "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
    # Imported targets are only visible in the directory that found them, the test directory calls this too.
    find_package(fmt CONFIG REQUIRED)
    find_package(Boost CONFIG REQUIRED)
    find_package(OpenSSL REQUIRED COMPONENTS SSL Crypto)
    find_package(Threads REQUIRED)
    add_library(
        ${name}
        STATIC
            ${FILE_TRANSFER_PROTOCOL_SOURCE_DIRECTORY}/client/control_connection.cpp
            ${FILE_TRANSFER_PROTOCOL_SOURCE_DIRECTORY}/client/data_connection.cpp
            ${FILE_TRANSFER_PROTOCOL_SOURCE_DIRECTORY}/client/login.cpp
            ${FILE_TRANSFER_PROTOCOL_SOURCE_DIRECTORY}/client/metadata.cpp
            ${FILE_TRANSFER_PROTOCOL_SOURCE_DIRECTORY}/client/pending_command.cpp
            ${FILE_TRANSFER_PROTOCOL_SOURCE_DIRECTORY}/client/socket_options.cpp
            ${FILE_TRANSFER_PROTOCOL_SOURCE_DIRECTORY}/client/tls.cpp
            ${FILE_TRANSFER_PROTOCOL_SOURCE_DIRECTORY}/client/transfer.cpp
            ${FILE_TRANSFER_PROTOCOL_SOURCE_DIRECTORY}/engine/call.cpp
            ${FILE_TRANSFER_PROTOCOL_SOURCE_DIRECTORY}/engine/delivery_ring.cpp
            ${FILE_TRANSFER_PROTOCOL_SOURCE_DIRECTORY}/engine/engine.cpp
            ${FILE_TRANSFER_PROTOCOL_SOURCE_DIRECTORY}/engine/resolver.cpp
            ${FILE_TRANSFER_PROTOCOL_SOURCE_DIRECTORY}/engine/ring.cpp
            ${FILE_TRANSFER_PROTOCOL_SOURCE_DIRECTORY}/engine/supply_ring.cpp
            ${FILE_TRANSFER_PROTOCOL_SOURCE_DIRECTORY}/engine/task_gate.cpp
            ${FILE_TRANSFER_PROTOCOL_SOURCE_DIRECTORY}/engine/work_counter.cpp
            ${FILE_TRANSFER_PROTOCOL_SOURCE_DIRECTORY}/pool/lease.cpp
            ${FILE_TRANSFER_PROTOCOL_SOURCE_DIRECTORY}/pool/pool.cpp
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
            ${FILE_TRANSFER_PROTOCOL_SOURCE_DIRECTORY}/remote/file_system_operations.cpp
            ${FILE_TRANSFER_PROTOCOL_SOURCE_DIRECTORY}/remote/ftp_file_stream.cpp
            ${FILE_TRANSFER_PROTOCOL_SOURCE_DIRECTORY}/remote/ftp_file_system.cpp
            ${FILE_TRANSFER_PROTOCOL_SOURCE_DIRECTORY}/remote/running_operations.cpp
    )

    target_include_directories(
        ${name}
        PUBLIC
            ${FILE_TRANSFER_PROTOCOL_SOURCE_DIRECTORY}/../../include
    )

    target_link_libraries(
        ${name}
        PUBLIC
            ${remoteIo}
            fmt::fmt
            Boost::headers
            OpenSSL::SSL
            OpenSSL::Crypto
            Threads::Threads
        PRIVATE
            log
    )
    if (WIN32)
        target_link_libraries(${name} PUBLIC ws2_32 mswsock)
    endif()
endfunction()

add_file_transfer_protocol_library(file-transfer-protocol remote-io)
