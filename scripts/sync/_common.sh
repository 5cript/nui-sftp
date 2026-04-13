# Shared helpers for sync-test fixture scripts.
#
# Each fixture script creates a matched `local/` and `remote/` tree under a chosen
# output directory.  Source this file at the top of the script, then call the
# helpers below.  Every helper takes absolute or fixture-relative paths.

set -euo pipefail

# Location-independent: allow overriding the fixture output root via $1 of the
# calling script.  Defaults to ./sync-fixtures/<scriptname>/ in the CWD.
fixture_root() {
    local script_name="$1"
    local override="${2:-}"
    if [[ -n "$override" ]]; then
        printf '%s\n' "$override"
    else
        printf '%s/sync-fixtures/%s\n' "$PWD" "$script_name"
    fi
}

# Reset both sides of the pair so re-running the script produces a clean state.
reset_pair() {
    local root="$1"
    rm -rf -- "$root/local" "$root/remote"
    mkdir -p -- "$root/local" "$root/remote"
}

# Make directory (no error if it already exists).
mkd() {
    mkdir -p -- "$1"
}

# Write a file with content, creating parent dirs if needed.
#   mkf <path> <content>
mkf() {
    local path="$1"
    local content="${2:-}"
    mkdir -p -- "$(dirname -- "$path")"
    printf '%s' "$content" >"$path"
}

# Write a file then stamp it to a specific mtime.
#   mkf_at <path> <content> <YYYYMMDDhhmm[.ss]>
mkf_at() {
    local path="$1"
    local content="$2"
    local stamp="$3"
    mkf "$path" "$content"
    touch -t "$stamp" -- "$path"
}

# Create a symlink, replacing any existing entry at the link path.
#   mklink <link_path> <target>
mklink() {
    local link="$1"
    local target="$2"
    mkdir -p -- "$(dirname -- "$link")"
    rm -f -- "$link"
    ln -s -- "$target" "$link"
}

# Pretty-print a header before running a fixture so CI/stdout is readable.
fixture_header() {
    local name="$1"
    local root="$2"
    printf '\n== %s ==\n  output: %s\n  local : %s/local\n  remote: %s/remote\n' \
        "$name" "$root" "$root" "$root"
}

# Dump a concise inventory of both sides so you can diff the fixture against
# what the sync dialog reports.
summarize_pair() {
    local root="$1"
    printf '\n-- local --\n'
    ( cd "$root/local" && find . -mindepth 1 -print0 | sort -z | xargs -0 -I{} ls -lh --time-style=+'%Y-%m-%d' {} 2>/dev/null || true )
    printf '\n-- remote --\n'
    ( cd "$root/remote" && find . -mindepth 1 -print0 | sort -z | xargs -0 -I{} ls -lh --time-style=+'%Y-%m-%d' {} 2>/dev/null || true )
    printf '\n'
}
