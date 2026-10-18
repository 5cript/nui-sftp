#!/bin/bash

set -e
set -u

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

source "${SCRIPT_DIR}/lib.sh"

# Variables that can also be args from env

#### INSTALL_TARGET
# The location where the application resources etc are installed
# Some good locations are:
# - Arch: /opt/nui-sftp
# - Flatpak: /app
INSTALL_TARGET="${INSTALL_TARGET:-${SCRIPT_DIR}/../build/install}"
INSTALL_TARGET=$(canonicalPath "${INSTALL_TARGET}")

BUILD_DIRECTORY="${BUILD_DIRECTORY:-${SCRIPT_DIR}/../build/clang_release}"
BUILD_DIRECTORY=$(canonicalPath "${BUILD_DIRECTORY}")

SOURCE_DIRECTORY="${SOURCE_DIRECTORY:-${SCRIPT_DIR}/..}"
SOURCE_DIRECTORY=$(canonicalPath "${SOURCE_DIRECTORY}")

OMIT_FRONTEND="${OMIT_FRONTEND:-false}"

NOLINK="${NOLINK:-false}"

PLATFORM="$(uname -s)"

if [ "${PLATFORM}" = "Darwin" ] && ! grep -qs '^NUI_BROWSER_ENGINE:[A-Z]*=cef$' "${BUILD_DIRECTORY}/CMakeCache.txt"; then
    INSTALL_TARGET="${INSTALL_TARGET}" BUILD_DIRECTORY="${BUILD_DIRECTORY}" SOURCE_DIRECTORY="${SOURCE_DIRECTORY}" \
        OMIT_FRONTEND="${OMIT_FRONTEND}" bash "${SCRIPT_DIR}/deploy_macos.sh"
    exit 0
fi

# Linux, and macOS with CEF: the install rules of the CMake project (nui_install). They leave out the frontend when the
# build has OMIT_FRONTEND_BUILD.
if [[ "${PLATFORM}" != *_NT-* ]]; then
    echo -e "Installing to \e[32m${INSTALL_TARGET}\e[0m"
    cmake --install "${BUILD_DIRECTORY}" --prefix "${INSTALL_TARGET}"
    if [ "$NOLINK" = false ] && [ "${PLATFORM}" != "Darwin" ]; then
        ln -sfn "./bin/nui-sftp" "${INSTALL_TARGET}/nui-sftp"
    fi
    exit 0
fi

# Windows (MSYS2): the executable with the clang runtime libraries of MSYS2 next to it.
EXECUTABLE="${BUILD_DIRECTORY}/bin/nui-sftp.exe"

echo -e "Installing to \e[32m${INSTALL_TARGET}\e[0m"

mkdir -p "${INSTALL_TARGET}/bin"
mkdir -p "${INSTALL_TARGET}/frontend"
mkdir -p "${INSTALL_TARGET}/assets/icons"
mkdir -p "${INSTALL_TARGET}/themes"

cp "${EXECUTABLE}" "${INSTALL_TARGET}/bin/nui-sftp.exe"
ldd "${EXECUTABLE}" | grep "clang" | awk 'NF == 4 { system("cp " $3 " '"${INSTALL_TARGET}/bin/"'") }'

# Assets (Images, icons, language files)...
cp -r "${SOURCE_DIRECTORY}/static/assets/." "${INSTALL_TARGET}/assets"

# Generated .ico, used by the Inno Setup installer as SetupIconFile and Start Menu shortcut icon.
if [ -f "${BUILD_DIRECTORY}/generated/icons/nui-sftp.ico" ]; then
    cp "${BUILD_DIRECTORY}/generated/icons/nui-sftp.ico" "${INSTALL_TARGET}/assets/icons/"
fi

cp -r "${SOURCE_DIRECTORY}/themes/." "${INSTALL_TARGET}/themes"
if [ "$OMIT_FRONTEND" = false ]; then
    cp -r "${BUILD_DIRECTORY}/module_nui-sftp/bin/." "${INSTALL_TARGET}/frontend"
fi
