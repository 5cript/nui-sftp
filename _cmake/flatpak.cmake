# When this file is included its a flatpak build

if(NOT EMSCRIPTEN)
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