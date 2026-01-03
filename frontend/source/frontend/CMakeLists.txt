set(NUISCP_NPM_INSTALL_ARGS "" CACHE STRING "Arguments to pass to npm install for nui-scp")

target_sources(
    ${PROJECT_NAME}
    PRIVATE
        main.cpp
        main_page.cpp
        sidebar.cpp
        toolbar.cpp
        session_area.cpp
        session.cpp
        dialog/input_dialog.cpp
        dialog/password_prompter.cpp
        dialog/confirm_dialog.cpp
        session_components/session_options.cpp
        session_components/operation_queue.cpp
        terminal/frontend_ssh_manager.cpp
        terminal/executing_engine.cpp
        terminal/sftp_file_engine.cpp
        terminal/ssh_engine.cpp
        terminal/user_control_engine.cpp
        terminal/ssh_channel.cpp
        components/progress_bar.cpp
        file_explorer/side_model.cpp
        file_explorer/remote_side_model.cpp
        file_explorer/local_side_model.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/frontend/include)

# Link frontend of nui inside of emscripten
target_link_libraries(
    ${PROJECT_NAME}
    PRIVATE
        core-target
        nui-frontend
        nui-file-explorer
        ui5
        roar-include-only
        shared-data
        utility
        log
        persistence
        nlohmann_json::nlohmann_json
        events
        ids
)

function(set_memory64_flags)
    cmake_parse_arguments(
        MEM64
        ""
        "TARGETS"
        ""
        ${ARGN}
    )
    foreach(target IN LISTS MEM64_TARGETS)
        set_target_properties(
            ${target}
            PROPERTIES
                LINK_FLAGS "-sMEMORY64=1"
                COMPILE_FLAGS "-sMEMORY64=1"
        )
    endforeach()
endfunction()

set_memory64_flags(TARGETS nui-frontend nlohmann_json::nlohmann_json)

nui_prepare_emscripten_target(
    TARGET
        ${PROJECT_NAME}
    PARCEL_ARGS
        --no-optimize
    PREJS
        ${PREJS_FILE}
    LEAN_INDEX_HTML
    NPM_INSTALL_ARGS
        ${NUISCP_NPM_INSTALL_ARGS}
    EMSCRIPTEN_LINK_OPTIONS
        -sEXPORTED_FUNCTIONS=_frontendMain
        -sALLOW_MEMORY_GROWTH=1
        -sMEMORY64=1
        -sWASM_BIGINT
        -fexceptions
        # -O2
        -g
        # or other options, see emscripten docs.
    EMSCRIPTEN_COMPILE_OPTIONS
        # -O3
        -sMEMORY64=1
        -g
    CXX_STANDARD
        23
    OBSERVED_BUNDLE_FILES
        "${CMAKE_SOURCE_DIR}/static/source/content_panels/file_explorer.ts"
        "${CMAKE_SOURCE_DIR}/static/source/content_panels/terminal.ts"
        "${CMAKE_SOURCE_DIR}/static/source/content_panel_manager.ts"
        "${CMAKE_SOURCE_DIR}/static/source/index.js"
        "${CMAKE_SOURCE_DIR}/static/styles/main_page.css"
        "${CMAKE_SOURCE_DIR}/static/styles/main.css"
        "${CMAKE_SOURCE_DIR}/static/styles/session_area.css"
        "${CMAKE_SOURCE_DIR}/static/styles/session.css"
        "${CMAKE_SOURCE_DIR}/static/styles/sidebar.css"
        "${CMAKE_SOURCE_DIR}/static/styles/toolbar.css"
        "${CMAKE_SOURCE_DIR}/static/styles/images/caretdown.png"
        "${CMAKE_SOURCE_DIR}/static/styles/images/caretleft.png"
        "${CMAKE_SOURCE_DIR}/static/styles/images/caretright.png"
        "${CMAKE_SOURCE_DIR}/static/styles/images/caretup.png"
        "${CMAKE_SOURCE_DIR}/static/styles/lumino/accordionpanel.css"
        "${CMAKE_SOURCE_DIR}/static/styles/lumino/commandpalette.css"
        "${CMAKE_SOURCE_DIR}/static/styles/lumino/datagrid.css"
        "${CMAKE_SOURCE_DIR}/static/styles/lumino/dockpanel.css"
        "${CMAKE_SOURCE_DIR}/static/styles/lumino/dragdrop/index.css"
        "${CMAKE_SOURCE_DIR}/static/styles/lumino/dragdrop"
        "${CMAKE_SOURCE_DIR}/static/styles/lumino/index.css"
        "${CMAKE_SOURCE_DIR}/static/styles/lumino/menu.css"
        "${CMAKE_SOURCE_DIR}/static/styles/lumino/menubar.css"
        "${CMAKE_SOURCE_DIR}/static/styles/lumino/scrollbar.css"
        "${CMAKE_SOURCE_DIR}/static/styles/lumino/tabbar.css"
        "${CMAKE_SOURCE_DIR}/static/styles/lumino/widgets/accordionpanel.css"
        "${CMAKE_SOURCE_DIR}/static/styles/lumino/widgets/commandpalette.css"
        "${CMAKE_SOURCE_DIR}/static/styles/lumino/widgets/dockpanel.css"
        "${CMAKE_SOURCE_DIR}/static/styles/lumino/widgets/index.css"
        "${CMAKE_SOURCE_DIR}/static/styles/lumino/widgets/menu.css"
        "${CMAKE_SOURCE_DIR}/static/styles/lumino/widgets/menubar.css"
        "${CMAKE_SOURCE_DIR}/static/styles/lumino/widgets/scrollbar.css"
        "${CMAKE_SOURCE_DIR}/static/styles/lumino/widgets/splitpanel.css"
        "${CMAKE_SOURCE_DIR}/static/styles/lumino/widgets/tabbar.css"
        "${CMAKE_SOURCE_DIR}/static/styles/lumino/widgets/tabpanel.css"
        "${CMAKE_SOURCE_DIR}/static/styles/lumino/widgets/widget.css"
        "${CMAKE_SOURCE_DIR}/static/styles/lumino/widgets"
        "${CMAKE_SOURCE_DIR}/static/styles/lumino"
)

add_dependencies(theme_compilation ${PROJECT_NAME}-npm-install)
add_dependencies(${PROJECT_NAME}-parcel theme_compilation)