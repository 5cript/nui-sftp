cmake_minimum_required(VERSION 3.21)

project(nui-scp VERSION 0.1.0)

set(NUI_FETCH_ROAR OFF CACHE BOOL "Dont fetch roar, use local copy" FORCE)
set(NUI_FETCH_NLOHMANN_JSON OFF CACHE BOOL "Dont fetch nlohmann, use local copy" FORCE)
set(ROAR_EXTERNAL_NLOHMANN_JSON ON CACHE BOOL "Use an external nlohmann_json library (provide it manually)" FORCE)
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "Dont build shared spdlog" FORCE)
set(NUI_FILE_EXPLORER_MEMORY64 ON CACHE BOOL "Enable MEMORY64 for nui-file-explorer" FORCE)

option(BUILD_SHARED_LIBS "Do not build shared libraries" OFF)
option(BUILD_STATIC_LIBS "Build static libraries" ON)
option(NUI_SCP_ENABLE_TESTING "Enable testing for nui-scp" OFF)

include (${CMAKE_CURRENT_LIST_DIR}/_cmake/common_options.cmake)

# Nui Dependency
add_subdirectory("${CMAKE_SOURCE_DIR}/dependencies/Nui")

# Add executable (sources are added later, depending on frontend/backend)
add_executable(${PROJECT_NAME})
target_link_libraries(${PROJECT_NAME} PUBLIC core-target)

# Subdirectories & Dependencies
add_library(roar-include-only INTERFACE)
target_include_directories(roar-include-only INTERFACE "${CMAKE_CURRENT_LIST_DIR}/dependencies/roar/include")

include("${CMAKE_CURRENT_LIST_DIR}/dependencies/Nui/cmake/dependencies/nlohmann_json.cmake")
add_subdirectory("${CMAKE_SOURCE_DIR}/constants/source/constants")
add_subdirectory("${CMAKE_SOURCE_DIR}/ids/source/ids")
add_subdirectory("${CMAKE_SOURCE_DIR}/shared_data/source/shared_data")
add_subdirectory("${CMAKE_SOURCE_DIR}/utility/source/utility")
add_subdirectory("${CMAKE_SOURCE_DIR}/log/source/log")
add_subdirectory("${CMAKE_SOURCE_DIR}/persistence/source/persistence")
add_subdirectory("${CMAKE_SOURCE_DIR}/events/source/events")
include("${CMAKE_CURRENT_LIST_DIR}/_cmake/dependencies.cmake")

set(PREJS_FILE "${CMAKE_SOURCE_DIR}/frontend/source/frontend/js/module.js")

if (EMSCRIPTEN)
    include("${CMAKE_CURRENT_LIST_DIR}/_cmake/theme_compilation.cmake")
    include("${CMAKE_CURRENT_LIST_DIR}/_cmake/dependencies/ui5.cmake")
    add_subdirectory("${CMAKE_SOURCE_DIR}/nui-file-explorer/source/nui-file-explorer")
    add_subdirectory("${CMAKE_SOURCE_DIR}/frontend/source/frontend")
else()
    include("${CMAKE_CURRENT_LIST_DIR}/_cmake/resource_copy.cmake")
    add_subdirectory("${CMAKE_SOURCE_DIR}/ssh/source/ssh")
	add_subdirectory("${CMAKE_SOURCE_DIR}/backend/source/backend")
    add_subdirectory("${CMAKE_SOURCE_DIR}/dependencies/roar")
    # add_subdirectory("${CMAKE_SOURCE_DIR}/dependencies/process")

    # If msys2, copy dynamic libraries to executable directory, visual studio does this automatically.
    # And there is no need on linux.
    if (DEFINED ENV{MSYSTEM})
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND bash -c "ldd $<TARGET_FILE:${PROJECT_NAME}>" | "grep" "clang" | awk "NF == 4 { system(\"${CMAKE_COMMAND} -E copy \" \$3 \" $<TARGET_FILE_DIR:${PROJECT_NAME}>\") }"
            VERBATIM
        )
    endif()

    # Copy bin (for SAPUI5) files to destination for themes and languages
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_BINARY_DIR}/module_${PROJECT_NAME}/bin" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/dynamic_sources"
        COMMAND ${CMAKE_COMMAND} -E rm "$<TARGET_FILE_DIR:${PROJECT_NAME}>/dynamic_sources/index.html"
        COMMAND ${CMAKE_COMMAND} -E rm "$<TARGET_FILE_DIR:${PROJECT_NAME}>/dynamic_sources/index.js"
        VERBATIM
    )

    if (NUI_SCP_ENABLE_TESTING)
        include(./_cmake/dependencies/gtest.cmake)
        enable_testing()
        add_subdirectory(ssh/test/ssh)
        add_subdirectory(backend/test/backend)
        add_subdirectory(utility/test/utility)
    endif()
endif()