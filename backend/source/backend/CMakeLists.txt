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
        rpc_system.cpp
        sftp/operation_queue.cpp
        sftp/download_operation.cpp
        sftp/upload_operation.cpp
        sftp/scan_operation.cpp
        sftp/local_scan_operation.cpp
        sftp/bulk_download_operation.cpp
        sftp/bulk_upload_operation.cpp
        sftp/delete_operation.cpp
)

if (WIN32)
    target_sources(
        backend
        PRIVATE
            pty/windows/conpty.cpp
            pty/windows/pipe.cpp
            windows/windows_drag_drop.cpp
            windows/main_windows.cpp
    )
else()
    target_sources(
        backend
        PRIVATE
            pty/linux/pty.cpp
            linux/main_linux.cpp
    )
endif()

target_sources(
    ${PROJECT_NAME}
    PRIVATE
        main.cpp
)

find_package(Boost CONFIG 1.86.0 REQUIRED COMPONENTS filesystem process)

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
        Boost::process
        roar-include-only
        shared-data
        utility
        log
        persistence
        secure-shell
        ids
        events
        utf8cpp
)

target_link_libraries(
    ${PROJECT_NAME}
    PRIVATE
        backend
)

nui_set_target_output_directories(${PROJECT_NAME})

add_dependencies(backend scp-resource-copy)

set(BROWSER_ENGINE "")
if (WIN32)
    set(BROWSER_ENGINE "webview2")
elseif(APPLE)
    set(BROWSER_ENGINE "webkit")
else()
    set(BROWSER_ENGINE "webkitgtk")
endif()

# Creates a target that is compiled through emscripten. This target becomes the frontend part.
nui_add_emscripten_target(
    TARGET
        ${PROJECT_NAME}
    PREJS
        ${PREJS_FILE}
    SOURCE_DIR
        ${CMAKE_SOURCE_DIR}
    CMAKE_OPTIONS
        # I recommend to work with a release build by default because debug builds get big fast.
        #-DCMAKE_BUILD_TYPE=Debug
        -DCMAKE_BUILD_TYPE=Release
        -DNUI_FETCH_TRAITS=Off
        -DBROWSER_ENGINE=${BROWSER_ENGINE}
        -DXML_TO_NUI_TOOL=$<TARGET_FILE:xml-to-nui>
)

add_dependencies(${PROJECT_NAME}-emscripten xml-to-nui)