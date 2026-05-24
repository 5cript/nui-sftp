set(NUI_SFTP_SVG "${CMAKE_SOURCE_DIR}/static/assets/icons/nui-sftp-logo.svg")
set(NUI_SFTP_ICO "${CMAKE_BINARY_DIR}/generated/icons/nui-sftp.ico")

find_program(MAGICK_EXECUTABLE
    NAMES magick
    DOC "ImageMagick CLI (used to rasterize SVG -> ICO for the Windows executable resource)"
)
if (NOT MAGICK_EXECUTABLE)
    message(FATAL_ERROR
        "ImageMagick (magick) is required on Windows to generate nui-sftp.ico from the SVG. "
        "Install via MSYS2: pacman -S mingw-w64-clang-x86_64-imagemagick"
    )
endif()

add_custom_command(
    OUTPUT  "${NUI_SFTP_ICO}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated/icons"
    COMMAND ${MAGICK_EXECUTABLE} -background none "${NUI_SFTP_SVG}"
            -define "icon:auto-resize=256,128,64,48,32,24,16"
            "${NUI_SFTP_ICO}"
    DEPENDS "${NUI_SFTP_SVG}"
    COMMENT "Generating nui-sftp.ico from ${NUI_SFTP_SVG}"
    VERBATIM
)
add_custom_target(nui-sftp-icon DEPENDS "${NUI_SFTP_ICO}")
