#!/usr/bin/env bash
# Deep audit fixture.  Differences sit on several levels so the sync dialog has
# to synthesize structural ancestor rows, lazily expand one-sided directories
# and keep upload and download rows apart in the same subtrees.
#
# Usage: deep.sh [root] [bulk directories] [files per bulk directory]
#   bulk directories default to 0.  With e.g. `deep.sh '' 40 50` the fixture adds
#   bulk/dNN/sub/fNNN.txt pairs (2000 differing files) for performance work.
#
# Expected rows with the default settings (direction Both, upload + download,
# recursive, respect ignore files, hidden files shown):
#
# Upload (local newer or local only)
#   top_local_only.txt
#   empty_local_dir/                                one-sided empty directory
#   projects/app/src/main.cpp                       content differs, local newer
#   projects/app/src/util/strings.cpp               same size, local newer
#   projects/app/src/size_only.h                    same mtime, sizes differ (ties favor upload)
#   projects/app/docs/guide/chapter1/intro.md       local only, five levels down
#   projects/new_module/                            one-sided subtree, 6 files on 3 levels
#   media/photos/2026/03/img_000{1,2,3}.jpg         local only, deep
#   tricky/a/file.txt                               local only ('a' sorts before 'a-b', 'a.b')
#   tricky/a.b                                      local newer file
#   unicode/übung/ナ.txt                             local only, non-ASCII names
#   .config/settings.json                           hidden, local newer
#   conflict_item                                   local directory vs remote file
#   links/current                                   symlink with a different target, local newer
#
# Download (remote newer or remote only)
#   top_remote_only.txt
#   empty_remote_dir/
#   projects/app/src/net/socket.cpp                 remote newer
#   projects/app/assets/icons/large/remote_icon.svg remote only, deep
#   projects/remote_tool/                           one-sided subtree (bin, lib/x/y)
#   media/videos/                                   one-sided subtree
#   logs/2026/10/12/app.log                         remote newer, four levels down
#   tricky/a-b/file.txt                             remote only
#   projects/app/.env                               hidden, remote only
#
# Never listed
#   projects/app/README.md, projects/app/src/identical.cpp, media/photos/2025/old.jpg (identical)
#   projects/app/src/same_stat.c   same size and mtime, different bytes (stat-based diff cannot see it)
#   build/output.bin, cache/x.tmp  ignored by the root .gitignore while "respect ignore files" is on
#
# Toggling "ignore hidden" drops .config, projects/app/.env.  Direction Upload with
# delete turns the download-side one-sided entries into remote deletes, direction
# Download with delete turns the upload-side ones into local deletes.
#
# Push the remote half to an SFTP host with push-remote.sh.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/_common.sh"

ROOT="$(fixture_root "deep" "${1:-}")"
BULK_DIRS="${2:-0}"
BULK_FILES="${3:-50}"
reset_pair "$ROOT"
fixture_header "deep" "$ROOT"

OLD=202501010900
NEW=202606010900
L="$ROOT/local"
R="$ROOT/remote"

# Identical on both sides
for side in "$L" "$R"; do
    mkf_at "$side/projects/app/README.md"          "# app"                "$OLD"
    mkf_at "$side/projects/app/src/identical.cpp"  "int identical();"     "$OLD"
    mkf_at "$side/media/photos/2025/old.jpg"       "JPEG-OLD"             "$OLD"
    mkf_at "$side/projects/app/src/same_stat.c"    "$( [[ $side == "$L" ]] && echo AAAA || echo BBBB )" "$OLD"
    mkf    "$side/.gitignore"                      $'build/\n*.tmp\n'
done

# Upload side
mkf_at "$L/top_local_only.txt"                          "local only"           "$NEW"
mkd    "$L/empty_local_dir"
mkf_at "$L/projects/app/src/main.cpp"                   "int main(){return 1;}" "$NEW"
mkf_at "$R/projects/app/src/main.cpp"                   "int main(){}"          "$OLD"
mkf_at "$L/projects/app/src/util/strings.cpp"           "strings v2"           "$NEW"
mkf_at "$R/projects/app/src/util/strings.cpp"           "strings v1"           "$OLD"
mkf_at "$L/projects/app/src/size_only.h"                "#pragma once // long" "$OLD"
mkf_at "$R/projects/app/src/size_only.h"                "#pragma once"         "$OLD"
mkf_at "$L/projects/app/docs/guide/chapter1/intro.md"   "# intro"              "$NEW"
mkf_at "$R/projects/app/docs/guide/index.md"            "# guide"              "$OLD"
mkf_at "$L/projects/app/docs/guide/index.md"            "# guide"              "$OLD"
mkf_at "$L/projects/new_module/CMakeLists.txt"          "project(new)"         "$NEW"
mkf_at "$L/projects/new_module/src/a.cpp"               "a"                    "$NEW"
mkf_at "$L/projects/new_module/src/b.cpp"               "b"                    "$NEW"
mkf_at "$L/projects/new_module/src/detail/c.cpp"        "c"                    "$NEW"
mkf_at "$L/projects/new_module/src/detail/d.cpp"        "d"                    "$NEW"
mkf_at "$L/projects/new_module/include/new.hpp"         "#pragma once"         "$NEW"
for idx in 1 2 3; do
    mkf_at "$L/media/photos/2026/03/img_000$idx.jpg"    "JPEG-$idx"            "$NEW"
