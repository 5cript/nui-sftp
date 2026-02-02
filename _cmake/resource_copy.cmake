# Icons

include(FetchContent)

FetchContent_Declare(
    base-icons
    URL "https://mega.nz/file/d6wnESCB#lt_MKiLff7nQ65XKhFZe9pf5uYqbOXJtGiiQs4xfiAc"
    URL_HASH MD5=38d01d8769dda2a006495c7a3caa11b4
)

FetchContent_MakeAvailable(base-icons)

add_custom_command(
    OUTPUT
        "${CMAKE_BINARY_DIR}/bin/assets/icons/folder_main.png"
        "${CMAKE_BINARY_DIR}/bin/assets/icons/file.png"
        "${CMAKE_BINARY_DIR}/bin/assets/icons/hard_drive.png"
        "${CMAKE_BINARY_DIR}/bin/assets/icons/cpp.png"
        "${CMAKE_BINARY_DIR}/bin/assets/icons/css.png"
        "${CMAKE_BINARY_DIR}/bin/assets/icons/go.png"
        "${CMAKE_BINARY_DIR}/bin/assets/icons/csharp.png"
        "${CMAKE_BINARY_DIR}/bin/assets/icons/cad.png"
        "${CMAKE_BINARY_DIR}/bin/assets/icons/c.png"
        "${CMAKE_BINARY_DIR}/bin/assets/icons/html.png"
        "${CMAKE_BINARY_DIR}/bin/assets/icons/jar.png"
        "${CMAKE_BINARY_DIR}/bin/assets/icons/java.png"
        "${CMAKE_BINARY_DIR}/bin/assets/icons/js.png"
        "${CMAKE_BINARY_DIR}/bin/assets/icons/log.png"
        "${CMAKE_BINARY_DIR}/bin/assets/icons/sql.png"
        "${CMAKE_BINARY_DIR}/bin/assets/icons/swift.png"
        "${CMAKE_BINARY_DIR}/bin/assets/icons/rust.png"
        "${CMAKE_BINARY_DIR}/bin/assets/icons/python.png"
    COMMAND ${CMAKE_COMMAND} -E copy "${base-icons_SOURCE_DIR}/os_folders/windows/11/folder_main.png" "${CMAKE_BINARY_DIR}/bin/assets/icons/folder_main.png"
    COMMAND ${CMAKE_COMMAND} -E copy "${base-icons_SOURCE_DIR}/masks/Others/hard-drive.png" "${CMAKE_BINARY_DIR}/bin/assets/icons/hard_drive.png"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_LIST_DIR}/../static/assets/file.png" "${CMAKE_BINARY_DIR}/bin/assets/icons/file.png"
    COMMAND ${CMAKE_COMMAND} -E copy "${base-icons_SOURCE_DIR}/masks/Development/noun-c-4921409.png" "${CMAKE_BINARY_DIR}/bin/assets/icons/cpp.png"
    COMMAND ${CMAKE_COMMAND} -E copy "${base-icons_SOURCE_DIR}/masks/Development/css-3-logo.png" "${CMAKE_BINARY_DIR}/bin/assets/icons/css.png"
    COMMAND ${CMAKE_COMMAND} -E copy "${base-icons_SOURCE_DIR}/masks/Development/noun-html-1174764.png" "${CMAKE_BINARY_DIR}/bin/assets/icons/html.png"
    COMMAND ${CMAKE_COMMAND} -E copy "${base-icons_SOURCE_DIR}/masks/Development/Go-Logo_Black.png" "${CMAKE_BINARY_DIR}/bin/assets/icons/go.png"
    COMMAND ${CMAKE_COMMAND} -E copy "${base-icons_SOURCE_DIR}/masks/Development/noun-c-4921443.png" "${CMAKE_BINARY_DIR}/bin/assets/icons/csharp.png"
    COMMAND ${CMAKE_COMMAND} -E copy "${base-icons_SOURCE_DIR}/masks/Development/noun-cad-4921405.png" "${CMAKE_BINARY_DIR}/bin/assets/icons/cad.png"
    COMMAND ${CMAKE_COMMAND} -E copy "${base-icons_SOURCE_DIR}/masks/Development/noun-c-file-115671.png" "${CMAKE_BINARY_DIR}/bin/assets/icons/c.png"
    COMMAND ${CMAKE_COMMAND} -E copy "${base-icons_SOURCE_DIR}/masks/Development/noun-jar-4921452.png" "${CMAKE_BINARY_DIR}/bin/assets/icons/jar.png"
    COMMAND ${CMAKE_COMMAND} -E copy "${base-icons_SOURCE_DIR}/masks/Development/noun-java-1156842.png" "${CMAKE_BINARY_DIR}/bin/assets/icons/java.png"
    COMMAND ${CMAKE_COMMAND} -E copy "${base-icons_SOURCE_DIR}/masks/Development/noun-js-4921450.png" "${CMAKE_BINARY_DIR}/bin/assets/icons/js.png"
    COMMAND ${CMAKE_COMMAND} -E copy "${base-icons_SOURCE_DIR}/masks/Development/noun-log-4921382.png" "${CMAKE_BINARY_DIR}/bin/assets/icons/log.png"
    COMMAND ${CMAKE_COMMAND} -E copy "${base-icons_SOURCE_DIR}/masks/Development/noun-sql-4921378.png" "${CMAKE_BINARY_DIR}/bin/assets/icons/sql.png"
    COMMAND ${CMAKE_COMMAND} -E copy "${base-icons_SOURCE_DIR}/masks/Development/noun-swift-file-3001056.png" "${CMAKE_BINARY_DIR}/bin/assets/icons/swift.png"
    COMMAND ${CMAKE_COMMAND} -E copy "${base-icons_SOURCE_DIR}/masks/Development/Rust_programming_language.png" "${CMAKE_BINARY_DIR}/bin/assets/icons/rust.png"
    COMMAND ${CMAKE_COMMAND} -E copy "${base-icons_SOURCE_DIR}/masks/Development/noun-python-1375869.png" "${CMAKE_BINARY_DIR}/bin/assets/icons/python.png"
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
)

add_custom_target(
    scp-resource-copy
    ALL
    DEPENDS
        "${CMAKE_BINARY_DIR}/bin/assets/icons/folder_main.png"
)