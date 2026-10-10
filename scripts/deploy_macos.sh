#!/bin/bash
# Assembles nui-sftp.app from a macOS build, called by deploy.sh.
# Layout: Contents/MacOS/nui-sftp, Contents/Resources/{frontend,assets,themes}, Contents/Frameworks/*.dylib.
# The bundle is ad-hoc signed, there is no Developer ID yet.

set -e
set -u

: "${INSTALL_TARGET:?}" "${BUILD_DIRECTORY:?}" "${SOURCE_DIRECTORY:?}" "${OMIT_FRONTEND:?}"

BUNDLE="${INSTALL_TARGET}/nui-sftp.app"
CONTENTS="${BUNDLE}/Contents"
RESOURCES="${CONTENTS}/Resources"

echo -e "Assembling \e[32m${BUNDLE}\e[0m"

rm -rf "${BUNDLE}"
mkdir -p "${CONTENTS}/MacOS" "${CONTENTS}/Frameworks" "${RESOURCES}/frontend" "${RESOURCES}/assets/icons" "${RESOURCES}/themes"

cp "${BUILD_DIRECTORY}/bin/nui-sftp" "${CONTENTS}/MacOS/nui-sftp"
cp "${BUILD_DIRECTORY}/generated/Info.plist" "${CONTENTS}/Info.plist"
cp "${BUILD_DIRECTORY}/generated/icons/nui-sftp.icns" "${RESOURCES}/nui-sftp.icns"

cp -R "${SOURCE_DIRECTORY}/static/assets/." "${RESOURCES}/assets"
cp "${SOURCE_DIRECTORY}/static/assets/icons/nui-sftp-logo.svg" "${RESOURCES}/assets/icons/"
cp -R "${SOURCE_DIRECTORY}/themes/." "${RESOURCES}/themes"
if [ "$OMIT_FRONTEND" = false ]; then
    cp -R "${BUILD_DIRECTORY}/module_nui-sftp/bin/." "${RESOURCES}/frontend"
fi

# Homebrew libraries (boost, libssh, openssl, fmt, ...) move into the bundle, system libraries stay.
dylibbundler --overwrite-files --bundle-deps --create-dir \
    --fix-file "${CONTENTS}/MacOS/nui-sftp" \
    --dest-dir "${CONTENTS}/Frameworks" \
    --install-path "@executable_path/../Frameworks/" \
    >/dev/null

codesign --force --deep --sign - "${BUNDLE}"
codesign --verify --deep --strict "${BUNDLE}"
