#!/bin/sh
# usage: login.sh <a|b>
#
# Starts the rig client for the slot and picks the slot's account in Steam's
# "Who's playing?" picker, then proves it via the client's own connection log.
# The picker appears on every start and the two tiles are at fixed positions on
# the 1280x720 rig display (slot a left, slot b right); no password or
# Steam Guard is involved when the clone carries a valid stored login.
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

if ! pgrep -f "$RIG/home-$slot" >/dev/null 2>&1; then
    ( cd "$root" && LIBGL_ALWAYS_SOFTWARE=1 nohup "$root/steam.sh" -silent \
        -cef-disable-gpu >>"$RIG/steam-$slot.log" 2>&1 & )
fi

# wait for the picker to be drawn (display no longer black)
picker=0
i=0
while [ "$i" -lt 20 ]; do
    i=$((i + 1))
    sleep 3
    DISPLAY="$(slot_display "$slot")" import -window root -silent "$RIG/shots/picker-$slot.png" \
        2>/dev/null || true
    mean=$(DISPLAY="$(slot_display "$slot")" identify -format '%[mean]' \
        "$RIG/shots/picker-$slot.png" 2>/dev/null || echo 0)
    case "$mean" in 0|0.0|"") ;; *) picker=1; break ;; esac
done
# a picker that never drew means the tile click below would land on nothing
[ "$picker" = 1 ] ||
    { echo "slot $slot: no account picker on $(slot_display "$slot") after 60 s" >&2; exit 1; }

# pick the account tile
DISPLAY="$(slot_display "$slot")" python3 "$(dirname "$0")/xkey.py" move "$tile_x" 384
DISPLAY="$(slot_display "$slot")" python3 "$(dirname "$0")/xkey.py" click 1

logged=0
i=0
while [ "$i" -lt 15 ]; do
    i=$((i + 1))
    sleep 3
    if grep -q "Logged On" "$root/logs/connection_log.txt" 2>/dev/null; then
        logged=1
        break
    fi
done

printf 'slot %s: login %s\n' "$slot" "$([ "$logged" = 1 ] && echo detected || echo not-detected)"

# the picker click, the log and this exit status are the only proof the account
# really came up: report a miss as a failure instead of a silent 0
[ "$logged" = 1 ] ||
    { echo "slot $slot: no \"Logged On\" in $root/logs/connection_log.txt after 45 s" >&2; exit 1; }
