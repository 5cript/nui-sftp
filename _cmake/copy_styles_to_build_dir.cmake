add_custom_command(
    OUTPUT
        "${CMAKE_BINARY_DIR}/styles/nui-file-explorer/dropdown_menu.css"
        "${CMAKE_BINARY_DIR}/styles/nui-file-explorer/file_grid.css"
        "${CMAKE_BINARY_DIR}/styles/5cript-nui-components/button.css"
        "${CMAKE_BINARY_DIR}/styles/5cript-nui-components/color_picker.css"
        "${CMAKE_BINARY_DIR}/styles/5cript-nui-components/resizeable_table.css"
        "${CMAKE_BINARY_DIR}/styles/5cript-nui-components/select.css"
        "${CMAKE_BINARY_DIR}/styles/5cript-nui-components/switch.css"
        "${CMAKE_BINARY_DIR}/styles/5cript-nui-components/text_input.css"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/styles/nui-file-explorer"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/nui-file-explorer/styles/dropdown_menu.css" "${CMAKE_BINARY_DIR}/styles/nui-file-explorer/dropdown_menu.css"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/nui-file-explorer/styles/file_grid.css" "${CMAKE_BINARY_DIR}/styles/nui-file-explorer/file_grid.css"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/dependencies/5cript-nui-components/styles/button.css" "${CMAKE_BINARY_DIR}/styles/5cript-nui-components/button.css"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/dependencies/5cript-nui-components/styles/color_picker.css" "${CMAKE_BINARY_DIR}/styles/5cript-nui-components/color_picker.css"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/dependencies/5cript-nui-components/styles/resizeable_table.css" "${CMAKE_BINARY_DIR}/styles/5cript-nui-components/resizeable_table.css"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/dependencies/5cript-nui-components/styles/select.css" "${CMAKE_BINARY_DIR}/styles/5cript-nui-components/select.css"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/dependencies/5cript-nui-components/styles/switch.css" "${CMAKE_BINARY_DIR}/styles/5cript-nui-components/switch.css"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/dependencies/5cript-nui-components/styles/text_input.css" "${CMAKE_BINARY_DIR}/styles/5cript-nui-components/text_input.css"
    DEPENDS
        "${CMAKE_SOURCE_DIR}/nui-file-explorer/styles/dropdown_menu.css"
        "${CMAKE_SOURCE_DIR}/nui-file-explorer/styles/file_grid.css"
        "${CMAKE_SOURCE_DIR}/dependencies/5cript-nui-components/styles/button.css"
        "${CMAKE_SOURCE_DIR}/dependencies/5cript-nui-components/styles/color_picker.css"
        "${CMAKE_SOURCE_DIR}/dependencies/5cript-nui-components/styles/resizeable_table.css"
        "${CMAKE_SOURCE_DIR}/dependencies/5cript-nui-components/styles/select.css"
        "${CMAKE_SOURCE_DIR}/dependencies/5cript-nui-components/styles/switch.css"
        "${CMAKE_SOURCE_DIR}/dependencies/5cript-nui-components/styles/text_input.css"
)

add_custom_target(
    nui-sftp-style-copy
    ALL
    DEPENDS
        "${CMAKE_BINARY_DIR}/styles/nui-file-explorer/dropdown_menu.css"
        "${CMAKE_BINARY_DIR}/styles/nui-file-explorer/file_grid.css"
        "${CMAKE_BINARY_DIR}/styles/5cript-nui-components/button.css"
        "${CMAKE_BINARY_DIR}/styles/5cript-nui-components/color_picker.css"
        "${CMAKE_BINARY_DIR}/styles/5cript-nui-components/resizeable_table.css"
        "${CMAKE_BINARY_DIR}/styles/5cript-nui-components/select.css"
        "${CMAKE_BINARY_DIR}/styles/5cript-nui-components/switch.css"
        "${CMAKE_BINARY_DIR}/styles/5cript-nui-components/text_input.css"
)