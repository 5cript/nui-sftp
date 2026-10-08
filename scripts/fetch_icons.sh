#!/bin/bash

# Downloads the icon bundle (file type and OS folder icons) into the per-user state
# directory (%state_home2%/nui-sftp/assets/icons), which every build searches. Packages
# extract the same archive into assets/icons themselves, so this is only needed for
# builds from a checkout.

set -e
set -u

ICONS_URL="https://s3.g.s4.mega.io/jgemkib4a5fte35rktt5wxrwkw4ejk4ybemkf/nui-scp/icons.tar.gz"
ICONS_SHA256="30ffa48c3a509e878db31a1e5d80376242852e34d9c2aa3b44d2e3d1da2ce32e"
if [ -n "${MSYSTEM:-}" ]; then
    STATE_DIRECTORY=$(cygpath -u "$(powershell.exe -NoProfile -Command "[Environment]::GetFolderPath('MyDocuments')" | tr -d '\r')")
else
    STATE_DIRECTORY="${XDG_STATE_HOME:-${HOME}/.local/state}"
fi
ICONS_DIRECTORY="${STATE_DIRECTORY}/nui-sftp/assets/icons"
STAMP_FILE="${ICONS_DIRECTORY}/.bundle-sha256"

if [ -f "${STAMP_FILE}" ] && [ "$(cat "${STAMP_FILE}")" = "${ICONS_SHA256}" ]; then
    echo "Icon bundle is up to date"
    exit 0
fi

ARCHIVE=$(mktemp)
trap 'rm -f "${ARCHIVE}"' EXIT

echo "Downloading the icon bundle"
curl -fsSL --output "${ARCHIVE}" "${ICONS_URL}"
echo "${ICONS_SHA256}  ${ARCHIVE}" | sha256sum --check --status \
    || { echo "Icon bundle checksum mismatch" >&2; exit 1; }

mkdir -p "${ICONS_DIRECTORY}"
rm -rf "${ICONS_DIRECTORY}/masks" "${ICONS_DIRECTORY}/os_folders"
tar -xzf "${ARCHIVE}" -C "${ICONS_DIRECTORY}"
echo "${ICONS_SHA256}" > "${STAMP_FILE}"
echo "Icon bundle extracted into ${ICONS_DIRECTORY}"
