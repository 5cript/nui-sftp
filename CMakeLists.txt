cmake_minimum_required(VERSION 4.1)

include("${CMAKE_CURRENT_LIST_DIR}/_cmake/version.cmake")

project(nui-sftp VERSION "${PROJECT_VERSION}" LANGUAGES CXX)

set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "Use an external fmt library (provide it manually)" FORCE)
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "Dont build shared spdlog" FORCE)
set(NUI_FILE_EXPLORER_MEMORY64 ON CACHE BOOL "Enable MEMORY64 for nui-file-explorer" FORCE)
set(JSON_Diagnostics ON CACHE BOOL "Enable json diagnostics" FORCE)
set(NUI_JSON_DIAGNOSTICS ON CACHE BOOL "Enable json diagnostics for nlohmann_json through nui" FORCE)
set(WEBKIT_PATH "" CACHE PATH "Path to custom built WebKit, if empty, will use system WebKit")
set(WEBKIT_PACKAGE_NAME "webkitgtk-6.0" CACHE STRING "Pkg-config package name for WebKit, used if WEBKIT_PATH is set")

option(FETCH_YAML_CPP "Fetch yaml-cpp" ON)
option(BUILD_SHARED_LIBS "Do not build shared libraries" OFF)
option(BUILD_STATIC_LIBS "Build static libraries" ON)
option(NUI_SFTP_ENABLE_TESTING "Enable testing for nui-sftp" OFF)
option(OFFLINE_BUILD "Inform CMake that we are building for offline environments (flatpak, yocto), this will set some additional flags and dependencies" OFF)
option(OMIT_FRONTEND_BUILD "Dont build the frontend, used primarily for flatpak, because using emscripten there is hell" OFF)
option(FETCH_RAPIDFUZZ "Fetch rapidfuzz" ON)
option(FETCH_SPDLOG "Fetch spdlog" ON)
option(FETCH_EFSW "Fetch efsw" ON)
option(NUI_SFT_ENABLE_LANGUAGE_TOOLING "Enable building language tooling, which includes a command line tool for checking language files" OFF)

if (OFFLINE_BUILD)
    include("${CMAKE_CURRENT_LIST_DIR}/_cmake/offline_build.cmake")
endif()

if (OMIT_FRONTEND_BUILD)
    set(NUI_OMIT_FRONTEND ON CACHE BOOL "Do not build the frontend, sometimes useful for environments where emscripten is hard to use" FORCE)
endif()

if (NOT EMSCRIPTEN)
    set(BACKEND_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
endif()
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/_cmake/configured_files/build_environment.hpp.in"
    "${CMAKE_BINARY_DIR}/include/build_environment.hpp"
)

include (${CMAKE_CURRENT_LIST_DIR}/_cmake/common_options.cmake)

# Webkit from custom build
if (NOT EMSCRIPTEN)
    if (WEBKIT_PATH)
        message(STATUS "Using custom WebKit build from ${WEBKIT_PATH}")
        set(ENV{PKG_CONFIG_PATH} "${WEBKIT_PATH}/lib/pkgconfig")
        message(STATUS "PKG_CONFIG_PATH set to: $ENV{PKG_CONFIG_PATH}")

        find_package(PkgConfig REQUIRED)
        pkg_search_module(
            webkit2 REQUIRED ${WEBKIT_PACKAGE_NAME}
            IMPORTED_TARGET GLOBAL
            NO_CMAKE_ENVIRONMENT_PATH
            PATHS "${WEBKIT_PATH}/lib/pkgconfig"
        )
        # get include directories from pkgconfig target
        get_target_property(WEBKIT_INCLUDE_DIRS PkgConfig::webkit2 INTERFACE_INCLUDE_DIRECTORIES)
        message(STATUS "WebKit include directories: ${WEBKIT_INCLUDE_DIRS}")
    endif()
endif()

# Nui Dependency
add_subdirectory("${CMAKE_SOURCE_DIR}/dependencies/Nui" EXCLUDE_FROM_ALL)

