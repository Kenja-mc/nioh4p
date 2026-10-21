#!/bin/sh
# usage: login.sh <a|b>
#
# Starts the rig client for the slot and picks the slot's account in Steam's
# "Who's playing?" picker, then proves it via the client's own connection log.
# The picker appears on every start and the two tiles are at fixed positions on
# the 1280x720 rig display (left = <account-name-private-1>, right = <account-name-private-2>); no password or
# Steam Guard is involved - the clone carries the stored JWT.
set -eu
. "$(dirname "$0")/env.sh"

slot="${1:?usage: login.sh <a|b>}"
root="$(slot_root "$slot")"
case "$slot" in
    a) tile_x=535 ;;
    b) tile_x=635 ;;
    *) echo "slot must be a or b" >&2; exit 1 ;;
esac

mkdir -p "$RIG/shots"

if ! pgrep -f "nioh3p-test/home-$slot" >/dev/null 2>&1; then
    ( cd "$root" && LIBGL_ALWAYS_SOFTWARE=1 nohup "$root/steam.sh" -silent \
        -cef-disable-gpu >>"$RIG/steam-$slot.log" 2>&1 & )
fi

# wait for the picker to be drawn (display no longer black)
i=0
while [ "$i" -lt 20 ]; do
    i=$((i + 1))
    sleep 3
    DISPLAY="$(slot_display "$slot")" import -window root -silent "$RIG/shots/picker-$slot.png" \
        2>/dev/null || true
    mean=$(DISPLAY="$(slot_display "$slot")" identify -format '%[mean]' \
        "$RIG/shots/picker-$slot.png" 2>/dev/null || echo 0)
    case "$mean" in 0|0.0|"") ;; *) break ;; esac
done

# pick the account tile
DISPLAY="$(slot_display "$slot")" python3 "$(dirname "$0")/xkey.py" move "$tile_x" 384
DISPLAY="$(slot_display "$slot")" python3 "$(dirname "$0")/xkey.py" click 1

i=0
while [ "$i" -lt 15 ]; do
    i=$((i + 1))
    sleep 3
    grep -q "Logged On" "$root/logs/connection_log.txt" 2>/dev/null && break
done

printf 'slot %s: ' "$slot"
tail -1 "$root/logs/connection_log.txt" 2>/dev/null | cut -c1-110
