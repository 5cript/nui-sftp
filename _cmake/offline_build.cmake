# When this file is included its a build requiring to be offline (flatpak, yocto).
if(NOT EMSCRIPTEN)
    # Installed into sytem:
    set(NUI_FETCH_BOOST_PREPROCESSOR OFF CACHE BOOL "Do not fetch boost preprocessor, assume it is provided by the system" FORCE)
    set(NUI_FETCH_INTERVAL_TREE OFF CACHE BOOL "Do not fetch interval tree, assume it is provided by the system" FORCE)
    set(NUI_FETCH_BOOST_DESCRIBE OFF CACHE BOOL "Do not fetch boost describe, assume it is provided by the system" FORCE)
    set(NUI_FETCH_BOOST_MP11 OFF CACHE BOOL "Do not fetch boost mp11, assume it is provided by the system" FORCE)
    set(NUI_FETCH_NLOHMANN_JSON OFF CACHE BOOL "Do not fetch nlohmann_json, assume it is provided by the system" FORCE)
    set(NUI_FETCH_BINARYEN OFF CACHE BOOL "Do not fetch binaryen, assume it is provided by the system" FORCE)
    set(NUI_FETCH_FMT OFF CACHE BOOL "Do not fetch fmt, assume it is provided by the system" FORCE)
    set(NUI_FIND_FMT OFF CACHE BOOL "Do not find fmt, assume it is provided by the system" FORCE)
    set(ROAR_EXTERNAL_NLOHMANN_JSON ON CACHE BOOL "Use external nlohmann_json for roar, assume it is provided by the system" FORCE)

    # Checked out in dependencies directory in source dir:
    set(NUI_FETCH_TRAITS OFF CACHE BOOL "Do not fetch nui traits, assume it is in dependencies/traits" FORCE)
    set(NUI_FETCH_WEBVIEW OFF CACHE BOOL "Do not fetch webview, assume it is in dependencies/webview" FORCE)
    set(NUI_FETCH_PORTABLE_FILE_DIALOG OFF CACHE BOOL "Do not fetch portable file dialog, assume it is in dependencies/portable-file-dialogs" FORCE)
    set(NUI_FETCH_ROAR OFF CACHE BOOL "Do not fetch roar, assume it is in dependencies/roar" FORCE)
    set(ROAR_EXTERNAL_PROMISE ON CACHE BOOL "Use external promise-cpp for roar, assume it is in dependencies/promise-cpp" FORCE)
    set(FETCH_YAML_CPP OFF CACHE BOOL "Do not fetch yaml-cpp, assume it is in dependencies/yml-cpp" FORCE)
    set(FETCH_RAPIDFUZZ OFF CACHE BOOL "Do not fetch rapidfuzz, assume it is in dependencies/rapidfuzz" FORCE)
    set(FETCH_SPDLOG OFF CACHE BOOL "Do not fetch spdlog, assume it is in dependencies/spdlog" FORCE)
    set(FETCH_EFSW OFF CACHE BOOL "Do not fetch efsw, assume it is in dependencies/efsw" FORCE)
    set(FETCH_ICONS OFF CACHE BOOL "Do not fetch icons, assume it is in dependencies/icons" FORCE)

    set(OMIT_FRONTEND_BUILD ON CACHE BOOL "Do not build the frontend, in offline environments emscripten is hard to use" FORCE)

    find_package(nlohmann_json REQUIRED)
    find_package(fmt REQUIRED)

    add_library(boost_preprocessor INTERFACE IMPORTED GLOBAL)
    add_library(interval-tree INTERFACE IMPORTED GLOBAL)
    add_library(boost_describe INTERFACE IMPORTED GLOBAL)
    add_library(boost_mp11 INTERFACE IMPORTED GLOBAL)

    add_subdirectory("${CMAKE_SOURCE_DIR}/dependencies/portable-file-dialogs" EXCLUDE_FROM_ALL)
    add_subdirectory("${CMAKE_SOURCE_DIR}/dependencies/webview" EXCLUDE_FROM_ALL)
    add_subdirectory("${CMAKE_SOURCE_DIR}/dependencies/yaml-cpp" EXCLUDE_FROM_ALL)
    add_subdirectory("${CMAKE_SOURCE_DIR}/dependencies/rapidfuzz" EXCLUDE_FROM_ALL)
    add_subdirectory("${CMAKE_SOURCE_DIR}/dependencies/spdlog" EXCLUDE_FROM_ALL)
    add_subdirectory("${CMAKE_SOURCE_DIR}/dependencies/efsw" EXCLUDE_FROM_ALL)
    add_subdirectory("${CMAKE_SOURCE_DIR}/dependencies/promise-cpp" EXCLUDE_FROM_ALL)
endif()