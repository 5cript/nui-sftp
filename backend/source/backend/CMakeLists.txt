add_library(
    backend
    STATIC
        session_manager.cpp
        session.cpp
        rpc_filesystem.cpp
        local_delete_guard.cpp
        local_removal.cpp
        local_operation_queue.cpp
        existence_check.cpp
        rpc_path_parameters.cpp
        rpc_system.cpp
        theme_finder.cpp
        dropped_entry.cpp
        opener_risk.cpp
        program_options.cpp
        inspector_environment.cpp
        password/password_prompter.cpp
        password/password_request.cpp
        process/process.cpp
        process/process_store.cpp
        process/environment.cpp
        file_tracking/batching_remote.cpp
        file_tracking/change_filter.cpp
        file_tracking/change_reporter.cpp
        file_tracking/directory_watch_service.cpp
        file_tracking/directory_watcher.cpp
        file_tracking/disk_place.cpp
        file_tracking/event_spelling.cpp
        file_tracking/file_states.cpp
        file_tracking/local_item_watch.cpp
        file_tracking/local_watch_manager.cpp
        file_tracking/settled_changes.cpp
        file_tracking/instance_bookkeeping.cpp
        file_tracking/instance_lock.cpp
        file_tracking/instance_watch.cpp
        file_tracking/temp_dir_instancing.cpp
        file_tracking/temp_dir_instance_manager.cpp
        file_tracking/tracker.cpp
        sftp/operation_queue.cpp
        sftp/download_operation.cpp
        sftp/upload_operation.cpp
        sftp/resume_check.cpp
        sftp/scan_operation.cpp
        sftp/local_scan_operation.cpp
        sftp/bulk_transfer_operation.cpp
        sftp/bulk_download_operation.cpp
        sftp/bulk_upload_operation.cpp
        sftp/bulk_resume_registry.cpp
        sftp/compared_state_check.cpp
        sftp/delete_operation.cpp
        sftp/local_delete_operation.cpp
        sftp/remote_delete_guard.cpp
        sftp/rename_operation.cpp
        sftp/archive_download_operation.cpp
        sftp/archive_upload_operation.cpp
        sync/sync_session.cpp
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
            windows/browser_engine_windows.cpp
    )
elseif(APPLE)
    enable_language(OBJCXX)
    target_sources(
        backend
        PRIVATE
            file_tracking/instance_lock_nix.cpp
            pty/posix/pty.cpp
            pty/posix/process_list.cpp
            mac/opener_mac.mm
            process/fork_pool.cpp
    )
    set_source_files_properties(
        mac/main_mac.mm
        mac/opener_mac.mm
        mac/web_view_hooks.mm
        mac/browser_engine_mac.mm
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc"
    )
    target_link_libraries(
        backend
        PRIVATE
            "-framework AppKit"
            "-framework UniformTypeIdentifiers"
    )
    if (NOT NUI_BROWSER_ENGINE STREQUAL "cef")
        target_sources(backend PRIVATE mac/main_mac.mm mac/web_view_hooks.mm mac/browser_engine_mac.mm)
        target_link_libraries(backend PRIVATE "-framework WebKit")
    endif()
else()
    target_sources(
        backend
        PRIVATE
            file_tracking/instance_lock_nix.cpp
            pty/posix/pty.cpp
            pty/posix/process_list.cpp
            linux/opener_linux.cpp
            process/fork_pool.cpp
    )
    if (NOT NUI_BROWSER_ENGINE STREQUAL "cef")
        target_sources(
            backend
            PRIVATE
                linux/main_linux.cpp
                linux/gtk_portal_parent_window.cpp
                linux/browser_engine_linux.cpp
        )
    endif()
    # GLib's GDBus replaces sd-bus for portal calls: Ubuntu 24.04's xdg-dbus-proxy
    # mangles sd-bus's one-shot AUTH EXTERNAL handshake but accepts GDBus's multi-step
    # negotiation, so the app can talk to portals without --socket=session-bus.
    # gio-unix-2.0 brings in GUnixFDList for file-descriptor passing.
    # Public: main.cpp names the app with g_set_prgname. WebKitGTK brought GLib along, CEF does not.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(gio REQUIRED IMPORTED_TARGET gio-2.0 gio-unix-2.0)
    target_link_libraries(
        backend
        PUBLIC
            PkgConfig::gio
    )
endif()

# Chromium on Linux and macOS: the hooks of the main window.
if (NUI_BROWSER_ENGINE STREQUAL "cef")
    target_sources(backend PRIVATE cef/main_cef.cpp cef/cef_hooks.cpp cef/browser_engine_cef.cpp)
    target_link_libraries(backend PUBLIC Nui::cef)
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
        command-store
)

target_link_libraries(
    ${PROJECT_NAME}
    PRIVATE
        backend
)

nui_set_target_output_directories(${PROJECT_NAME})

# The frontend is compiled per engine and per platform, the macOS frontend also runs in Chromium.
if (NUI_BROWSER_ENGINE STREQUAL "cef")
    set(BROWSER_ENGINE "cef")
elseif (WIN32)
    set(BROWSER_ENGINE "webview2")
elseif (APPLE)
    set(BROWSER_ENGINE "wkwebview")
else()
    set(BROWSER_ENGINE "webkitgtk")
endif()
if (WIN32)
    set(NUI_SFTP_PLATFORM "windows")
elseif (APPLE)
    set(NUI_SFTP_PLATFORM "macos")
else()
    set(NUI_SFTP_PLATFORM "linux")
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
            -DNUI_SFTP_PLATFORM=${NUI_SFTP_PLATFORM}
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