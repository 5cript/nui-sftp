#!/bin/bash
# Assembles nui-sftp.app from a macOS build, called by deploy.sh.
# Layout: Contents/MacOS/nui-sftp, Contents/Resources/{frontend,assets,themes}.
# The bundle is ad-hoc signed, there is no Developer ID yet.

set -e
set -u

: "${INSTALL_TARGET:?}" "${BUILD_DIRECTORY:?}" "${SOURCE_DIRECTORY:?}" "${OMIT_FRONTEND:?}"

BUNDLE="${INSTALL_TARGET}/nui-sftp.app"
CONTENTS="${BUNDLE}/Contents"
RESOURCES="${CONTENTS}/Resources"

echo -e "Assembling \e[32m${BUNDLE}\e[0m"

rm -rf "${BUNDLE}"
mkdir -p "${CONTENTS}/MacOS" "${RESOURCES}/frontend" "${RESOURCES}/assets" "${RESOURCES}/themes"

cp "${BUILD_DIRECTORY}/bin/nui-sftp" "${CONTENTS}/MacOS/nui-sftp"
cp "${BUILD_DIRECTORY}/generated/Info.plist" "${CONTENTS}/Info.plist"
cp "${BUILD_DIRECTORY}/generated/icons/nui-sftp.icns" "${RESOURCES}/nui-sftp.icns"

cp -R "${SOURCE_DIRECTORY}/static/assets/." "${RESOURCES}/assets"
cp -R "${SOURCE_DIRECTORY}/themes/." "${RESOURCES}/themes"
if [ "$OMIT_FRONTEND" = false ]; then
    cp -R "${BUILD_DIRECTORY}/module_nui-sftp/bin/." "${RESOURCES}/frontend"
fi

# Everything but the OS is linked statically (scripts/macos/build_dependencies.sh), libc++ included.
LINKED_LIBRARIES=$(otool -L "${CONTENTS}/MacOS/nui-sftp" | tail -n +2 | awk '{print $1}')
FOREIGN_LIBRARIES=$(
    grep -E -v '^(/usr/lib/|/System/Library/)' <<< "${LINKED_LIBRARIES}" || true
    grep -E '^/usr/lib/libc\+\+' <<< "${LINKED_LIBRARIES}" || true
)
if [ -n "${FOREIGN_LIBRARIES}" ]; then
    echo "nui-sftp links libraries that are not part of the app or the OS:" >&2
    echo "${FOREIGN_LIBRARIES}" >&2
    exit 1
fi

codesign --force --deep --sign - "${BUNDLE}"
codesign --verify --deep --strict "${BUNDLE}"
