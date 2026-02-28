# Icons

include(FetchContent)

FetchContent_Declare(
    base-icons
    URL "https://s3.g.s4.mega.io/jgemkib4a5fte35rktt5wxrwkw4ejk4ybemkf/nui-scp/icons.tar.gz"
    URL_HASH MD5=38d01d8769dda2a006495c7a3caa11b4
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_MakeAvailable(base-icons)

add_custom_command(
    OUTPUT
        "${CMAKE_BINARY_DIR}/assets/icons/folder_main.png"
        "${CMAKE_BINARY_DIR}/assets/icons/file.png"
        "${CMAKE_BINARY_DIR}/assets/icons/hard_drive.png"
        "${CMAKE_BINARY_DIR}/assets/icons/cpp.png"
        "${CMAKE_BINARY_DIR}/assets/icons/css.png"
        "${CMAKE_BINARY_DIR}/assets/icons/go.png"
        "${CMAKE_BINARY_DIR}/assets/icons/csharp.png"
        "${CMAKE_BINARY_DIR}/assets/icons/cad.png"
        "${CMAKE_BINARY_DIR}/assets/icons/c.png"
        "${CMAKE_BINARY_DIR}/assets/icons/html.png"
        "${CMAKE_BINARY_DIR}/assets/icons/jar.png"
        "${CMAKE_BINARY_DIR}/assets/icons/java.png"
        "${CMAKE_BINARY_DIR}/assets/icons/js.png"
        "${CMAKE_BINARY_DIR}/assets/icons/log.png"
        "${CMAKE_BINARY_DIR}/assets/icons/sql.png"
        "${CMAKE_BINARY_DIR}/assets/icons/swift.png"
        "${CMAKE_BINARY_DIR}/assets/icons/rust.png"
        "${CMAKE_BINARY_DIR}/assets/icons/python.png"
        "${CMAKE_BINARY_DIR}/themes/dark/css_variables.css"
    COMMAND ${CMAKE_COMMAND} -E copy "${base-icons_SOURCE_DIR}/os_folders/windows/11/folder_main.png" "${CMAKE_BINARY_DIR}/assets/icons/folder_main.png"
    COMMAND ${CMAKE_COMMAND} -E copy "${base-icons_SOURCE_DIR}/masks/Others/hard-drive.png" "${CMAKE_BINARY_DIR}/assets/icons/hard_drive.png"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_LIST_DIR}/../static/assets/file.png" "${CMAKE_BINARY_DIR}/assets/icons/file.png"
    COMMAND ${CMAKE_COMMAND} -E copy "${base-icons_SOURCE_DIR}/masks/Development/noun-c-4921409.png" "${CMAKE_BINARY_DIR}/assets/icons/cpp.png"
    COMMAND ${CMAKE_COMMAND} -E copy "${base-icons_SOURCE_DIR}/masks/Development/css-3-logo.png" "${CMAKE_BINARY_DIR}/assets/icons/css.png"
    COMMAND ${CMAKE_COMMAND} -E copy "${base-icons_SOURCE_DIR}/masks/Development/noun-html-1174764.png" "${CMAKE_BINARY_DIR}/assets/icons/html.png"
    COMMAND ${CMAKE_COMMAND} -E copy "${base-icons_SOURCE_DIR}/masks/Development/Go-Logo_Black.png" "${CMAKE_BINARY_DIR}/assets/icons/go.png"
    COMMAND ${CMAKE_COMMAND} -E copy "${base-icons_SOURCE_DIR}/masks/Development/noun-c-4921443.png" "${CMAKE_BINARY_DIR}/assets/icons/csharp.png"
    COMMAND ${CMAKE_COMMAND} -E copy "${base-icons_SOURCE_DIR}/masks/Development/noun-cad-4921405.png" "${CMAKE_BINARY_DIR}/assets/icons/cad.png"
    COMMAND ${CMAKE_COMMAND} -E copy "${base-icons_SOURCE_DIR}/masks/Development/noun-c-file-115671.png" "${CMAKE_BINARY_DIR}/assets/icons/c.png"
    COMMAND ${CMAKE_COMMAND} -E copy "${base-icons_SOURCE_DIR}/masks/Development/noun-jar-4921452.png" "${CMAKE_BINARY_DIR}/assets/icons/jar.png"
    COMMAND ${CMAKE_COMMAND} -E copy "${base-icons_SOURCE_DIR}/masks/Development/noun-java-1156842.png" "${CMAKE_BINARY_DIR}/assets/icons/java.png"
    COMMAND ${CMAKE_COMMAND} -E copy "${base-icons_SOURCE_DIR}/masks/Development/noun-js-4921450.png" "${CMAKE_BINARY_DIR}/assets/icons/js.png"
    COMMAND ${CMAKE_COMMAND} -E copy "${base-icons_SOURCE_DIR}/masks/Development/noun-log-4921382.png" "${CMAKE_BINARY_DIR}/assets/icons/log.png"
    COMMAND ${CMAKE_COMMAND} -E copy "${base-icons_SOURCE_DIR}/masks/Development/noun-sql-4921378.png" "${CMAKE_BINARY_DIR}/assets/icons/sql.png"
    COMMAND ${CMAKE_COMMAND} -E copy "${base-icons_SOURCE_DIR}/masks/Development/noun-swift-file-3001056.png" "${CMAKE_BINARY_DIR}/assets/icons/swift.png"
    COMMAND ${CMAKE_COMMAND} -E copy "${base-icons_SOURCE_DIR}/masks/Development/Rust_programming_language.png" "${CMAKE_BINARY_DIR}/assets/icons/rust.png"
    COMMAND ${CMAKE_COMMAND} -E copy "${base-icons_SOURCE_DIR}/masks/Development/noun-python-1375869.png" "${CMAKE_BINARY_DIR}/assets/icons/python.png"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_LIST_DIR}/../themes/dark/css_variables.css" "${CMAKE_BINARY_DIR}/themes/dark/css_variables.css"
    DEPENDS
        "${base-icons_SOURCE_DIR}/os_folders/windows/11/folder_main.png"
        "${CMAKE_CURRENT_LIST_DIR}/../static/assets/file.png"
        "${base-icons_SOURCE_DIR}/masks/Others/hard-drive.png"
        "${base-icons_SOURCE_DIR}/masks/Development/noun-c-4921409.png"
        "${base-icons_SOURCE_DIR}/masks/Development/css-3-logo.png"
        "${base-icons_SOURCE_DIR}/masks/Development/noun-html-1174764.png"
        "${base-icons_SOURCE_DIR}/masks/Development/Go-Logo_Black.png"
        "${base-icons_SOURCE_DIR}/masks/Development/noun-c-4921443.png"
        "${base-icons_SOURCE_DIR}/masks/Development/noun-cad-4921405.png"
        "${base-icons_SOURCE_DIR}/masks/Development/noun-c-file-115671.png"
        "${base-icons_SOURCE_DIR}/masks/Development/noun-jar-4921452.png"
        "${base-icons_SOURCE_DIR}/masks/Development/noun-java-1156842.png"
        "${base-icons_SOURCE_DIR}/masks/Development/noun-js-4921450.png"
        "${base-icons_SOURCE_DIR}/masks/Development/noun-log-4921382.png"
        "${base-icons_SOURCE_DIR}/masks/Development/noun-sql-4921378.png"
        "${base-icons_SOURCE_DIR}/masks/Development/noun-swift-file-3001056.png"
        "${base-icons_SOURCE_DIR}/masks/Development/Rust_programming_language.png"
        "${base-icons_SOURCE_DIR}/masks/Development/noun-python-1375869.png"
        "${CMAKE_CURRENT_LIST_DIR}/../themes/dark/css_variables.css"
)

add_custom_target(
    nui-sftp-resource-copy
    ALL
    DEPENDS
        "${CMAKE_BINARY_DIR}/assets/icons/folder_main.png"
)