#!/usr/bin/env bash
# Uploads framed screenshots to the MEGA S4 buckets that the README, the webpage and the flatpak metainfo read from.
# Usage: upload.sh [--token-file <path>] [--check] <directory with the framed PNGs>
#
# The token is "<access key id>:<secret access key>", taken from NUI_SFTP_S4_TOKEN or from the first line of the
# token file. A token file may also hold the key id and the secret on two lines.
# --check only validates the token and exits.
#
# Every <name>.png in the directory goes to s3://nui-sftp-releases/images/Screenshots/<name>.png, session.png also to
# s3://nui-scp/images/nui-sftp-session.png (the flatpak metainfo screenshot). Needs the aws CLI.
set -euo pipefail

endpoint="https://s3.eu-central-1.s4.mega.io"
region="eu-central-1"
tokenFile=""
checkOnly=0
directory=""

while [ $# -gt 0 ]; do
    case "$1" in
        --token-file) tokenFile="${2:?--token-file needs a path}"; shift 2 ;;
        --check) checkOnly=1; shift ;;
        -h|--help) sed -n '2,12s/^# \{0,1\}//p' "$0"; exit 0 ;;
        *) directory="$1"; shift ;;
    esac
done

token="${NUI_SFTP_S4_TOKEN:-}"
if [ -n "$tokenFile" ]; then
    [ -r "$tokenFile" ] || { echo "cannot read the token file $tokenFile" >&2; exit 1; }
    first="$(sed -n '1p' "$tokenFile" | tr -d '[:space:]')"
    second="$(sed -n '2p' "$tokenFile" | tr -d '[:space:]')"
    if [[ "$first" == *:* ]]; then
        token="$first"
    else
        token="$first:$second"
    fi
fi
[ -n "$token" ] || { echo "no upload token, set NUI_SFTP_S4_TOKEN or pass --token-file" >&2; exit 1; }
accessKey="${token%%:*}"
secretKey="${token#*:}"
if [ -z "$accessKey" ] || [ -z "$secretKey" ] || [ "$accessKey" = "$token" ]; then
    echo "the upload token has to look like <access key id>:<secret access key>" >&2
    exit 1
fi
command -v aws > /dev/null || { echo "the aws CLI is needed for the upload" >&2; exit 1; }
[ "$checkOnly" = 1 ] && exit 0

[ -d "$directory" ] || { echo "usage: upload.sh [--token-file <path>] <directory with the framed PNGs>" >&2; exit 2; }

upload() {
    local file="$1"
    local destination="$2"
    AWS_ACCESS_KEY_ID="$accessKey" AWS_SECRET_ACCESS_KEY="$secretKey" AWS_DEFAULT_REGION="$region" \
        AWS_CONFIG_FILE=/dev/null AWS_SHARED_CREDENTIALS_FILE=/dev/null \
        aws s3 cp "$file" "$destination" --endpoint-url "$endpoint" --content-type image/png --only-show-errors
    echo "uploaded $file to $destination"
}

shopt -s nullglob
files=("$directory"/*.png)
[ ${#files[@]} -gt 0 ] || { echo "no PNG files in $directory" >&2; exit 1; }
for file in "${files[@]}"; do
    name="$(basename -- "$file")"
    upload "$file" "s3://nui-sftp-releases/images/Screenshots/$name"
    if [ "$name" = session.png ]; then
        upload "$file" "s3://nui-scp/images/nui-sftp-session.png"
    fi
done
