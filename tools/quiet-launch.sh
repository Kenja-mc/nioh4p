#!/usr/bin/env bash
# Steam launch-option wrapper for Nioh: Complete Edition (appid 485510).
#
# Set once in Steam -> Nioh -> Properties -> Launch Options:
#
#     /path/to/nioh-mp-mod/tools/quiet-launch.sh %command%
#
# then:
#   * normal play (default): exactly the invocation your launch options already
#     use, nested fullscreen gamescope at 1920x1080/120 - unchanged behaviour
#   * headless mode (`--headless-on`, or NIOH3P_HEADLESS=1): the same Steam
#     launch (so the DRM stub is happy) rendered on gamescope's headless backend
#     inside a capped systemd scope - no window, no focus steal, capped CPU/RAM
#
# Why a wrapper instead of editing the launch options per test: launch options
# are per-title, so a headless line would also hide the game from you during
# normal play. This keeps one line and switches modes with a flag file that only
# exists while a test is running.
#
#   tools/quiet-launch.sh --headless-on         # arm headless mode
#   tools/quiet-launch.sh --headless-off        # back to normal play
#   tools/quiet-launch.sh --status              # which mode is armed
#   tools/quiet-launch.sh --dry-run vkcube      # print the command that would run
#
# Env: NIOH3P_GPU picks DXVK's device for a headless run (default GFX1201;
#      RAPHAEL_MENDOCINO is the iGPU). NIOH3P_VK_DEVICE optionally selects the
#      compositor's PCI vendor:device too (e.g. 1002:13c0 for this rig's iGPU).
#      NIOH3P_SILENT=1 routes audio to a null sink; NIOH3P_SCOPE=0 drops the scope.
set -uo pipefail

FLAG="${NIOH3P_FLAG:-$HOME/.cache/nioh3p/headless.flag}"
GPU="${NIOH3P_GPU:-GFX1201}"
W=1280 H=720 R=30

case "${1:-}" in
    --headless-on)
        mkdir -p "$(dirname "$FLAG")"; : > "$FLAG"
        echo "headless mode armed: $FLAG"; exit 0 ;;
    --headless-off)
        rm -f "$FLAG"; echo "headless mode off (normal play)"; exit 0 ;;
    --status)
        [ -e "$FLAG" ] && echo headless || echo normal; exit 0 ;;
esac

DRY=0
if [ "${1:-}" = "--dry-run" ]; then
    DRY=1; shift
fi
[ "$#" -gt 0 ] || { echo "usage: $0 [--dry-run] <command...>  (Steam passes %command%)" >&2; exit 2; }

headless=0
[ -e "$FLAG" ] || [ "${NIOH3P_HEADLESS:-0}" = "1" ] && headless=1

if [ "$headless" = "1" ]; then
    CMD=(gamescope --backend headless -W "$W" -H "$H" -r "$R")
    if [ -n "${NIOH3P_VK_DEVICE:-}" ]; then
        CMD+=(--prefer-vk-device "$NIOH3P_VK_DEVICE")
    fi
    CMD+=(-- "$@")
    # An isolated runtime directory cannot reach the desktop's Wayland socket.
    # Gamescope supplies its own DISPLAY to the child; Wine needs DXVK's default WSI.
    CMD=(env -u DISPLAY -u WAYLAND_DISPLAY -u DXVK_WSI_DRIVER "${CMD[@]}")
    if [ "${NIOH3P_SCOPE:-1}" = "1" ]; then
        CMD=(systemd-run --user --scope --quiet -p CPUQuota=200% -p CPUWeight=20 \
             -p IOWeight=20 -p MemoryHigh=8G -p MemoryMax=12G -E "DXVK_FILTER_DEVICE_NAME=$GPU" "${CMD[@]}")
    else
        CMD=(env "DXVK_FILTER_DEVICE_NAME=$GPU" "${CMD[@]}")
    fi
    if [ "${NIOH3P_SILENT:-0}" = "1" ]; then
        CMD=(env "PULSE_SINK=${PULSE_SINK:-nioh3p}" "${CMD[@]}")
    fi
    who="headless (gamescope --backend headless, GPU $GPU)"
else
    # the invocation this machine already used for the game
    CMD=(env ENABLE_GAMESCOPE_WSI=0 gamescope -f -w 1920 -h 1080 -W 1920 -H 1080 -r 120 -- "$@")
    who="normal (nested fullscreen gamescope)"
fi

if [ "$DRY" = "1" ]; then
    printf '%s mode: ' "$who"
    printf '%q ' "${CMD[@]}"
    printf '\n'
    exit 0
fi

# The flag arms exactly one launch - the next one - and this is it, so consume
# it here.  After the exec below this shell is gone and no trap could disarm it
# later, and an interrupted test must not leave the next normal launch on
# gamescope's headless backend, windowless and unexplained.
if [ "$headless" = "1" ]; then
    rm -f "$FLAG"
fi

echo "quiet-launch: $who" >&2
exec "${CMD[@]}"
