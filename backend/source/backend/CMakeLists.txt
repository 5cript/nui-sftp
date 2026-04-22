add_library(
    backend
    STATIC
        session_manager.cpp
        session.cpp
        rpc_filesystem.cpp
        rpc_system.cpp
        theme_finder.cpp
        program_options.cpp
        password/password_prompter.cpp
        process/process.cpp
        process/process_store.cpp
        process/environment.cpp
        file_tracking/instance_lock.cpp
        file_tracking/instance_watch.cpp
        file_tracking/temp_dir_instancing.cpp
        file_tracking/temp_dir_instance_manager.cpp
        sftp/operation_queue.cpp
        sftp/download_operation.cpp
        sftp/upload_operation.cpp
        sftp/scan_operation.cpp
        sftp/local_scan_operation.cpp
        sftp/bulk_download_operation.cpp
        sftp/bulk_upload_operation.cpp
        sftp/bulk_resume_registry.cpp
        sftp/delete_operation.cpp
        sftp/rename_operation.cpp
        sftp/archive_download_operation.cpp
        sftp/archive_upload_operation.cpp
        "${CMAKE_BINARY_DIR}/include/build_environment.hpp"
)

if (WIN32)
    target_sources(
        backend
        PRIVATE
            file_tracking/instance_lock_win.cpp
            pty/windows/conpty.cpp
            pty/windows/pipe.cpp
            windows/windows_drag_drop.cpp
            windows/main_windows.cpp
            windows/opener_windows.cpp
    )
else()
    target_sources(
        backend
        PRIVATE
            file_tracking/instance_lock_nix.cpp
            pty/linux/pty.cpp
            linux/main_linux.cpp
            linux/opener_linux.cpp
            process/fork_pool.cpp
    )
    # GLib's GDBus replaces sd-bus for portal calls: Ubuntu 24.04's xdg-dbus-proxy
    # mangles sd-bus's one-shot AUTH EXTERNAL handshake but accepts GDBus's multi-step
    # negotiation, so the app can talk to portals without --socket=session-bus.
    # gio-unix-2.0 brings in GUnixFDList for file-descriptor passing.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(gio REQUIRED IMPORTED_TARGET gio-2.0 gio-unix-2.0)
    target_link_libraries(
        backend
        PRIVATE
            PkgConfig::gio
    )
endif()

target_sources(
    ${PROJECT_NAME}
    PRIVATE
        main.cpp
)

find_package(Boost CONFIG 1.86.0 REQUIRED COMPONENTS filesystem process program_options)

target_include_directories(backend PUBLIC "${CMAKE_SOURCE_DIR}/backend/include")

target_compile_definitions(
    backend
    PUBLIC
        -DSSH_NO_CPP_EXCEPTIONS
)

include("${CMAKE_CURRENT_LIST_DIR}/../../../_cmake/dependencies/libssh.cmake")

# Link backend of nui outside of emscripten
target_link_libraries(
    backend
    PUBLIC
        nui-backend
        nui-file-explorer-support
        efsw-static
        Boost::filesystem
        Boost::process
        Boost::program_options
        roar-include-only
        shared-data
        utility
        log
        persistence
        secure-shell
        ids
        events
        tar-archive
)

target_link_libraries(
    ${PROJECT_NAME}
    PRIVATE
        backend
)

nui_set_target_output_directories(${PROJECT_NAME})

set(BROWSER_ENGINE "")
if (WIN32)
    set(BROWSER_ENGINE "webview2")
elseif(APPLE)
    set(BROWSER_ENGINE "webkit")
else()
    set(BROWSER_ENGINE "webkitgtk")
endif()

include(ProcessorCount)
ProcessorCount(nproc)

if (NOT OMIT_FRONTEND_BUILD)
    # Creates a target that is compiled through emscripten. This target becomes the frontend part.
    nui_add_emscripten_target(
        TARGET
            ${PROJECT_NAME}
        PREJS
            ${PREJS_FILE}
        SOURCE_DIR
            ${CMAKE_SOURCE_DIR}
        DISABLE_BIN2HPP
        CMAKE_OPTIONS
            -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
            -DNUI_FETCH_TRAITS=Off
            -DNUI_FETCH_BOOST_DESCRIBE=ON
            -DNUI_FETCH_BOOST_MP11=ON
            -DNUI_FETCH_BOOST_PREPROCESSOR=ON
            -DNUI_FETCH_NLOHMANN_JSON=ON
            -DNUI_FIND_NLOHMANN_JSON=OFF
            -DNUI_FETCH_INTERVAL_TREE=ON
            -DNUI_FETCH_FMT=ON
            -DNUI_FIND_FMT=OFF
            -DBROWSER_ENGINE=${BROWSER_ENGINE}
            # -DXML_TO_NUI_TOOL=$<TARGET_FILE:xml-to-nui>
            -DOFFLINE_BUILD=${OFFLINE_BUILD}
            -DBACKEND_BUILD_TYPE=${BACKEND_BUILD_TYPE}
        BUILD_OPTIONS
            -- -j${nproc}
    )

    target_sources(
        ${PROJECT_NAME}
        PRIVATE
            "${CMAKE_BINARY_DIR}/module_nui-sftp/bin/index.html"
    )

    # add_dependencies(${PROJECT_NAME}-emscripten xml-to-nui)
    add_dependencies(${PROJECT_NAME}-emscripten nui-sftp-style-copy)
endif()