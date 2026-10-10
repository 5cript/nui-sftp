set(NUI_SFTP_SVG "${CMAKE_SOURCE_DIR}/static/assets/icons/nui-sftp-logo.svg")
set(NUI_SFTP_ICONSET "${CMAKE_BINARY_DIR}/generated/icons/nui-sftp.iconset")
set(NUI_SFTP_ICNS "${CMAKE_BINARY_DIR}/generated/icons/nui-sftp.icns")

find_program(MAGICK_EXECUTABLE
    NAMES magick
    DOC "ImageMagick CLI (used to rasterize SVG -> PNG for the macOS app icon)"
)
find_program(ICONUTIL_EXECUTABLE NAMES iconutil DOC "Apple iconutil (packs an .iconset into .icns)")
if (NOT MAGICK_EXECUTABLE OR NOT ICONUTIL_EXECUTABLE)
    message(FATAL_ERROR
        "ImageMagick (magick) and iconutil are required on macOS to generate nui-sftp.icns from the SVG. "
        "Install ImageMagick via Homebrew: brew install imagemagick librsvg"
    )
endif()

set(NUI_SFTP_ICONSET_COMMANDS "")
foreach(size 16 32 128 256 512)
    math(EXPR doubleSize "${size} * 2")
    list(APPEND NUI_SFTP_ICONSET_COMMANDS
        COMMAND ${MAGICK_EXECUTABLE} -background none -density 600 "${NUI_SFTP_SVG}"
                -resize ${size}x${size} "${NUI_SFTP_ICONSET}/icon_${size}x${size}.png"
        COMMAND ${MAGICK_EXECUTABLE} -background none -density 600 "${NUI_SFTP_SVG}"
                -resize ${doubleSize}x${doubleSize} "${NUI_SFTP_ICONSET}/icon_${size}x${size}@2x.png"
    )
endforeach()

add_custom_command(
    OUTPUT  "${NUI_SFTP_ICNS}"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${NUI_SFTP_ICONSET}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${NUI_SFTP_ICONSET}"
    ${NUI_SFTP_ICONSET_COMMANDS}
    COMMAND ${ICONUTIL_EXECUTABLE} -c icns -o "${NUI_SFTP_ICNS}" "${NUI_SFTP_ICONSET}"
    DEPENDS "${NUI_SFTP_SVG}"
    COMMENT "Generating nui-sftp.icns from ${NUI_SFTP_SVG}"
    VERBATIM
)
add_custom_target(nui-sftp-icon DEPENDS "${NUI_SFTP_ICNS}")
