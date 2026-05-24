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

# Optional: directory containing extra icons (e.g. extracted icons.tar.gz).
# When set, all contents are copied into assets/icons/ and folder_main.png is
# placed directly in assets/icons/ for quick access.
ICONS_SOURCE="${ICONS_SOURCE:-}"

# On Windows executeable is called nui-sftp.exe, look if that exsists and then use that as the source for the executable
# Also set a variable for future reference.

if [ -f "${BUILD_DIRECTORY}/bin/nui-sftp.exe" ]; then
    EXECUTABLE="${BUILD_DIRECTORY}/bin/nui-sftp.exe"
    EXECUTABLE_NAME="nui-sftp.exe"
    IS_WINDOWS=true
else
    EXECUTABLE="${BUILD_DIRECTORY}/bin/nui-sftp"
    EXECUTABLE_NAME="nui-sftp"
    IS_WINDOWS=false
fi

echo -e "Installing to \e[32m${INSTALL_TARGET}\e[0m"

# Create the install directory if it doesn't exist
mkdir -p "${INSTALL_TARGET}"
mkdir -p "${INSTALL_TARGET}/bin"
mkdir -p "${INSTALL_TARGET}/frontend"
mkdir -p "${INSTALL_TARGET}/assets"
mkdir -p "${INSTALL_TARGET}/themes"
mkdir -p "${INSTALL_TARGET}/assets/icons"

cp "${EXECUTABLE}" "${INSTALL_TARGET}/bin/${EXECUTABLE_NAME}"
if [ "$IS_WINDOWS" = true ]; then
    # use ldd to copy dependencies from msys2 into the bin dir
    ldd "${EXECUTABLE}" | grep "clang" | awk 'NF == 4 { system("cp " $3 " '"${INSTALL_TARGET}/bin/"'") }'
fi

# Assets (Images, icons, language files)...
cp -r "${SOURCE_DIRECTORY}/static/assets/." "${INSTALL_TARGET}/assets"
cp "${SOURCE_DIRECTORY}/static/assets/icons/file.png" "${INSTALL_TARGET}/assets/icons/"
cp "${SOURCE_DIRECTORY}/static/assets/icons/nui-sftp-logo.svg" "${INSTALL_TARGET}/assets/icons/"

# Generated Windows .ico (only present on Windows builds; used by the Inno Setup
# installer as SetupIconFile and Start Menu shortcut icon).
if [ -f "${BUILD_DIRECTORY}/generated/icons/nui-sftp.ico" ]; then
    cp "${BUILD_DIRECTORY}/generated/icons/nui-sftp.ico" "${INSTALL_TARGET}/assets/icons/"
fi

# Extra icons bundle (e.g. OS folder icons).
if [ -n "${ICONS_SOURCE}" ] && [ -d "${ICONS_SOURCE}" ]; then
    cp -r "${ICONS_SOURCE}/." "${INSTALL_TARGET}/assets/icons/"
    # Also expose folder_main directly in assets/icons/ for easy lookup.
    if [ -f "${ICONS_SOURCE}/os_folders/windows/11/folder_main.png" ]; then
        cp "${ICONS_SOURCE}/os_folders/windows/11/folder_main.png" "${INSTALL_TARGET}/assets/icons/folder_main.png"
    fi
fi

cp -r "${SOURCE_DIRECTORY}/themes/." "${INSTALL_TARGET}/themes"
if [ "$OMIT_FRONTEND" = false ]; then
    cp -r "${BUILD_DIRECTORY}/module_nui-sftp/bin/." "${INSTALL_TARGET}/frontend"
fi

if [ "$NOLINK" = false ]; then
    if [ "$IS_WINDOWS" = false ]; then
        # Dont create a symlink on windows or if NOLINK is true
        ln -s "./bin/${EXECUTABLE_NAME}" "${INSTALL_TARGET}/${EXECUTABLE_NAME}"
    else
        # TODO?
        :
    fi
fi
