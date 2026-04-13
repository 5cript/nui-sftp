#!/usr/bin/env bash
# Generates every fixture in one go under ./sync-fixtures/ (or an override root
# passed as $1).  Useful to regenerate the full set before a test session.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

ROOT="${1:-$PWD/sync-fixtures}"
mkdir -p -- "$ROOT"

for fx in basic nested mtime gitignore symlinks random; do
    bash "$SCRIPT_DIR/$fx.sh" "$ROOT/$fx"
done

printf '\nAll fixtures generated under: %s\n' "$ROOT"
