#!/bin/env bash
# Live-reload static server for the produced webpage output.
# Watches the parcel dist directory; whenever the C++/Parcel build refreshes
# index.html / index.js / styles, the browser auto-reloads.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
WEBPAGE_DIR="$(cd -- "${SCRIPT_DIR}/.." &>/dev/null && pwd)"
ROOT_DIR="$(cd -- "${WEBPAGE_DIR}/.." &>/dev/null && pwd)"

BUILD_VARIANT="${BUILD_VARIANT:-clang_webpage}"
DIST_DIR="${ROOT_DIR}/build/${BUILD_VARIANT}/module_nui-sftp-webpage/bin"

if [ ! -d "${DIST_DIR}" ]; then
    echo "Dist dir not found: ${DIST_DIR}" >&2
    echo "Build the webpage first (cmake target nui-sftp-webpage)." >&2
    exit 1
fi

cd "${WEBPAGE_DIR}"
exec npx live-server \
    --no-browser \
    --port=3000 \
    --wait=200 \
    "${DIST_DIR}"
