add_library(
    backend
    STATIC
        password/password_prompter.cpp
        process/process.cpp
        process/process_store.cpp
        process/environment.cpp
        session_manager.cpp
        session.cpp
        rpc_filesystem.cpp
        sftp/operation_queue.cpp
        sftp/download_operation.cpp
        sftp/upload_operation.cpp
        sftp/scan_operation.cpp
        sftp/bulk_download_operation.cpp
)

if (WIN32)
    target_sources(
        backend
        PRIVATE
            pty/windows/conpty.cpp
            pty/windows/pipe.cpp
    )
else()
    target_sources(
        backend
        PRIVATE
            pty/linux/pty.cpp
    )
endif()

target_sources(
    ${PROJECT_NAME}
    PRIVATE
        main.cpp
)

find_package(Boost CONFIG 1.86.0 REQUIRED COMPONENTS system filesystem asio process)

target_include_directories(backend PUBLIC "${CMAKE_SOURCE_DIR}/backend/include")

target_compile_definitions(backend PUBLIC -DSSH_NO_CPP_EXCEPTIONS)

include("${CMAKE_CURRENT_LIST_DIR}/../../../_cmake/dependencies/libssh.cmake")

# Link backend of nui outside of emscripten
target_link_libraries(
    backend
    PUBLIC
        nui-backend
        mplex
        efsw-static
        Boost::filesystem
        Boost::asio
        Boost::system
        Boost::process
        roar-include-only
        shared-data
        utility
        log
        persistence
        secure-shell
        ids
        events
)

target_link_libraries(
    ${PROJECT_NAME}
    PRIVATE
        backend
)

nui_set_target_output_directories(${PROJECT_NAME})

add_dependencies(backend scp-resource-copy)

# Creates a target that is compiled through emscripten. This target becomes the frontend part.
nui_add_emscripten_target(
    TARGET
        ${PROJECT_NAME}
    ENABLE_TAILWIND
    PREJS
        ${PREJS_FILE}
    SOURCE_DIR
        ${CMAKE_SOURCE_DIR}
    CMAKE_OPTIONS
        # I recommend to work with a release build by default because debug builds get big fast.
        -DCMAKE_BUILD_TYPE=Release
        -DNUI_FETCH_TRAITS=Off
)