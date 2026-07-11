set(NUISCP_NPM_INSTALL_ARGS "" CACHE STRING "Arguments to pass to npm install for nui-sftp")

add_npm_install(
    TARGET_NAME ${PROJECT_NAME}-npm-install
    NPM_EXECUTABLE "${NUI_NPM}"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    INSTALL_ARGS ${NUISCP_NPM_INSTALL_ARGS}
)

# ── Embedded license bundle ────────────────────────────────────────────────
# Generates licenses_data.hpp at build time and embeds the merged C++ + npm
# license bundle as a std::string_view, so the WASM frontend can render the
# Licenses page without any runtime file lookup.

set(NUI_SFTP_LICENSES_GEN_DIR "${CMAKE_BINARY_DIR}/generated/licenses")
set(NUI_SFTP_NPM_LICENSES_JSON "${NUI_SFTP_LICENSES_GEN_DIR}/npm-licenses.json")
set(NUI_SFTP_LICENSES_JSON     "${NUI_SFTP_LICENSES_GEN_DIR}/licenses.json")
set(NUI_SFTP_LICENSES_HEADER   "${NUI_SFTP_LICENSES_GEN_DIR}/licenses_data.hpp")

find_package(Python3 COMPONENTS Interpreter REQUIRED)

# Glob the per-package license texts so the header re-generates whenever any
# of them is edited.
file(GLOB NUI_SFTP_LICENSE_TEXT_FILES
    CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/licenses/texts/*"
)

# Step 1: walk the build-tree node_modules and dump per-package license info.
add_custom_command(
    OUTPUT "${NUI_SFTP_NPM_LICENSES_JSON}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${NUI_SFTP_LICENSES_GEN_DIR}"
    COMMAND "${CMAKE_BINARY_DIR}/node_modules/.bin/license-checker-rseidelsohn"
        --start "${CMAKE_BINARY_DIR}"
        --production
        --json
        --out "${NUI_SFTP_NPM_LICENSES_JSON}"
    DEPENDS ${PROJECT_NAME}-npm-install
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "Collecting npm production licenses"
    VERBATIM
)

# Step 2: merge SPDX + npm output into the embedded C++ header. Re-runs when
# any source license input changes.
add_custom_command(
    OUTPUT "${NUI_SFTP_LICENSES_HEADER}" "${NUI_SFTP_LICENSES_JSON}"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/scripts/build_licenses_bundle.py"
        --out    "${NUI_SFTP_LICENSES_JSON}"
        --header "${NUI_SFTP_LICENSES_HEADER}"
        --npm-licenses  "${NUI_SFTP_NPM_LICENSES_JSON}"
        --npm-host-root "${CMAKE_BINARY_DIR}"
    DEPENDS
        "${NUI_SFTP_NPM_LICENSES_JSON}"
        "${CMAKE_SOURCE_DIR}/scripts/build_licenses_bundle.py"
        "${CMAKE_SOURCE_DIR}/licenses/third_party.spdx.json"
        ${NUI_SFTP_LICENSE_TEXT_FILES}
    COMMENT "Generating embedded license header"
    VERBATIM
)

add_custom_target(nui-sftp-licenses-data
    DEPENDS "${NUI_SFTP_LICENSES_HEADER}"
)

# Ensure the embedded header exists at configure time, so source files that
# `#include <licenses_data.hpp>` resolve before the build-time custom command
# has run. Whatever is here will be overwritten on the first build with the
# full cpp+npm bundle once npm install has produced a node_modules.
if (NOT EXISTS "${NUI_SFTP_LICENSES_HEADER}")
    file(MAKE_DIRECTORY "${NUI_SFTP_LICENSES_GEN_DIR}")
    execute_process(
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/scripts/build_licenses_bundle.py"
            --out    "${NUI_SFTP_LICENSES_JSON}"
            --header "${NUI_SFTP_LICENSES_HEADER}"
            --npm-licenses  "${NUI_SFTP_NPM_LICENSES_JSON}"
            --npm-host-root "${CMAKE_BINARY_DIR}"
        RESULT_VARIABLE NUI_SFTP_LICENSES_BOOTSTRAP_RESULT
    )
    if (NOT NUI_SFTP_LICENSES_BOOTSTRAP_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to bootstrap licenses_data.hpp at configure time")
    endif()
endif()

target_sources(
    ${PROJECT_NAME}
    PRIVATE
        main.cpp
        main_page.cpp
        sidebar.cpp
        toolbar.cpp
        session_area.cpp
        session.cpp
        proto_session.cpp
        settings.cpp
        licenses.cpp
        color.cpp
        icon_from_name.cpp
        theme_controller.cpp
        dialog/input_dialog.cpp
        dialog/multi_input_dialog.cpp
        dialog/password_prompter.cpp
        dialog/confirm_dialog.cpp
        dialog/new_session_dialog.cpp
        dialog/file_property_dialog.cpp
        dialog/direct_connect_dialog.cpp
        dialog/dialog_buttons_keyboard_support.cpp
        dialog/archive_transfer_dialog.cpp
        settings/general_settings.cpp
        settings/addressable_setting.cpp
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
        session/session_helpers.cpp
        command_store/command_store_client.cpp
        session_components/file_tracking.cpp
        session_components/session_options.cpp
        session_components/operation_queue.cpp
        session_components/terminal_panel.cpp
        session_components/file_explorer_panel.cpp
        session_components/connection_loss_overlay.cpp
        session_components/session_layout_initializer.cpp
        session_components/session_snapshot_manager.cpp
        sync_dialog/sync_dialog.cpp
        sync_dialog/sync_progress_dialog.cpp
        sync_dialog/backend_sync_provider.cpp
        terminal/terminal_channel.cpp
        terminal/frontend_session_manager.cpp
        terminal/executing_engine.cpp
        terminal/executing_channel.cpp
        terminal/file_engine.cpp
        terminal/ssh_engine.cpp
        terminal/ssh_channel.cpp
        components/progress_bar.cpp
        onboarding/onboarding.cpp
        file_explorer/side_model.cpp
        file_explorer/remote_side_model.cpp
        file_explorer/local_side_model.cpp
        "${CMAKE_BINARY_DIR}/include/build_environment.hpp"
        "${NUI_SFTP_LICENSES_HEADER}"
)

target_include_directories(${PROJECT_NAME} PRIVATE "${NUI_SFTP_LICENSES_GEN_DIR}")
add_dependencies(${PROJECT_NAME} nui-sftp-licenses-data)
# Nui's inline preprocessor compiles every .cpp into a .i before the WASM
# target builds, so it also needs the generated header to exist first.
if (TARGET nui-inline-${PROJECT_NAME})
    add_dependencies(nui-inline-${PROJECT_NAME} nui-sftp-licenses-data)
endif()

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
    # Emit index.wasm separately instead of base64-inlining it into index.js. Avoids V8 exploding
    # while parsing a multi-hundred-MB source string (previously caused STATUS_BREAKPOINT on load).
    UNPACKED_MODE on
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
        "${CMAKE_SOURCE_DIR}/nui-file-explorer/styles/file_grid.css"
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
        "${CMAKE_SOURCE_DIR}/static/styles/dialog/direct_connect_dialog.css"
        "${CMAKE_SOURCE_DIR}/static/styles/dialog/confirm_dialog.css"
        "${CMAKE_SOURCE_DIR}/static/styles/dialog/input_dialog.css"
        "${CMAKE_SOURCE_DIR}/static/styles/dialog/archive_transfer_dialog.css"
        "${CMAKE_SOURCE_DIR}/static/styles/operation_queue.css"
        "${CMAKE_SOURCE_DIR}/static/styles/session_area.css"
        "${CMAKE_SOURCE_DIR}/static/styles/session.css"
        "${CMAKE_SOURCE_DIR}/static/styles/settings_group.css"
        "${CMAKE_SOURCE_DIR}/static/styles/settings.css"
        "${CMAKE_SOURCE_DIR}/static/styles/licenses.css"
        "${CMAKE_SOURCE_DIR}/static/styles/sync_dialog.css"
        "${CMAKE_SOURCE_DIR}/static/styles/sidebar.css"
        "${CMAKE_SOURCE_DIR}/static/styles/toolbar.css"
        "${CMAKE_SOURCE_DIR}/static/styles/local_shell_tab.css"
        "${CMAKE_SOURCE_DIR}/static/styles/terminal_toolbar.css"
        "${CMAKE_SOURCE_DIR}/static/styles/file_tracking_panel.css"
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
        "${CMAKE_SOURCE_DIR}/dependencies/5cript-nui-components/styles/pagination.css"
        "${CMAKE_SOURCE_DIR}/dependencies/5cript-nui-components/styles/popup_menu.css"
        "${CMAKE_SOURCE_DIR}/dependencies/5cript-nui-components/styles/dropdown_menu.css"
        "${CMAKE_SOURCE_DIR}/dependencies/5cript-nui-components/styles/resizeable_table.css"
        "${CMAKE_SOURCE_DIR}/dependencies/5cript-nui-components/styles/spotlight_overlay.css"
        "${CMAKE_SOURCE_DIR}/dependencies/5cript-nui-components/styles/select.css"
        "${CMAKE_SOURCE_DIR}/dependencies/5cript-nui-components/styles/switch.css"
        "${CMAKE_SOURCE_DIR}/dependencies/5cript-nui-components/styles/tabs.css"
        "${CMAKE_SOURCE_DIR}/dependencies/5cript-nui-components/styles/text_input.css"
        "${CMAKE_SOURCE_DIR}/dependencies/5cript-nui-components/styles/tree.css"
)

target_include_directories(
    ${PROJECT_NAME}
    PRIVATE
        "${CMAKE_SOURCE_DIR}/frontend/include"
        "${CMAKE_BINARY_DIR}/include"
        # Only command-store/types.hpp is used here; the library itself is backend/native only.
        "${CMAKE_SOURCE_DIR}/command-store/include"
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