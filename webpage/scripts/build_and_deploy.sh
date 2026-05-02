#!/usr/bin/env bash
# ============================================================================
# Build the webpage and deploy it to a remote server over SSH (via rsync).
#
# Required environment variables:
#   DEPLOY_HOST   Remote hostname or IP (e.g. example.com)
#   DEPLOY_USER   SSH user to log in as
#   DEPLOY_PORT   SSH port number
#
# Optional environment variables:
#   DEPLOY_PATH       Remote target directory. Default: ~/nui-sftp-webpage
#                     (Tilde and shell vars are expanded on the remote side.
#                     Avoid spaces.)
#   BUILD_VARIANT     CMake build dir name under <repo>/build.
#                     Default: clang_webpage
#   SKIP_BUILD        Set to "1" to skip configure/build and just deploy
#                     whatever is already in the dist dir.
#   RSYNC_EXTRA_ARGS  Extra arguments forwarded to rsync (e.g. "--delete").
#   DEPLOY_OWNER      If set (e.g. "http:http"), the remote rsync runs under
#                     `sudo` and writes files with the given owner:group.
#                     Requires DEPLOY_USER to have sudo for `rsync` (and
#                     `mkdir`) on the remote, ideally NOPASSWD.
#
# Example:
#   DEPLOY_HOST=example.com DEPLOY_USER=tim DEPLOY_PORT=22 \
#       DEPLOY_PATH=/srv/http/nui-sftp DEPLOY_OWNER=http:http \
#       bash webpage/scripts/build_and_deploy.sh
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
WEBPAGE_DIR="$(cd -- "${SCRIPT_DIR}/.." &>/dev/null && pwd)"
ROOT_DIR="$(cd -- "${WEBPAGE_DIR}/.." &>/dev/null && pwd)"

require_env() {
    local var_name="$1"
    if [ -z "${!var_name:-}" ]; then
        echo "Error: required environment variable '${var_name}' is not set." >&2
        echo "See the variable reference at the top of $(basename "${BASH_SOURCE[0]}")." >&2
        exit 1
    fi
}

require_env DEPLOY_HOST
require_env DEPLOY_USER
require_env DEPLOY_PORT

DEPLOY_PATH="${DEPLOY_PATH:-~/nui-sftp-webpage}"
BUILD_VARIANT="${BUILD_VARIANT:-clang_webpage}"
SKIP_BUILD="${SKIP_BUILD:-0}"
RSYNC_EXTRA_ARGS="${RSYNC_EXTRA_ARGS:-}"
DEPLOY_OWNER="${DEPLOY_OWNER:-}"

BUILD_DIR="${ROOT_DIR}/build/${BUILD_VARIANT}"
DIST_DIR="${BUILD_DIR}/module_nui-sftp-webpage/bin"

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
if [ "${SKIP_BUILD}" != "1" ]; then
    if [ ! -f "${BUILD_DIR}/CMakeCache.txt" ]; then
        echo -e "Configuring cmake build in \e[36m${BUILD_DIR}\e[0m"
        cmake -S "${WEBPAGE_DIR}" -B "${BUILD_DIR}" \
            -G Ninja \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_C_COMPILER=clang \
            -DCMAKE_CXX_COMPILER=clang++
    fi
    echo -e "Building \e[36mnui-sftp-webpage-emscripten\e[0m"
    cmake --build "${BUILD_DIR}" --target nui-sftp-webpage-emscripten
fi

if [ ! -d "${DIST_DIR}" ]; then
    echo "Dist dir not found: ${DIST_DIR}" >&2
    echo "Build produced no output (or SKIP_BUILD was set without a prior build)." >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Deploy
# ---------------------------------------------------------------------------
echo -e "Deploying \e[32m${DIST_DIR}/\e[0m"
echo -e "       to \e[32m${DEPLOY_USER}@${DEPLOY_HOST}:${DEPLOY_PATH}\e[0m (port ${DEPLOY_PORT})"
if [ -n "${DEPLOY_OWNER}" ]; then
    echo -e "    owner \e[32m${DEPLOY_OWNER}\e[0m (via sudo on the remote)"
fi

# Build a remote-shell prefix and rsync-path depending on whether we need
# the remote side to run as root (to chown to a different user/group).
if [ -n "${DEPLOY_OWNER}" ]; then
    REMOTE_MKDIR="sudo mkdir -p ${DEPLOY_PATH}"
    REMOTE_RSYNC_PATH=(--rsync-path="sudo rsync" "--chown=${DEPLOY_OWNER}")
else
    REMOTE_MKDIR="mkdir -p ${DEPLOY_PATH}"
    REMOTE_RSYNC_PATH=()
fi

# Make sure the remote directory exists. DEPLOY_PATH is left unquoted on the
# remote side so the remote shell expands ~ and any env vars.
ssh -p "${DEPLOY_PORT}" "${DEPLOY_USER}@${DEPLOY_HOST}" "${REMOTE_MKDIR}"

rsync -avz \
    -e "ssh -p ${DEPLOY_PORT}" \
    "${REMOTE_RSYNC_PATH[@]}" \
    --exclude='*.map' \
    ${RSYNC_EXTRA_ARGS} \
    "${DIST_DIR}/" \
    "${DEPLOY_USER}@${DEPLOY_HOST}:${DEPLOY_PATH}/"

echo -e "\e[32mDeploy complete.\e[0m"
