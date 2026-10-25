#!/usr/bin/env bash
# Takes the README, webpage and flatpak screenshots of nui-sftp with a neutral demo profile and demo files.
#
# Usage: scripts/screenshots/take.sh [options]
#   --build-dir <dir>     CEF build to run, default build/clang_debug of the main checkout (also from a worktree)
#   --output <dir>        default build/screenshots: framed PNGs on top, raw captures in raw/, profile in work/
#   --scenes <a,b,...>    default all: session history snippets synchronize download_as_archive live_edit theming
#                         settings local
#   --size <W>x<H>        viewport, default 1900x1000 (the app's window size)
#   --scale <factor>      device scale factor of the captures, default 1, 2 for HiDPI images
#   --host <address>      SFTP server, default 192.168.178.70 (the sftp-container)
#   --port <port>         default 5000
#   --user <name>         default sftpuser
#   --key <private key>   default ~/.ssh/dockerkey, copied with mode 0600 into the demo home
#   --remote-root <dir>   demo tree on the server, default /home/<user>/demo, replaced on every run
#   --debug-port <port>   DevTools port of the app, default 9460
#   --token-file <path>   upload with the token in this file, see upload.sh; NUI_SFTP_S4_TOKEN works as well
#   --no-upload           never upload, even when a token is there
#   --no-bwrap            keep the demo home at its real path instead of showing it as /home/demo
#   --keep-running        leave the app running after the scenes, to iterate on a scene with drive.mjs
#
# The app runs headless, so no window opens. bubblewrap mounts the demo home at /home/demo and HOME and XDG_* point
# into it, so the real profile, command history and ~/.ssh stay untouched. Inside, the user is "demo" and the host
# "workstation". Needs node, python3, magick, ssh, tar,
# git and bwrap; the upload needs the aws CLI.
set -euo pipefail

scriptDirectory="$(cd "$(dirname "$0")" && pwd)"
repository="$(git -C "$scriptDirectory" rev-parse --show-toplevel)"
mainCheckout="$(dirname -- "$(git -C "$scriptDirectory" rev-parse --path-format=absolute --git-common-dir)")"

buildDirectory="$mainCheckout/build/clang_debug"
output="$repository/build/screenshots"
scenes=""
size="1900x1000"
scale="1"
host="192.168.178.70"
port="5000"
user="sftpuser"
key="$HOME/.ssh/dockerkey"
remoteRoot=""
debugPort="9460"
tokenFile=""
noUpload=0
useBwrap=1
keepRunning=0

while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) buildDirectory="$(realpath -- "${2:?}")"; shift 2 ;;
        --output) output="$(realpath -m -- "${2:?}")"; shift 2 ;;
        --scenes) scenes="${2:?}"; shift 2 ;;
        --size) size="${2:?}"; shift 2 ;;
        --scale) scale="${2:?}"; shift 2 ;;
        --host) host="${2:?}"; shift 2 ;;
        --port) port="${2:?}"; shift 2 ;;
        --user) user="${2:?}"; shift 2 ;;
        --key) key="${2:?}"; shift 2 ;;
        --remote-root) remoteRoot="${2:?}"; shift 2 ;;
        --debug-port) debugPort="${2:?}"; shift 2 ;;
        --token-file) tokenFile="$(realpath -- "${2:?}")"; shift 2 ;;
        --no-upload) noUpload=1; shift ;;
        --no-bwrap) useBwrap=0; shift ;;
        --keep-running) keepRunning=1; shift ;;
        -h|--help) sed -n '2,25s/^# \{0,1\}//p' "$0"; exit 0 ;;
        *) echo "unknown option $1, see --help" >&2; exit 2 ;;
    esac
done

remoteRoot="${remoteRoot:-/home/$user/demo}"
width="${size%x*}"
height="${size#*x}"
binary="$buildDirectory/bin/nui-sftp"
work="$output/work"
raw="$output/raw"
demoHome="$work/home"
appHome="$demoHome"
[ "$useBwrap" = 1 ] && appHome="/home/demo"

log() {
    printf '[take] %s\n' "$*"
}

fail() {
    printf '[take] %s\n' "$*" >&2
    exit 1
}

for tool in node python3 magick ssh tar git curl; do
    command -v "$tool" > /dev/null || fail "$tool is missing"
