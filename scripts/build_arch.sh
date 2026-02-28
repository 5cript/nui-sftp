#!/bin/bash

set -e
set -u

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
source "${SCRIPT_DIR}/lib.sh"

SOURCE_DIR="${SOURCE_DIRECTORY:-${SCRIPT_DIR}/..}"
SOURCE_DIR=$(canonicalPath "${SOURCE_DIR}")

BUILD_DIR="${BUILD_DIRECTORY:-${SOURCE_DIR}/build/archpkg}"
BUILD_DIR=$(canonicalPath "${BUILD_DIR}")

mkdir -p "${BUILD_DIR}"

cd "${BUILD_DIR}"
rm "${BUILD_DIR}/PKGBUILD" || true
ln -s "${SOURCE_DIR}/PKGBUILD" PKGBUILD

makepkg -c