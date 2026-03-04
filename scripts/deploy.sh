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
mkdir -p "${INSTALL_TARGET}/themes/dark"

cp "${EXECUTABLE}" "${INSTALL_TARGET}/bin/${EXECUTABLE_NAME}"
cp -r "${BUILD_DIRECTORY}/assets/." "${INSTALL_TARGET}/assets"
cp -r "${BUILD_DIRECTORY}/themes/." "${INSTALL_TARGET}/themes"
if [ "$OMIT_FRONTEND" = false ]; then
    cp -r "${BUILD_DIRECTORY}/module_nui-sftp/bin/." "${INSTALL_TARGET}/frontend"
fi

# Dont create a symlink on windows or if NOLINK is true
if [ "$IS_WINDOWS" = false ] && [ "$NOLINK" = false ]; then
    ln -s "./bin/${EXECUTABLE_NAME}" "${INSTALL_TARGET}/${EXECUTABLE_NAME}"
fi