done
[ "$useBwrap" = 0 ] || command -v bwrap > /dev/null || fail "bwrap is missing, install bubblewrap or pass --no-bwrap"
[ -x "$binary" ] || fail "no nui-sftp binary at $binary, build it first"
grep -q '^NUI_BROWSER_ENGINE:STRING=cef' "$buildDirectory/CMakeCache.txt" 2>/dev/null ||
    fail "$buildDirectory is not a CEF build, the scenes drive the app over the DevTools protocol"
[ -r "$key" ] || fail "cannot read the SSH key $key"
[[ "$remoteRoot" == /*/* && "$remoteRoot" != *"'"* ]] || fail "--remote-root has to be an absolute path below / without quotes"
[[ "$width" =~ ^[0-9]+$ && "$height" =~ ^[0-9]+$ ]] || fail "--size has to look like 1900x1000"
if curl -s -m 1 "http://127.0.0.1:$debugPort/json/version" > /dev/null 2>&1; then
    fail "port $debugPort is in use, pass another --debug-port"
fi

upload=0
if [ "$noUpload" = 0 ] && { [ -n "${NUI_SFTP_S4_TOKEN:-}" ] || [ -n "$tokenFile" ]; }; then
    uploadArguments=()
    [ -n "$tokenFile" ] && uploadArguments=(--token-file "$tokenFile")
    "$scriptDirectory/upload.sh" --check "${uploadArguments[@]}" || fail "the upload token is not usable"
    upload=1
    log "a token is there, the screenshots get uploaded when every scene worked"
fi

appPid=""

stopApp() {
    [ -n "$appPid" ] || return 0
    if kill -0 "$appPid" 2>/dev/null; then
        kill -TERM "$appPid" 2>/dev/null || true
        for _ in $(seq 1 40); do
            kill -0 "$appPid" 2>/dev/null || break
            sleep 0.5
        done
        if kill -0 "$appPid" 2>/dev/null; then
            kill -KILL "$appPid" 2>/dev/null || true
        fi
    fi
    appPid=""
}

cleanup() {
    if [ "$keepRunning" = 1 ] && [ -n "$appPid" ]; then
        log "the app keeps running as pid $appPid on DevTools port $debugPort"
        return
    fi
    stopApp
}
trap cleanup EXIT

startApp() {
    local logFile="$1"
    local pidFile="$work/app.pid"
    local screenWidth=$((width > 1900 ? width + 20 : 1920))
    local screenHeight=$((height > 1000 ? height + 80 : 1080))
    local sandbox=()
    local identity=()
    if [ "$useBwrap" = 1 ]; then
        sandbox=(bwrap --dev-bind / / --tmpfs /home --unshare-user --uid "$(id -u)" --gid "$(id -g)")
        sandbox+=(--unshare-uts --hostname workstation)
        sandbox+=(--ro-bind "$work/passwd" /etc/passwd --ro-bind "$work/group" /etc/group)
        [[ "$HOME" == /home/* ]] && sandbox+=(--bind "$HOME" "$HOME")
        sandbox+=(--bind "$demoHome" "$appHome" --)
        identity=(USER=demo LOGNAME=demo)
    fi
    rm -f -- "$pidFile"
    setsid "${sandbox[@]}" env -u LANGUAGE \
        "${identity[@]}" \
        HOME="$appHome" \
        XDG_CONFIG_HOME="$appHome/.config" \
        XDG_STATE_HOME="$appHome/.local/state" \
        XDG_DATA_HOME="$appHome/.local/share" \
        XDG_CACHE_HOME="$appHome/.cache" \
        LANG=en_US.UTF-8 \
        LC_ALL=en_US.UTF-8 \
        NUI_CEF_REMOTE_DEBUGGING_PORT="$debugPort" \
        NUI_CEF_ARGUMENTS="--ozone-platform=headless --ozone-override-screen-size=$screenWidth,$screenHeight" \
        bash -c 'echo $$ > "$0"; cd "$1" && exec "$2"' "$pidFile" "$appHome" "$binary" \
        > "$logFile" 2>&1 < /dev/null &
    for _ in $(seq 1 40); do
        [ -s "$pidFile" ] && break
        sleep 0.25
    done
    appPid="$(cat "$pidFile" 2>/dev/null || true)"
    [ -n "$appPid" ] || fail "the app did not start, see $logFile"
    for _ in $(seq 1 240); do
        if grep -q "Setup is complete" "$logFile" 2>/dev/null; then
            return 0
        fi
        kill -0 "$appPid" 2>/dev/null || { tail -n 30 "$logFile" >&2; fail "the app exited during startup, see $logFile"; }
        sleep 0.25
    done
    tail -n 30 "$logFile" >&2
    fail "the app did not finish its setup within 60 s, see $logFile"
}

log "preparing $output"
mkdir -p -- "$output"
rm -rf -- "$work" "$raw"
rm -f -- "$output"/*.png
mkdir -p -- "$work" "$raw"

"$scriptDirectory/demo_data.sh" "$demoHome" "$work/server"
awk -F: -v OFS=: -v uid="$(id -u)" -v home="$appHome" \
    '$3 == uid { $1 = "demo"; $5 = "Demo User"; $6 = home } { print }' /etc/passwd > "$work/passwd"
awk -F: -v OFS=: -v gid="$(id -g)" '$3 == gid { $1 = "demo" } { print }' /etc/group > "$work/group"
mkdir -p -- "$demoHome/.ssh"
install -m 600 -- "$key" "$demoHome/.ssh/id_demo"
if [ -r "$key.pub" ]; then
    install -m 644 -- "$key.pub" "$demoHome/.ssh/id_demo.pub"
else
    ssh-keygen -y -f "$demoHome/.ssh/id_demo" > "$demoHome/.ssh/id_demo.pub"
fi

sshArguments=(
    -F /dev/null
    -i "$demoHome/.ssh/id_demo"
    -p "$port"
    -o BatchMode=yes
    -o IdentitiesOnly=yes
    -o StrictHostKeyChecking=accept-new
    -o UserKnownHostsFile="$work/known_hosts"
    "$user@$host"
)
log "copying the demo tree to $user@$host:$remoteRoot"
ssh "${sshArguments[@]}" true || fail "cannot log in to $user@$host:$port with $key"
ssh "${sshArguments[@]}" "test ! -e '$remoteRoot' || test -e '$remoteRoot/.nui-sftp-screenshots-demo'" ||
    fail "$remoteRoot exists on the server but was not made by this script, pass another --remote-root"
tar -C "$work/server" -cf - . |
    ssh "${sshArguments[@]}" "rm -rf -- '$remoteRoot' && mkdir -p -- '$remoteRoot' && tar -C '$remoteRoot' -xf -"

log "first start, writes the default profile"
startApp "$work/app-first-start.log"
stopApp

configDirectory="$demoHome/.config/nui-sftp"
dataDirectory="$demoHome/.local/share/nui-sftp"
stateFile="$configDirectory/persistence.devel.json"
[ -f "$stateFile" ] || stateFile="$configDirectory/persistence.json"
databaseFile="$dataDirectory/command_store.devel.db"
[ -f "$databaseFile" ] || databaseFile="$dataDirectory/command_store.db"
[ -f "$stateFile" ] || fail "the first start wrote no profile in $configDirectory"
python3 -I "$scriptDirectory/seed_profile.py" "$stateFile" "$databaseFile" "$appHome" "$host" "$port" "$user" \
    "$remoteRoot" id_demo

log "second start, takes the screenshots"
startApp "$work/app.log"
driveStatus=0
node "$scriptDirectory/drive.mjs" \
    --port "$debugPort" \
    --output "$raw" \
    --scenes "$scenes" \
    --width "$width" \
    --height "$height" \
    --scale "$scale" \
    --home "$appHome" \
    --remote-root "$remoteRoot" \
    --remote-user "$user" \
    --remote-host "$host" \
    --remote-port "$port" || driveStatus=$?
[ "$keepRunning" = 1 ] || stopApp

shopt -s nullglob
framed=0
for capture in "$raw"/*.png; do
    [[ "$capture" == *.failed.png ]] && continue
    "$scriptDirectory/frame.sh" "$capture" "$output/$(basename -- "$capture")" "$scale"
    framed=$((framed + 1))
done
log "framed $framed screenshots in $output"

if [ "$driveStatus" != 0 ]; then
    [ "$upload" = 1 ] && log "not uploading, some scenes failed"
    fail "some scenes failed, the app log is $work/app.log"
fi
if [ "$upload" = 1 ]; then
    "$scriptDirectory/upload.sh" "${uploadArguments[@]}" "$output"
fi
log "done"
