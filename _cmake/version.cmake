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
  message(WARNING "Git information not found, using default version 0.0.0")
else()
  execute_process(
    COMMAND ${GIT_EXECUTABLE} describe --exact-match --tags OUTPUT_VARIABLE GIT_TAG ERROR_QUIET)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse --abbrev-ref HEAD OUTPUT_VARIABLE GIT_BRANCH)

  string(STRIP "${GIT_TAG}" GIT_TAG)
  string(STRIP "${GIT_BRANCH}" GIT_BRANCH)
  string(STRIP "${VERSION}" VERSION)
  string(STRIP "${VERSION_NON_DIRTY}" VERSION_NON_DIRTY)
  message(STATUS "Git tag: ${GIT_TAG}")
  message(STATUS "Git branch: ${GIT_BRANCH}")
  message(STATUS "Version: ${VERSION}")
  message(STATUS "Version (non-dirty): ${VERSION_NON_DIRTY}")
endif()

configure_file(
  "${CMAKE_CURRENT_LIST_DIR}/configured_files/version.hpp.in"
  "${CMAKE_BINARY_DIR}/include/version.hpp"
)
configure_file(
  "${CMAKE_CURRENT_LIST_DIR}/configured_files/version.txt.in"
  "${CMAKE_BINARY_DIR}/text/version.txt"
)