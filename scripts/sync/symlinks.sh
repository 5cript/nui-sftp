#!/usr/bin/env bash
# Symlink scenarios the sync has to handle cleanly on both sides.
#
#   same_target         local and remote link to the same literal path   -> no diff
#   diff_target         same link name, different targets                 -> upload/download
#   local_link_only     symlink exists only locally                       -> upload
#   remote_link_only    symlink exists only remotely                      -> download
#   dangling            both sides point at a non-existent path           -> equal, no action
#   link_vs_file        one side has a symlink, the other a regular file  -> always diff
#   nested/link         symlink inside a subdirectory                     -> upload/download

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/_common.sh"

ROOT="$(fixture_root "symlinks" "${1:-}")"
reset_pair "$ROOT"
fixture_header "symlinks" "$ROOT"

# A real file that some links can point at.
mkf "$ROOT/local/target.txt"  "real content"
mkf "$ROOT/remote/target.txt" "real content"

# same literal target
mklink "$ROOT/local/same_target"  "target.txt"
mklink "$ROOT/remote/same_target" "target.txt"

# different literal targets
mklink "$ROOT/local/diff_target"  "target.txt"
mklink "$ROOT/remote/diff_target" "other.txt"

# one-side-only
mklink "$ROOT/local/local_link_only"   "target.txt"
mklink "$ROOT/remote/remote_link_only" "target.txt"

# dangling but with the same literal target on both sides
mklink "$ROOT/local/dangling"  "/no/such/path.txt"
mklink "$ROOT/remote/dangling" "/no/such/path.txt"

# type mismatch: link vs regular file
mklink "$ROOT/local/link_vs_file"  "target.txt"
mkf    "$ROOT/remote/link_vs_file" "plain file, not a link"

# nested symlink
mklink "$ROOT/local/nested/link"  "../target.txt"
mklink "$ROOT/remote/nested/link" "../target.txt.moved"

summarize_pair "$ROOT"
