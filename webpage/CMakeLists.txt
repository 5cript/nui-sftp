cmake_minimum_required(VERSION 3.21)

# Pull in the shared git-tag-driven version pipeline. Produces
# ${CMAKE_BINARY_DIR}/include/version.hpp inside this subbuild's binary dir,
# defining VERSION / VERSION_NON_DIRTY / GIT_TAG / GIT_BRANCH macros.
include("${CMAKE_CURRENT_LIST_DIR}/../_cmake/version.cmake")

# When invoked as a standalone subprocess by nui_add_emscripten_target the
# webpage acts as its own top-level project. When pulled in via
# add_subdirectory() from the outer build it just adds the emscripten module.
project(nui-sftp-webpage VERSION "${PROJECT_VERSION}" LANGUAGES CXX)

# Pull in Nui only when not already provided by an outer build (e.g. when
# this CMakeLists.txt is the entry point of the emscripten subbuild).
# `nui-events` exists in both EMSCRIPTEN and host builds, so it's a reliable
# probe for "Nui already added".
if (NOT TARGET nui-events)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../dependencies/Nui" "${CMAKE_BINARY_DIR}/_nui")
endif()

# Reuse the outer common-options helper if it is already in scope. When this
# file is the standalone entry point, fall back to a minimal shim.
if (NOT COMMAND set_common_options)
    function(set_common_options TARGET)
        set(COMMON_OPTIONS -fexceptions -Wall -pedantic-errors -pedantic -fconstexpr-steps=1271242)
        target_compile_options(${TARGET} PUBLIC "$<$<CONFIG:DEBUG>:-g;${COMMON_OPTIONS}>")
        target_compile_options(${TARGET} PUBLIC "$<$<CONFIG:RELEASE>:-O3;${COMMON_OPTIONS}>")
    endfunction()
endif()

set(WEBPAGE_PREJS_FILE "${CMAKE_CURRENT_LIST_DIR}/frontend/source/frontend/js/module.js")

if (EMSCRIPTEN)
    add_executable(nui-sftp-webpage)
    if (NOT TARGET script-nui-components)
        add_subdirectory(
            "${CMAKE_CURRENT_LIST_DIR}/../dependencies/5cript-nui-components"
            "${CMAKE_BINARY_DIR}/_5cript_nui_components"
        )
    endif()
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/frontend/source/frontend")
else()
    add_library(nui-sftp-webpage INTERFACE)

    # ------------------------------------------------------------------
    # Fetch product screenshots used by the carousel slides.
    # The zip is unpacked once per build dir; the PNGs are then staged
    # into the parcel dist as a POST_BUILD step (see below) so the
    # browser can request them at /assets/<name>.png.
    # ------------------------------------------------------------------
    include(FetchContent)
    FetchContent_Declare(
        webpage_page_assets
        URL "https://s3.g.s4.mega.io/jgemkib4a5fte35rktt5wxrwkw4ejk4ybemkf/nui-scp/PageAssets.zip"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(webpage_page_assets)

    nui_add_emscripten_target(nui-sftp-webpage
        TARGET
            nui-sftp-webpage
        PREJS
            ${WEBPAGE_PREJS_FILE}
        SOURCE_DIR
            "${CMAKE_CURRENT_LIST_DIR}"
        CMAKE_OPTIONS
    )

    # Stage fetched PNGs and committed static assets (SVG icons, etc.) into
    # the parcel dist after the webpage builds. Both end up under bin/assets/
    # so the C++ runtime can reference them as `assets/<name>` URLs.
    set(WEBPAGE_DIST_DIR "${CMAKE_BINARY_DIR}/module_nui-sftp-webpage/bin")
    add_custom_command(
        TARGET nui-sftp-webpage-emscripten POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${WEBPAGE_DIST_DIR}/assets"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${webpage_page_assets_SOURCE_DIR}" "${WEBPAGE_DIST_DIR}/assets"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_LIST_DIR}/static/assets" "${WEBPAGE_DIST_DIR}/assets"
        COMMENT "Staging fetched and committed page assets into webpage dist"
        VERBATIM
    )
endif()
