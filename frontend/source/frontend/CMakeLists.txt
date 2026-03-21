set(NUISCP_NPM_INSTALL_ARGS "" CACHE STRING "Arguments to pass to npm install for nui-sftp")

add_npm_install(
    TARGET_NAME ${PROJECT_NAME}-npm-install
    NPM_EXECUTABLE "${NUI_NPM}"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    INSTALL_ARGS ${NUISCP_NPM_INSTALL_ARGS}
)

target_sources(
    ${PROJECT_NAME}
    PRIVATE
        main.cpp
        main_page.cpp
        sidebar.cpp
        toolbar.cpp
        session_area.cpp
        session.cpp
        settings.cpp
        color.cpp
        icon_from_name.cpp
        dialog/input_dialog.cpp
        dialog/multi_input_dialog.cpp
        dialog/password_prompter.cpp
        dialog/confirm_dialog.cpp
        dialog/new_session_dialog.cpp
        dialog/file_property_dialog.cpp
        dialog/dialog_buttons_keyboard_support.cpp
        settings/general_settings.cpp
        settings/optional_converters.cpp
        settings/sftp_options.cpp
        settings/ssh_options.cpp
        settings/setting_group.cpp
        settings/termios_settings.cpp
        settings/terminal_options.cpp
        settings/queue_options.cpp
        settings/executing_session_options.cpp
        settings/ssh_session_options.cpp
        settings/session_options.cpp
        settings/subgroup.cpp
        settings/layout_setting.cpp
        settings/log_options.cpp
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

function(set_memory64_flags)
    cmake_parse_arguments(
        MEM64
        ""
        "TARGETS"
        ""
        ${ARGN}
    )
    foreach(target IN LISTS MEM64_TARGETS)
        if (WIN32)
            set_target_properties(
                ${target}
                PROPERTIES
                    LINK_FLAGS "-sMEMORY64=1"
                    COMPILE_FLAGS "-sMEMORY64=1"
            )
        else()
            # No support for WASM64 in webkit
            set_target_properties(
                ${target}
                PROPERTIES
                    LINK_FLAGS "-sWASM_BIGINT=1"
            )
        endif()
    endforeach()
endfunction()

set_memory64_flags(TARGETS nui-frontend nlohmann_json::nlohmann_json script-nui-components fmt::fmt)

set(DEPENDANT_LINK_OPTIONS "")
set(DEPENDANT_COMPILE_OPTIONS "")
set(PARCEL_ARGS "")
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(DEPENDANT_LINK_OPTIONS "-g1")
    set(DEPENDANT_COMPILE_OPTIONS "-g1")
    set(PARCEL_ARGS "--no-optimize")
else()
    set(DEPENDANT_LINK_OPTIONS "-O2")
    set(DEPENDANT_COMPILE_OPTIONS "-O2")
    set(PARCEL_ARGS "--no-source-maps")
endif()

nui_prepare_emscripten_target(
    NO_NPM_INSTALL
    TARGET
        ${PROJECT_NAME}
    PARCEL_ARGS
        ${PARCEL_ARGS}
    PREJS
        ${PREJS_FILE}
    LEAN_INDEX_HTML_NO_INDEX_JS
    NPM_INSTALL_ARGS
        ${NUISCP_NPM_INSTALL_ARGS}
    EMSCRIPTEN_LINK_OPTIONS
        -sEXPORTED_FUNCTIONS=_frontendMain
        -sALLOW_MEMORY_GROWTH=1
        -sWASM_BIGINT
        -fexceptions
        ${DEPENDANT_LINK_OPTIONS}
        # or other options, see emscripten docs.
    EMSCRIPTEN_COMPILE_OPTIONS
        -fexceptions
        ${DEPENDANT_COMPILE_OPTIONS}
    CXX_STANDARD
        23
    OBSERVED_BUNDLE_FILES
        "${CMAKE_SOURCE_DIR}/static/index.html"
        "${CMAKE_SOURCE_DIR}/static/source/content_panels/file_explorer.ts"
        "${CMAKE_SOURCE_DIR}/static/source/content_panels/terminal.ts"
        "${CMAKE_SOURCE_DIR}/static/source/content_panel_manager.ts"
        "${CMAKE_SOURCE_DIR}/static/source/index.js"
        "${CMAKE_SOURCE_DIR}/static/styles/icon_panel.css"
        "${CMAKE_SOURCE_DIR}/static/styles/main_page.css"
        "${CMAKE_SOURCE_DIR}/static/styles/main.css"
        "${CMAKE_SOURCE_DIR}/static/styles/dialog/file_property_dialog.css"
        "${CMAKE_SOURCE_DIR}/static/styles/dialog/multi_input_dialog.css"
        "${CMAKE_SOURCE_DIR}/static/styles/dialog/new_session_dialog.css"
        "${CMAKE_SOURCE_DIR}/static/styles/dialog/confirm_dialog.css"
        "${CMAKE_SOURCE_DIR}/static/styles/dialog/input_dialog.css"
        "${CMAKE_SOURCE_DIR}/static/styles/operation_queue.css"
        "${CMAKE_SOURCE_DIR}/static/styles/session_area.css"
        "${CMAKE_SOURCE_DIR}/static/styles/session.css"
        "${CMAKE_SOURCE_DIR}/static/styles/settings_group.css"
        "${CMAKE_SOURCE_DIR}/static/styles/settings.css"
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
        # 5cript-nui-components
        "${CMAKE_SOURCE_DIR}/dependencies/5cript-nui-components/styles/button.css"
        "${CMAKE_SOURCE_DIR}/dependencies/5cript-nui-components/styles/color_picker.css"
        "${CMAKE_SOURCE_DIR}/dependencies/5cript-nui-components/styles/dialog.css"
        "${CMAKE_SOURCE_DIR}/dependencies/5cript-nui-components/styles/popup_menu.css"
        "${CMAKE_SOURCE_DIR}/dependencies/5cript-nui-components/styles/resizeable_table.css"
        "${CMAKE_SOURCE_DIR}/dependencies/5cript-nui-components/styles/select.css"
        "${CMAKE_SOURCE_DIR}/dependencies/5cript-nui-components/styles/switch.css"
        "${CMAKE_SOURCE_DIR}/dependencies/5cript-nui-components/styles/tabs.css"
        "${CMAKE_SOURCE_DIR}/dependencies/5cript-nui-components/styles/text_input.css"
)

target_include_directories(
    ${PROJECT_NAME}
    PRIVATE
        "${CMAKE_SOURCE_DIR}/frontend/include"
        "${CMAKE_BINARY_DIR}/include"
)

target_compile_definitions(
    script-nui-components
    PUBLIC -DSCRIPT_NUI_COMPONENTS_NO_INLINE
)

# Link frontend of nui inside of emscripten
target_link_libraries(
    ${PROJECT_NAME}
    PRIVATE
        core-target
        nui-frontend
        nui-file-explorer
        roar-include-only
        shared-data
        utility
        log
        persistence
        nlohmann_json::nlohmann_json
        events
        ids
        script-nui-components
)

target_compile_definitions(${PROJECT_NAME} PRIVATE -DBROWSER_ENGINE=${BROWSER_ENGINE})