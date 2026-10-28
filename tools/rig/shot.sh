#!/bin/sh
# usage: shot.sh [slot] [name]  ->  $RIG/shots/<name>.png  (that slot's display)
set -eu
. "$(dirname "$0")/env.sh"
slot="${1:-}"; name="${2:-shot}"
mkdir -p "$RIG/shots"
out="$RIG/shots/$name.png"
disp="$(slot_display "$slot")"
if ! DISPLAY="$disp" import -window root -silent "$out" 2>/dev/null; then
    DISPLAY="$disp" ffmpeg -loglevel error -f x11grab \
        -video_size 1280x720 -i "$disp" -frames:v 1 -y "$out"
fi
printf '%s\n' "$out"
