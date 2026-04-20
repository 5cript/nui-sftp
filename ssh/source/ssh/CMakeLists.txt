add_library(
    secure-shell
    STATIC
        async/buffer_provider.cpp
        async/processing_thread.cpp
        async/transfer_context.cpp
        session.cpp
        channel.cpp
        sftp_session.cpp
        file_stream.cpp
        file_information.cpp
)

target_include_directories(
    secure-shell
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

target_compile_definitions(secure-shell PRIVATE -DSSH_NO_CPP_EXCEPTIONS)

find_package(LIBSSH REQUIRED)
find_package(fmt CONFIG REQUIRED)

target_link_libraries(
    secure-shell
    PUBLIC
        ssh
        ids
        persistence
        fmt::fmt
        shared-data
)