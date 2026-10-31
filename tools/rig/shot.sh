#!/bin/sh
# usage: shot.sh [slot] [name]  ->  $RIG/shots/<name>.png
#
# Captures the slot's GAME frame from its headless gamescope. The runtime dir
# and wayland display are read from the live game process (launch order is not
# a stable display number), like nioh_nav.shot. The slot's Xvfb shows only the
# Steam client UI, never the game; capture that with an ffmpeg x11grab on the
# slot display instead of this script.
set -eu
. "$(dirname "$0")/env.sh"
slot="${1:-b}"
name="${2:-shot}"
mkdir -p "$RIG/shots"
out="$RIG/shots/$name.png"
rm -f "$out"

rundir='' wayland=''
for d in /proc/[0-9]*; do
    [ -r "$d/environ" ] || continue
    env_home=$(tr '\0' '\n' <"$d/environ" 2>/dev/null | grep -m1 '^HOME=' | cut -d= -f2-)
    [ "$env_home" = "$RIG/home-$slot" ] || continue
    rundir=$(tr '\0' '\n' <"$d/environ" 2>/dev/null | grep -m1 '^XDG_RUNTIME_DIR=' | cut -d= -f2-)
    wayland=$(tr '\0' '\n' <"$d/environ" 2>/dev/null | grep -m1 '^GAMESCOPE_WAYLAND_DISPLAY=' | cut -d= -f2-)
    break
done
[ -n "$rundir" ] ||
    { echo "shot.sh: no live game for slot $slot (rig.py up first)" >&2; exit 1; }
: "${wayland:=gamescope-0}"

XDG_RUNTIME_DIR="$rundir" GAMESCOPE_WAYLAND_DISPLAY="$wayland" \
    gamescopectl screenshot "$out" >/dev/null 2>&1 || true

# gamescopectl writes the file asynchronously; wait for the PNG trailer
i=0
while [ "$i" -lt 30 ]; do
    if [ -s "$out" ] && [ "$(tail -c 12 "$out" | od -An -tx1 | tr -d ' \n')" \
            = "0000000049454e44ae426082" ]; then
        printf '%s\n' "$out"
        exit 0
    fi
    sleep 0.3
    i=$((i + 1))
done
echo "shot.sh: gamescopectl gave no complete frame for slot $slot" >&2
exit 1
