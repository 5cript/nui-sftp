#!/usr/bin/env bash
# mtime-focused fixture: every file has identical bytes on both sides but a
# different modification time.  Lets you verify the diff picks the right
# direction (upload / download) based on "newer wins" in Both mode.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/_common.sh"

ROOT="$(fixture_root "mtime" "${1:-}")"
reset_pair "$ROOT"
fixture_header "mtime" "$ROOT"

# local newer by a year
mkf_at "$ROOT/local/local_newer.txt"  "identical bytes"  "202612010900"
mkf_at "$ROOT/remote/local_newer.txt" "identical bytes"  "202512010900"

# remote newer by a year
mkf_at "$ROOT/local/remote_newer.txt"  "identical bytes"  "202512010900"
mkf_at "$ROOT/remote/remote_newer.txt" "identical bytes"  "202612010900"

# same second — should diff as unchanged
mkf_at "$ROOT/local/same_mtime.txt"  "identical bytes"  "202601010900"
mkf_at "$ROOT/remote/same_mtime.txt" "identical bytes"  "202601010900"

# nested — mtime newer only at leaf
mkf_at "$ROOT/local/nested/leaf.txt"  "identical bytes"  "202512010900"
mkf_at "$ROOT/remote/nested/leaf.txt" "identical bytes"  "202612010900"

summarize_pair "$ROOT"
