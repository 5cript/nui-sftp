#!/usr/bin/env bash
# Uploads framed screenshots to the MEGA S4 buckets that the README, the webpage and the flatpak metainfo read from.
# Usage: upload.sh [--token-file <path>] [--check] [--verify] <directory with the framed PNGs>
#
# The token is "<access key id>:<secret access key>", taken from NUI_SFTP_S4_TOKEN or from the first line of the
# token file. A token file may also hold the key id and the secret on two lines, or be the key file MEGA S4 offers
# for download, with its aws_access_key_id=... and aws_secret_access_key=... lines.
# --check only validates the token and exits.
# --verify uploads nothing and needs no token: it only checks that the public URL of every PNG answers as image/png.
#
# Every <name>.png in the directory goes to s3://nui-sftp-releases/images/Screenshots/<name>.png, session.png also to
# s3://nui-scp/images/nui-sftp-session.png (the flatpak metainfo screenshot). Needs the aws CLI.
#
# The objects have to be stored as image/png. GitHub shows README images through a proxy that refuses every other
# content type, and a file put there by other means is usually stored as binary/octet-stream. So after the upload
# the public URLs are checked as well.
set -euo pipefail

endpoint="https://s3.eu-central-1.s4.mega.io"
publicBase="https://s3.g.s4.mega.io/jgemkib4a5fte35rktt5wxrwkw4ejk4ybemkf"
region="eu-central-1"
tokenFile=""
checkOnly=0
verifyOnly=0
directory=""

while [ $# -gt 0 ]; do
    case "$1" in
        --token-file) tokenFile="${2:?--token-file needs a path}"; shift 2 ;;
        --check) checkOnly=1; shift ;;
        --verify) verifyOnly=1; shift ;;
        -h|--help) sed -n '2,16s/^# \{0,1\}//p' "$0"; exit 0 ;;
        *) directory="$1"; shift ;;
    esac
done

credentialOf() {
    sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*//p" "$2" | head -n 1 | tr -d '[:space:]'
}

if [ "$verifyOnly" = 0 ]; then
    token="${NUI_SFTP_S4_TOKEN:-}"
    if [ -n "$tokenFile" ]; then
        [ -r "$tokenFile" ] || { echo "cannot read the token file $tokenFile" >&2; exit 1; }
        keyId="$(credentialOf aws_access_key_id "$tokenFile")"
        secret="$(credentialOf aws_secret_access_key "$tokenFile")"
        first="$(sed -n '1p' "$tokenFile" | tr -d '[:space:]')"
        second="$(sed -n '2p' "$tokenFile" | tr -d '[:space:]')"
        if [ -n "$keyId" ] && [ -n "$secret" ]; then
            token="$keyId:$secret"
        elif [[ "$first" == *:* ]]; then
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
    [ "$checkOnly" = 0 ] || exit 0
fi

[ -d "$directory" ] || {
    echo "usage: upload.sh [--token-file <path>] [--verify] <directory with the framed PNGs>" >&2
    exit 2
}

upload() {
    local file="$1"
    local destination="$2"
    AWS_ACCESS_KEY_ID="$accessKey" AWS_SECRET_ACCESS_KEY="$secretKey" AWS_DEFAULT_REGION="$region" \
        AWS_CONFIG_FILE=/dev/null AWS_SHARED_CREDENTIALS_FILE=/dev/null \
        aws s3 cp "$file" "$destination" --endpoint-url "$endpoint" --content-type image/png --only-show-errors
    echo "uploaded $file to $destination"
}

wrongTypes=0

verify() {
    local url="$publicBase/$1"
    local contentType
    contentType="$(curl -s -m 30 -r 0-0 -o /dev/null -w '%{content_type}' "$url" || true)"
    if [ "$contentType" = image/png ]; then
        echo "served as image/png: $url"
    else
        echo "served as '${contentType:-nothing}' instead of image/png: $url" >&2
        wrongTypes=$((wrongTypes + 1))
    fi
}

shopt -s nullglob
files=("$directory"/*.png)
[ ${#files[@]} -gt 0 ] || { echo "no PNG files in $directory" >&2; exit 1; }
for file in "${files[@]}"; do
    name="$(basename -- "$file")"
    [ "$verifyOnly" = 1 ] || upload "$file" "s3://nui-sftp-releases/images/Screenshots/$name"
    verify "nui-sftp-releases/images/Screenshots/$name"
    if [ "$name" = session.png ]; then
        [ "$verifyOnly" = 1 ] || upload "$file" "s3://nui-scp/images/nui-sftp-session.png"
        verify "nui-scp/images/nui-sftp-session.png"
    fi
done
if [ "$wrongTypes" -gt 0 ]; then
    echo "$wrongTypes public URLs are not image/png, GitHub will not show them in the README" >&2
    exit 1
fi
