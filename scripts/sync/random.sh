#!/usr/bin/env bash
# Seeded random fixture for stress-testing the diff UI and transfer queue.
# Reproducible across runs given the same seed (second arg, default 1).
#
# Generates a few dozen files across a random directory tree.  Each file is
# randomly assigned one of:
#   - identical both sides
#   - local-only
#   - remote-only
#   - same content but local newer
#   - same content but remote newer
#   - different content (random side newer)
#
# Re-run with different seeds to explore different shapes.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/_common.sh"

ROOT="$(fixture_root "random" "${1:-}")"
SEED="${2:-1}"
FILE_COUNT="${3:-40}"
MAX_DEPTH="${4:-4}"

reset_pair "$ROOT"
fixture_header "random" "$ROOT"
printf '  seed  : %s\n  files : %s\n  depth : up to %s\n\n' "$SEED" "$FILE_COUNT" "$MAX_DEPTH"

# awk-based PRNG so the distribution is stable per seed (bash $RANDOM is
# non-seedable on most shells).  Generates space-separated ints in [0, bound).
random_stream() {
    awk -v seed="$SEED" -v count="$1" -v bound="$2" '
        BEGIN {
            srand(seed)
            for (index_ = 0; index_ < count; ++index_)
                printf "%d ", int(rand() * bound)
            print ""
        }
    '
}

# Pre-generate enough ints for every random decision below.
INTS=( $(random_stream $((FILE_COUNT * 8)) 10000) )
i=0
next_int() { local v="${INTS[$i]}"; i=$((i + 1)); printf '%s' "$v"; }

rand_name() {
    local n=$(( $(next_int) % 10000 ))
    printf 'f_%04d.txt' "$n"
}

rand_dir() {
    local depth=$(( $(next_int) % (MAX_DEPTH + 1) ))
    local dir=""
    while (( depth > 0 )); do
        local seg_idx=$(( $(next_int) % 5 ))
        local segs=(alpha beta gamma delta epsilon)
        dir="$dir/${segs[$seg_idx]}"
        depth=$((depth - 1))
    done
    printf '%s' "$dir"
}

rand_mtime() {
    # Random mtime in 2024-2026 range, 10-minute granularity.
    local y=$(( 2024 + $(next_int) % 3 ))
    local mo=$(( 1 + $(next_int) % 12 ))
    local d=$(( 1 + $(next_int) % 28 ))
    local h=$(( $(next_int) % 24 ))
    local mn=$(( ($(next_int) % 6) * 10 ))
    printf '%04d%02d%02d%02d%02d' "$y" "$mo" "$d" "$h" "$mn"
}

for _ in $(seq 1 "$FILE_COUNT"); do
    dir="$(rand_dir)"
    name="$(rand_name)"
    rel="${dir:+${dir}/}$name"
    rel="${rel#/}"
    category=$(( $(next_int) % 6 ))
    mt_a="$(rand_mtime)"
    mt_b="$(rand_mtime)"

    case "$category" in
        0)  # identical
            mkf_at "$ROOT/local/$rel"  "payload $rel" "$mt_a"
            mkf_at "$ROOT/remote/$rel" "payload $rel" "$mt_a"
            ;;
        1)  # local-only
            mkf_at "$ROOT/local/$rel" "local-only $rel" "$mt_a"
            ;;
        2)  # remote-only
            mkf_at "$ROOT/remote/$rel" "remote-only $rel" "$mt_a"
            ;;
        3)  # same content, local newer
            mkf_at "$ROOT/local/$rel"  "shared $rel" "202612010900"
            mkf_at "$ROOT/remote/$rel" "shared $rel" "202401010900"
            ;;
        4)  # same content, remote newer
            mkf_at "$ROOT/local/$rel"  "shared $rel" "202401010900"
            mkf_at "$ROOT/remote/$rel" "shared $rel" "202612010900"
            ;;
        5)  # different content, mtimes from rand
            mkf_at "$ROOT/local/$rel"  "local payload $rel $(next_int)"  "$mt_a"
            mkf_at "$ROOT/remote/$rel" "remote payload $rel $(next_int)" "$mt_b"
            ;;
    esac
done

summarize_pair "$ROOT"
