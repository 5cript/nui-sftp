#!/bin/bash

set -e
set -u

function canonicalPath
{
    local path="$1" ; shift
    if [ -d "$path" ]
    then
        echo "$(cd "$path" ; pwd)"
    else
        local b=$(basename "$path")
        local p=$(dirname "$path")
        echo "$(cd "$p" ; pwd)/$b"
    fi
}

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

# Variables that can also be args from env
INSTALL_TARGET="${INSTALL_TARGET:-${SCRIPT_DIR}/../build/install}"
INSTALL_TARGET=$(canonicalPath "${INSTALL_TARGET}")

BUILD_DIRECTORY="${BUILD_DIRECTORY:-${SCRIPT_DIR}/../build/clang_release}"
BUILD_DIRECTORY=$(canonicalPath "${BUILD_DIRECTORY}")

SOURCE_DIRECTORY="${SOURCE_DIRECTORY:-${SCRIPT_DIR}/..}"
SOURCE_DIRECTORY=$(canonicalPath "${SOURCE_DIRECTORY}")

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
mkdir -p "${INSTALL_TARGET}/assets"
mkdir -p "${INSTALL_TARGET}/themes"
mkdir -p "${INSTALL_TARGET}/themes/dark"

cp "${EXECUTABLE}" "${INSTALL_TARGET}/bin/${EXECUTABLE_NAME}"
cp -r "${BUILD_DIRECTORY}/assets/." "${INSTALL_TARGET}/assets"
cp -r "${BUILD_DIRECTORY}/themes/." "${INSTALL_TARGET}/themes"
cp -r "${BUILD_DIRECTORY}/module_nui-sftp/bin/." "${INSTALL_TARGET}/bin"

# Dont create a symlink on windows or if NOLINK is true
if [ "$IS_WINDOWS" = false ] && [ "$NOLINK" = false ]; then
    ln -s "./bin/${EXECUTABLE_NAME}" "${INSTALL_TARGET}/${EXECUTABLE_NAME}"
fi