done
mkf_at "$L/tricky/a/file.txt"                           "local a"              "$NEW"
mkf_at "$L/tricky/a.b"                                  "a.b v2"               "$NEW"
mkf_at "$R/tricky/a.b"                                  "a.b v1"               "$OLD"
mkf_at "$L/unicode/übung/ナ.txt"                         "unicode"              "$NEW"
mkf_at "$L/.config/settings.json"                       '{"v":2}'              "$NEW"
mkf_at "$R/.config/settings.json"                       '{"v":1}'              "$OLD"
mkf_at "$L/conflict_item/inside.txt"                    "dir on local"         "$NEW"
mkf_at "$R/conflict_item"                               "file on remote"       "$OLD"
mklink "$L/links/current" "v2"
mklink "$R/links/current" "v1"
touch -h -t "$NEW" "$L/links/current"
touch -h -t "$OLD" "$R/links/current"

# Download side
mkf_at "$R/top_remote_only.txt"                         "remote only"          "$NEW"
mkd    "$R/empty_remote_dir"
mkf_at "$L/projects/app/src/net/socket.cpp"             "socket v1"            "$OLD"
mkf_at "$R/projects/app/src/net/socket.cpp"             "socket v2 longer"     "$NEW"
mkf_at "$R/projects/app/assets/icons/large/remote_icon.svg" "<svg/>"           "$NEW"
mkd    "$L/projects/app/assets/icons"
mkf_at "$R/projects/remote_tool/bin/tool"               "#!/bin/sh"            "$NEW"
mkf_at "$R/projects/remote_tool/lib/x/y/deep.so"        "ELF"                  "$NEW"
mkf_at "$R/projects/remote_tool/README"                 "tool"                 "$NEW"
mkf_at "$R/media/videos/2026/clip.mp4"                  "MP4"                  "$NEW"
mkf_at "$R/media/videos/2025/old_clip.mp4"              "MP4-OLD"              "$NEW"
mkf_at "$L/logs/2026/10/12/app.log"                     "log v1"               "$OLD"
mkf_at "$R/logs/2026/10/12/app.log"                     "log v1\nlog v2"       "$NEW"
mkf_at "$R/tricky/a-b/file.txt"                         "remote a-b"           "$NEW"
mkf_at "$R/projects/app/.env"                           "SECRET=1"             "$NEW"

# Ignored by .gitignore (both differ, neither should show while ignore files are respected)
mkf_at "$L/build/output.bin"                            "BIN"                  "$NEW"
mkf_at "$R/cache/x.tmp"                                 "TMP"                  "$NEW"

# Bulk section for performance work: half of the files are local newer, half remote newer.
for ((dir = 0; dir < BULK_DIRS; ++dir)); do
    dirName=$(printf 'd%03d' "$dir")
    mkdir -p -- "$L/bulk/$dirName/sub" "$R/bulk/$dirName/sub"
    newer=()
    older=()
    for ((file = 0; file < BULK_FILES; ++file)); do
        rel=$(printf 'bulk/%s/sub/f%04d.txt' "$dirName" "$file")
        printf 'local %s' "$rel" >"$L/$rel"
        printf 'remote %s!' "$rel" >"$R/$rel"
        if ((file % 2 == 0)); then
            newer+=("$L/$rel")
            older+=("$R/$rel")
        else
            newer+=("$R/$rel")
            older+=("$L/$rel")
        fi
    done
    touch -t "$NEW" -- "${newer[@]}"
    touch -t "$OLD" -- "${older[@]}"
done

if ((BULK_DIRS == 0)); then
    summarize_pair "$ROOT"
else
    printf 'bulk: %d directories x %d files\n' "$BULK_DIRS" "$BULK_FILES"
fi
