set(THEME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/../themes")
set(THEME_NAMES "dark")

set(THEME_OUTPUTS "")
foreach(THEME_NAME ${THEME_NAMES})
    list(APPEND THEME_OUTPUTS "${THEME_OUTPUT_DIRECTORY}/${THEME_NAME}/css_variables.css")
endforeach()

set(THEME_DEPENDS "")
foreach(THEME_NAME ${THEME_NAMES})
    list(APPEND THEME_DEPENDS "${CMAKE_SOURCE_DIR}/themes/${THEME_NAME}/theme.less")
endforeach()

set(SYMLINK_COMMAND "")
if(CMAKE_HOST_WIN32)
else()
    set(SYMLINK_COMMAND COMMAND ${CMAKE_COMMAND} -E create_symlink "${CMAKE_CURRENT_BINARY_DIR}/node_modules" "${CMAKE_SOURCE_DIR}/themes/node_modules")
endif()

add_custom_command(
    OUTPUT ${THEME_OUTPUTS}
    ${SYMLINK_COMMAND}
    COMMAND ${NUI_NODE} "${CMAKE_SOURCE_DIR}/themes/compile.mjs" "${THEME_OUTPUT_DIRECTORY}"
    DEPENDS ${THEME_DEPENDS}
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

add_custom_target(
    theme_compilation ALL
    DEPENDS ${THEME_OUTPUTS}
)