#!/usr/bin/env bash
# Multi-level nested fixture.  Exercises the recursive scan, per-level ignore
# rules, and the "parent directory missing" path on both sides.
#
# Intentional differences:
#   src/a/same.c              identical on both sides
#   src/a/diff.c              content differs, remote newer
#   src/a/local_only.c        local-only; remote lacks whole "src/a" until sync
#   src/b/deep/x/y/z/new.log  exists only locally; remote has no "src/b" tree
#   docs/README.md            identical content but different mtime
#   docs/remote_only.md       remote-only, tests download of a nested file
#   build/out.bin             exists both sides (should be ignored if .gitignore respected)

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/_common.sh"

ROOT="$(fixture_root "nested" "${1:-}")"
reset_pair "$ROOT"
fixture_header "nested" "$ROOT"

# Local side
mkf_at "$ROOT/local/src/a/same.c"             "int same(void){return 0;}"  "202602100900"
mkf_at "$ROOT/local/src/a/diff.c"             "int v(void){return 1;}"     "202602110900"
mkf_at "$ROOT/local/src/a/local_only.c"       "// local scratch"           "202602120900"
mkf_at "$ROOT/local/src/b/deep/x/y/z/new.log" "fresh log line"             "202604050900"
mkf_at "$ROOT/local/docs/README.md"           "# project"                  "202602100900"
mkf_at "$ROOT/local/build/out.bin"            "LOCAL_BUILD_ARTIFACT"       "202604070900"
mkf    "$ROOT/local/.gitignore"               $'build/\n*.log\n'

# Remote side
mkf_at "$ROOT/remote/src/a/same.c"             "int same(void){return 0;}"  "202602100900"
mkf_at "$ROOT/remote/src/a/diff.c"             "int v(void){return 2;}"     "202602200900"
mkf_at "$ROOT/remote/docs/README.md"           "# project"                  "202603100900"
mkf_at "$ROOT/remote/docs/remote_only.md"      "only on server"             "202604030900"
mkf_at "$ROOT/remote/build/out.bin"            "REMOTE_BUILD_ARTIFACT"      "202604070900"
mkf    "$ROOT/remote/.gitignore"               $'build/\n*.log\n'

summarize_pair "$ROOT"
