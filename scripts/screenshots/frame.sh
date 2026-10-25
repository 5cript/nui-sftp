#!/usr/bin/env bash
# Frames a window capture like Spectacle does: rounded corners, a thin outline and a soft drop shadow on a
# transparent border.
# Usage: frame.sh <capture.png> <framed.png> [scale, default 1]
# The scale multiplies radius, border and shadow, pass the device scale factor of the capture.
set -euo pipefail

input="${1:?capture png}"
output="${2:?framed png}"
scale="${3:-1}"

scaled() {
    awk -v value="$1" -v scale="$scale" 'BEGIN { printf "%d", value * scale + 0.5 }'
}

radius="$(scaled 8)"
padding="$(scaled 64)"
blur="$(scaled 20)"
offset="$(scaled 12)"
size="$(magick identify -format '%w %h' "$input")"
read -r width height <<< "$size"

magick "$input" \
    \( -size "${width}x${height}" xc:none -fill white \
        -draw "roundrectangle 0,0 $((width - 1)),$((height - 1)) $radius,$radius" \) \
    -alpha set -compose DstIn -composite \
    \( +clone -alpha extract -morphology EdgeIn Diamond:1 -background 'rgba(0,0,0,0.35)' -alpha shape \) \
    -compose Over -composite \
    -bordercolor none -border "$padding" \
    \( +clone -background black -shadow "85x${blur}+0+${offset}" \) +swap \
    -background none -layers merge +repage \
    -gravity center -crop "$((width + 2 * padding))x$((height + 2 * padding))+0+0" +repage \
    -depth 8 "$output"