# Add executable (sources are added later, depending on frontend/backend)
add_executable(${PROJECT_NAME})
target_link_libraries(${PROJECT_NAME} PUBLIC core-target)

# Subdirectories & Dependencies
add_library(roar-include-only INTERFACE)
target_include_directories(roar-include-only INTERFACE "${CMAKE_CURRENT_LIST_DIR}/dependencies/roar/include")

include("${CMAKE_CURRENT_LIST_DIR}/dependencies/Nui/cmake/dependencies/nlohmann_json.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/dependencies/Nui/cmake/dependencies/fmt.cmake")
add_subdirectory("${CMAKE_SOURCE_DIR}/dependencies/gimo" EXCLUDE_FROM_ALL)
add_subdirectory("${CMAKE_SOURCE_DIR}/dependencies/traits" EXCLUDE_FROM_ALL)
add_subdirectory("${CMAKE_SOURCE_DIR}/constants/source/constants")
add_subdirectory("${CMAKE_SOURCE_DIR}/ids/source/ids")
add_subdirectory("${CMAKE_SOURCE_DIR}/shared_data/source/shared_data")
add_subdirectory("${CMAKE_SOURCE_DIR}/utility/source/utility")
add_subdirectory("${CMAKE_SOURCE_DIR}/log/source/log")
if (NOT EMSCRIPTEN)
    include("${CMAKE_CURRENT_LIST_DIR}/_cmake/dependencies/yaml-cpp.cmake" EXCLUDE_FROM_ALL)
endif()
add_subdirectory("${CMAKE_SOURCE_DIR}/persistence/source/persistence")
add_subdirectory("${CMAKE_SOURCE_DIR}/events/source/events")
include("${CMAKE_CURRENT_LIST_DIR}/_cmake/dependencies.cmake")

set(PREJS_FILE "${CMAKE_SOURCE_DIR}/frontend/source/frontend/js/module.js")

if (EMSCRIPTEN)
    add_subdirectory("${CMAKE_SOURCE_DIR}/nui-file-explorer/source/nui-file-explorer")
    add_subdirectory("${CMAKE_SOURCE_DIR}/dependencies/5cript-nui-components")
    add_subdirectory("${CMAKE_SOURCE_DIR}/frontend/source/frontend")
else()
    include("${CMAKE_CURRENT_LIST_DIR}/_cmake/copy_styles_to_build_dir.cmake")

    if (NOT ${NUI_FETCH_ROAR})
        add_subdirectory("${CMAKE_SOURCE_DIR}/dependencies/roar")
    endif()

    add_subdirectory("${CMAKE_SOURCE_DIR}/ssh/source/ssh")
	add_subdirectory("${CMAKE_SOURCE_DIR}/backend/source/backend")

    # If msys2, copy dynamic libraries to executable directory, visual studio does this automatically.
    # And there is no need on linux.
    if (DEFINED ENV{MSYSTEM})
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND bash -c "ldd $<TARGET_FILE:${PROJECT_NAME}>" | "grep" "clang" | awk "NF == 4 { system(\"${CMAKE_COMMAND} -E copy \" \$3 \" $<TARGET_FILE_DIR:${PROJECT_NAME}>\") }"
            VERBATIM
        )
    endif()

    # Copy module bin to frontend.
    if (NOT OMIT_FRONTEND_BUILD)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_BINARY_DIR}/module_${PROJECT_NAME}/bin" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/../frontend"
            VERBATIM
        )
    endif()

    if (NUI_SFTP_ENABLE_TESTING)
        include(./_cmake/dependencies/gtest.cmake)
        enable_testing()
        add_subdirectory(ssh/test/ssh)
        add_subdirectory(backend/test/backend)
        add_subdirectory(utility/test/utility)
    endif()

    if (NUI_SFT_ENABLE_LANGUAGE_TOOLING)
        add_subdirectory(tools/language_check/source/language_check)
    endif()
endif()