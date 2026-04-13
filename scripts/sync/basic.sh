#!/usr/bin/env bash
# Flat fixture with every basic diff category represented exactly once.
#
#   identical.txt      same on both sides, same size, same mtime      -> no action
#   content_diff.txt   same size, different content, different mtime  -> upload or download
#   size_diff.txt      different size and content                     -> upload or download
#   local_only.txt     only exists locally                            -> upload
#   remote_only.txt    only exists remotely                           -> download
#   older_local.txt    local mtime older than remote, content differs -> download (remote newer)
#   newer_local.txt    local mtime newer than remote, content differs -> upload   (local newer)

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/_common.sh"

ROOT="$(fixture_root "basic" "${1:-}")"
reset_pair "$ROOT"
fixture_header "basic" "$ROOT"

# identical — byte-for-byte equal, same mtime
mkf_at "$ROOT/local/identical.txt"  "hello world"  "202601010900"
mkf_at "$ROOT/remote/identical.txt" "hello world"  "202601010900"

# content differs but size matches (same length)
mkf_at "$ROOT/local/content_diff.txt"  "aaaaa bbbbb"  "202603150900"
mkf_at "$ROOT/remote/content_diff.txt" "xxxxx yyyyy"  "202603160900"

# size differs
mkf_at "$ROOT/local/size_diff.txt"  "tiny"             "202604010900"
mkf_at "$ROOT/remote/size_diff.txt" "tiny plus extra"  "202604010900"

# one-side-only
mkf_at "$ROOT/local/local_only.txt"   "local side"   "202604020900"
mkf_at "$ROOT/remote/remote_only.txt" "remote side"  "202604020900"

# remote is newer (download direction on Both)
mkf_at "$ROOT/local/older_local.txt"  "old payload"  "202601010900"
mkf_at "$ROOT/remote/older_local.txt" "new payload"  "202612010900"

# local is newer (upload direction on Both)
mkf_at "$ROOT/local/newer_local.txt"  "new payload"  "202612010900"
mkf_at "$ROOT/remote/newer_local.txt" "old payload"  "202601010900"

summarize_pair "$ROOT"
