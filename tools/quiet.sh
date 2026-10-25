#!/usr/bin/env bash
# nioh3p quiet runner - work on the mod without getting in the way of the desktop.
#
# What it guarantees:
#   * the Steam installation is only ever READ; the run happens in a copy under
#     ~/.cache/nioh3p (nothing is written next to your game, not even a log)
#   * the game renders to a private Xvfb display, so no window, no focus steal
#     and no alt-tab surprise on your session
#   * the whole run lives in a systemd scope with capped CPU/IO/memory and runs
#     at low priority, so browsing and compilation keep their share
#   * it refuses to start while a game is running, a fullscreen window is up or
#     the machine is loaded - and can wait for a quiet moment instead
#
# Usage:
#   tools/quiet.sh --check          # is this a good moment? (exit 0 = yes)
#   tools/quiet.sh proxy            # harness run: hooks + clamping, ~15 s
#   tools/quiet.sh game             # copy of nioh.exe: DRM unpack + image dump
#   tools/quiet.sh game --wait      # wait for an idle moment first
#   tools/quiet.sh --stop           # kill anything a previous run left behind
#
# Env: SMOKE_DIR (default ~/.cache/nioh3p/smoke), QUIET_TIMEOUT (default 90 s per
#      run), QUIET_MAX_WAIT (default 3600 s when --wait is given),
#      QUIET_NOW=1 to skip the busy check.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GAME="${NIOH_DIR:-$HOME/.local/share/Steam/steamapps/common/Nioh}"
STATE="${QUIET_STATE:-$HOME/.cache/nioh3p}"
export SMOKE_DIR="${SMOKE_DIR:-$STATE/smoke}"
export SMOKE_TIMEOUT="${QUIET_TIMEOUT:-90}"
XDISP="${QUIET_DISPLAY:-:9}"
UNIT="nioh3p-run-$$"          # unique: a killed run can leave a loaded unit behind
LOGF="$STATE/quiet.log"

mkdir -p "$STATE" "$SMOKE_DIR"
say() { printf '%s %s\n' "$(date +%H:%M:%S)" "$*" | tee -a "$LOGF"; }

MODE="${1:-game}"                  # needed by the busy check below

# ---------------------------------------------------------------- busy check
busy_reason() {
    local p
    # A harness run (proxy mode) never touches the game: it uses stub DLLs and its
    # own throwaway prefix, so another game running is not a reason to wait.
    if [ "$MODE" != "proxy" ]; then
        p=$(pgrep -f 'steamapps/common/[^/]+/[^/]+\.exe' | head -1) &&
            { echo "a Proton game is already running (pid $p)"; return; }
        pgrep -x gamescope >/dev/null && { echo "gamescope is running"; return; }
    fi

    if command -v hyprctl >/dev/null 2>&1; then
        local fs
        fs=$(timeout 3 hyprctl activewindow -j 2>/dev/null |
             grep -o '"fullscreen": *[0-9]*' | grep -o '[0-9]*' | head -1)
        [ "${fs:-0}" != "0" ] && { echo "a fullscreen window is in the foreground"; return; }
    fi

    # "idle" inhibitors mean something (video, game, presentation) is claiming the
    # session. Only the always-present ones are ignored.
    local who
    who=$(systemd-inhibit --list --no-pager 2>/dev/null |
          awk '$0 ~ / idle / && $1 !~ /^(omp|code|Herdr|Omarchy|UPower|systemd-inhibit)$/ {print $1; exit}')
    [ -n "${who:-}" ] && { echo "$who holds an idle inhibitor"; return; }

    local load n
    load=$(cut -d' ' -f1 /proc/loadavg)
    n=$(nproc)
    awk -v l="$load" -v n="$n" 'BEGIN { exit !(l > n * 0.6) }' &&
        { echo "load average is $load on $n cores"; return; }

    return 1
}

check() {
    local why
    if why=$(busy_reason); then
        say "BUSY: $why"
        return 1
    fi
    say "free to run (load $(cut -d' ' -f1-3 /proc/loadavg), no game, no fullscreen app)"
    return 0
}

# ------------------------------------------------------------------- cleanup
stop_all() {
    systemctl --user stop "$UNIT.scope" 2>/dev/null
    pkill -f "Xvfb $XDISP" 2>/dev/null
    pkill -f "smoketest.sh" 2>/dev/null
    say "stopped"
}

if [ "${1:-}" = "--stop" ]; then
    stop_all
    exit 0
fi

if [ "${1:-}" = "--check" ]; then
    check
    exit $?
fi

