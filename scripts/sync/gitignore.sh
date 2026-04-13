#!/usr/bin/env bash
# Exercises the respect-ignore-files toggle.  All files below are *intentionally
# different* between sides so that they'd appear in the diff without the flag;
# with the flag on they should be filtered out at scan time.
#
# Ignore rules used on both sides:
#   build/           directory-only, anchored at the rule's origin
#   *.log            basename match at any depth
#   /out.tmp         anchored to root
#   !keep/*.log      re-includes keep/*.log
#   __pycache__/     directory-only
#
# Expected behaviour with "Respect ignore files" ON:
#   - build/**, out.tmp, *.log outside keep/, __pycache__/** vanish from the diff
#   - keep/important.log is re-included by the negation
#   - All other files still diff normally

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/_common.sh"

ROOT="$(fixture_root "gitignore" "${1:-}")"
reset_pair "$ROOT"
fixture_header "gitignore" "$ROOT"

IGNORE=$'build/\n*.log\n/out.tmp\n!keep/*.log\n__pycache__/\n'

# Both sides get the same rules file but with differing payloads.
mkf "$ROOT/local/.gitignore"  "$IGNORE"
mkf "$ROOT/remote/.gitignore" "$IGNORE"

# build/ — different on both sides, must be filtered
mkf_at "$ROOT/local/build/out.bin"  "LOCAL"   "202601010900"
mkf_at "$ROOT/remote/build/out.bin" "REMOTE"  "202612010900"

# *.log at root — must be filtered
mkf_at "$ROOT/local/debug.log"  "local"  "202601010900"
mkf_at "$ROOT/remote/debug.log" "remote" "202612010900"

# Nested *.log — must be filtered
mkf_at "$ROOT/local/logs/a.log"  "local"  "202601010900"
mkf_at "$ROOT/remote/logs/a.log" "remote" "202612010900"

# /out.tmp — anchored, must be filtered at root only
mkf_at "$ROOT/local/out.tmp"  "local"  "202601010900"
mkf_at "$ROOT/remote/out.tmp" "remote" "202612010900"
# sub/out.tmp — NOT ignored, different depth
mkf_at "$ROOT/local/sub/out.tmp"  "local"  "202601010900"
mkf_at "$ROOT/remote/sub/out.tmp" "remote" "202612010900"

# keep/important.log — negated, should REAPPEAR in diff
mkf_at "$ROOT/local/keep/important.log"  "local"  "202601010900"
mkf_at "$ROOT/remote/keep/important.log" "remote" "202612010900"

# __pycache__ — typical python noise
mkf_at "$ROOT/local/pkg/__pycache__/mod.pyc"  "local"  "202601010900"
mkf_at "$ROOT/remote/pkg/__pycache__/mod.pyc" "remote" "202612010900"

# Plain non-ignored file — must appear in diff
mkf_at "$ROOT/local/src/main.py"  "print('hi, local')"   "202601010900"
mkf_at "$ROOT/remote/src/main.py" "print('hi, remote')"  "202612010900"

summarize_pair "$ROOT"
