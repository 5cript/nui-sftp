#!/usr/bin/env bash
# Copies the remote half of a fixture to an SFTP host, keeping mtimes and symlinks.
#
# Usage: push-remote.sh <fixture root> <user@host> <remote directory> [ssh args...]
#   push-remote.sh sync-fixtures/deep sftpuser@192.168.178.70 sync-audit/deep -p 5000 -i ~/.ssh/key
#
# The remote directory is replaced (rm -rf) before extracting.

set -euo pipefail

FIXTURE="${1:?fixture root}"
TARGET="${2:?user@host}"
REMOTE_DIR="${3:?remote directory}"
shift 3

if [[ ! -d "$FIXTURE/remote" ]]; then
    printf 'No remote/ directory in %s\n' "$FIXTURE" >&2
    exit 1
fi

tar -C "$FIXTURE/remote" -cf - . |
    ssh "$@" "$TARGET" "rm -rf -- '$REMOTE_DIR' && mkdir -p -- '$REMOTE_DIR' && tar -C '$REMOTE_DIR' -xf -"

printf 'Pushed %s/remote to %s:%s\n' "$FIXTURE" "$TARGET" "$REMOTE_DIR"
