#!/bin/bash

set -e
set -u

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
source "${SCRIPT_DIR}/lib.sh"

SOURCE_DIR="${SOURCE_DIRECTORY:-${SCRIPT_DIR}/..}"
SOURCE_DIR=$(canonicalPath "${SOURCE_DIR}")


flatpak-builder --force-clean --install-deps-from=flathub build/flatpak org.nuicpp.nui_sftp.yml
flatpak build-export build/flatpak-export build/flatpak
flatpak build-bundle build/flatpak-export build/nui-sftp.flatpak org.nuicpp.nui_sftp --runtime-repo=https://flathub.org/repo/flathub.flatpakrepo