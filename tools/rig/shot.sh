#!/bin/sh
# usage: shot.sh [name]   ->  $RIG/shots/<name>.png  (the rig display only)
set -eu
. "$(dirname "$0")/env.sh"
slot="${1:-}"; name="${2:-shot}"
mkdir -p "$RIG/shots"
out="$RIG/shots/$name.png"
if ! DISPLAY="$(slot_display "$slot")" import -window root -silent "$out" 2>/dev/null; then
    DISPLAY="$(slot_display "$slot")" ffmpeg -loglevel error -f x11grab \
        -video_size 1280x720 -i "$XVFB_DISPLAY" -frames:v 1 -y "$out"
fi
printf '%s\n' "$out"
