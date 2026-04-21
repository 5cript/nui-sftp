if(DEFINED FORCED_PROJECT_VERSION)
    if(NOT "${FORCED_PROJECT_VERSION}" MATCHES "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$")
        message(FATAL_ERROR "FORCED_PROJECT_VERSION must be in 'Major.Minor.Patch' format, got: '${FORCED_PROJECT_VERSION}'")
    endif()

    string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$" _ "${FORCED_PROJECT_VERSION}")
    set(SEMVER_MAJOR "${CMAKE_MATCH_1}")
    set(SEMVER_MINOR "${CMAKE_MATCH_2}")
    set(SEMVER_PATCH "${CMAKE_MATCH_3}")
    set(SEMVER_PRERELEASE "")
    set(SEMVER_BUILD "")
    set(VERSION "${FORCED_PROJECT_VERSION}")
    set(VERSION_NON_DIRTY "${FORCED_PROJECT_VERSION}")
    set(PROJECT_VERSION "${FORCED_PROJECT_VERSION}")
    set(GIT_TAG "N/A")
    set(GIT_BRANCH "N/A")
    message(STATUS "Using forced version: ${FORCED_PROJECT_VERSION}")
else()
    find_package(Git REQUIRED)
    execute_process(
        COMMAND
            "${GIT_EXECUTABLE}" rev-list --tags --max-count=1
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE
            REV_LIST
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    execute_process(
        COMMAND
            "${GIT_EXECUTABLE}" describe --tags --dirty
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE
            VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    execute_process(
        COMMAND
            "${GIT_EXECUTABLE}" describe --tags ${REV_LIST}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE
            VERSION_NON_DIRTY
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    #if tag starts with "v" remove it
    if ("${VERSION}" MATCHES "^v.+")
        string(SUBSTRING "${VERSION}" 1 -1 VERSION)
    endif()
    if ("${VERSION_NON_DIRTY}" MATCHES "^v.+")
        string(SUBSTRING "${VERSION_NON_DIRTY}" 1 -1 VERSION_NON_DIRTY)
    endif()

    if("${VERSION}" STREQUAL "")
        set(GIT_TAG "N/A")
        set(GIT_BRANCH "N/A")
        set(VERSION "0.0.0")
        set(VERSION_NON_DIRTY "0.0.0")
        set(PROJECT_VERSION "0.0.0")
        message(WARNING "Git information not found, using default version 0.0.0")
    else()
        execute_process(
            COMMAND ${GIT_EXECUTABLE} describe --exact-match --tags OUTPUT_VARIABLE GIT_TAG ERROR_QUIET)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} rev-parse --abbrev-ref HEAD OUTPUT_VARIABLE GIT_BRANCH)

        set(SEMVER_REGEX
            "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)(-((0|[1-9][0-9]*|[0-9]*[a-zA-Z-][0-9a-zA-Z-]*)(\\.(0|[1-9][0-9]*|[0-9]*[a-zA-Z-][0-9a-zA-Z-]*))*))?(\\+([0-9a-zA-Z-]+(\\.[0-9a-zA-Z-]+)*))?$"
        )

        string(REGEX MATCH "${SEMVER_REGEX}" _ "${VERSION_NON_DIRTY}")
        set(SEMVER_MAJOR "${CMAKE_MATCH_1}")
        set(SEMVER_MINOR "${CMAKE_MATCH_2}")
        set(SEMVER_PATCH "${CMAKE_MATCH_3}")
        set(SEMVER_PRERELEASE "${CMAKE_MATCH_5}") # without leading '-'
        set(SEMVER_BUILD "${CMAKE_MATCH_10}")      # without leading '+'
        set(PROJECT_VERSION "${SEMVER_MAJOR}.${SEMVER_MINOR}.${SEMVER_PATCH}")

        string(STRIP "${GIT_TAG}" GIT_TAG)
        string(STRIP "${GIT_BRANCH}" GIT_BRANCH)
        string(STRIP "${VERSION}" VERSION)
        string(STRIP "${VERSION_NON_DIRTY}" VERSION_NON_DIRTY)
        message(STATUS "Git tag: ${GIT_TAG}")
        message(STATUS "Git branch: ${GIT_BRANCH}")
        message(STATUS "Version: ${VERSION}")
        message(STATUS "Version (non-dirty): ${VERSION_NON_DIRTY}")
        message(STATUS "Semver major: ${SEMVER_MAJOR}")
        message(STATUS "Semver minor: ${SEMVER_MINOR}")
        message(STATUS "Semver patch: ${SEMVER_PATCH}")
        message(STATUS "Semver prerelease: ${SEMVER_PRERELEASE}")
        message(STATUS "Semver build: ${SEMVER_BUILD}")
    endif()
endif()

configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/configured_files/version.hpp.in"
    "${CMAKE_BINARY_DIR}/include/version.hpp"
)
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/configured_files/version.txt.in"
    "${CMAKE_BINARY_DIR}/text/version.txt"
)