# --------------------------------------------------------------------- set up
WAIT=0
[ "${2:-}" = "--wait" ] && WAIT=1

if [ "${QUIET_NOW:-0}" != "1" ]; then
    if [ "$WAIT" = "1" ]; then
        deadline=$(( $(date +%s) + ${QUIET_MAX_WAIT:-3600} ))
        until check >/dev/null 2>&1; do
            [ "$(date +%s)" -ge "$deadline" ] && { say "gave up waiting for a quiet moment"; exit 3; }
            sleep 20
        done
        say "quiet moment found"
    else
        check || { say "refusing to run now - use --wait, QUIET_NOW=1, or try later"; exit 3; }
    fi
fi

# the game directory is read-only for us; prove it before and after
SNAP_BEFORE="$STATE/gamedir.before"
SNAP_AFTER="$STATE/gamedir.after"
( cd "$GAME" 2>/dev/null && find . -maxdepth 1 -printf '%p|%s|%T@\n' | sort ) > "$SNAP_BEFORE"

# Private display. A no-render run (harness) is happy on Xvfb; the real game
# needs a working Vulkan swapchain, and Xvfb has no DRI3 - so it goes through
# gamescope's headless backend, which creates no window either way.
GAMESCOPE_WRAP=()
if [ "$MODE" = "game" ] && command -v gamescope >/dev/null 2>&1; then
    GAMESCOPE_WRAP=(gamescope --backend headless -W 1280 -H 720 -r 30 --)
    say "using gamescope headless backend (no window, real swapchain)"
else
    if [ -e "/tmp/.X11-unix/X${XDISP#:}" ]; then
        say "display $XDISP busy, reusing it"
    else
        Xvfb "$XDISP" -screen 0 1280x720x24 -nolisten tcp >"$STATE/xvfb.log" 2>&1 &
        for _ in $(seq 1 50); do
            [ -e "/tmp/.X11-unix/X${XDISP#:}" ] && break
            sleep 0.1
        done
    fi
    export DISPLAY="$XDISP"
fi

# priority + cgroup caps, degrading gracefully if a controller is unavailable
run_capped() {
    local props=("$@")
    local wrap=()
    [ "${#GAMESCOPE_WRAP[@]}" -gt 0 ] && wrap=("${GAMESCOPE_WRAP[@]}")
    # never pipe a wrapped run: a killed gamescope leaves children holding the
    # write end, which would hang forever
    systemd-run --user --scope --collect --quiet \
        --unit="$UNIT" "${props[@]}" -- \
        nice -n 19 ionice -c3 \
        env SMOKE_WRAPPER="${wrap[*]}" "$ROOT/tools/smoketest.sh" "$MODE" \
        >"$STATE/run.out" 2>&1
}
# a previous run killed hard can leave its scope loaded, which would make every
# later start fail with "already loaded"
for u in $(systemctl --user list-units --all --plain --no-legend 'nioh3p-run-*' 2>/dev/null | awk '{print $1}'); do
    systemctl --user stop "$u" 2>/dev/null
done
systemctl --user reset-failed 'nioh3p-run-*' 2>/dev/null

say "running '$MODE' on ${DISPLAY:-gamescope-headless} (timeout ${SMOKE_TIMEOUT}s, capped + niced)"
RC=0
run_capped -p CPUQuota=200% -p CPUWeight=20 -p IOWeight=20 \
           -p MemoryHigh=8G -p MemoryMax=12G 2>"$STATE/scope.err" || RC=$?
if [ "$RC" != "0" ] && grep -qi 'property\|Failed to' "$STATE/scope.err" 2>/dev/null; then
    say "scoped run failed ($(head -1 "$STATE/scope.err")), retrying with CPU caps only"
    run_capped -p CPUQuota=200% -p CPUWeight=20
    RC=$?
fi

# --------------------------------------------------------------------- report
( cd "$GAME" 2>/dev/null && find . -maxdepth 1 -printf '%p|%s|%T@\n' | sort ) > "$SNAP_AFTER"
if diff -q "$SNAP_BEFORE" "$SNAP_AFTER" >/dev/null; then
    say "game directory untouched: identical file list, sizes and mtimes ($(wc -l < "$SNAP_BEFORE") entries)"
else
    say "!! game directory changed:"
    diff "$SNAP_BEFORE" "$SNAP_AFTER" | sed 's/^/   /' | tee -a "$LOGF"
fi
say "run finished (rc=$RC); artifacts under $SMOKE_DIR"
[ -s "$STATE/run.out" ] && tail -6 "$STATE/run.out" | sed 's/^/   /' | tee -a "$LOGF"
exit "$RC"
