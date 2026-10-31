#!/usr/bin/env bash
# nioh3p smoke test - runs a copy of the game (or the EOS/dinput8 test harness)
# under Proton with a throwaway prefix. Nothing in the real install is touched.
#
#   tools/smoketest.sh proxy   # harness: DLL load, dinput8 forwarding, EOS hook+clamp
#   tools/smoketest.sh game    # copy of nioh.exe: VMProtect unpack + runtime image dump
#   tools/smoketest.sh ownership # CopyInfo result survives until caller release
#
# Env: NIOH3P_TRACE (default 1 for proxy/ownership, 0 for game),
#      NIOH3P_DUMP (dump_image, default 1, game only),
#      NIOH3P_DUMP_EVERY (dump_every_s, default 8, game only).
# The run uses build/research/dinput8.dll when the effective trace is 1 or a
# dump is requested, and the player build build/dinput8.dll otherwise. It
# prints which one it copied.
# The harness's "filter pass-through" PASS lines exist in player mode only:
#   NIOH3P_TRACE=0 tools/smoketest.sh proxy
# (trace=1 logs every filter call in full instead and makes the harness print
# SKIP for those two checks.)
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GAME="$HOME/.local/share/Steam/steamapps/common/Nioh"
PROTON="$HOME/.local/share/Steam/steamapps/common/Proton - Experimental/proton"
STEAM="$HOME/.local/share/Steam"
MODE="${1:-proxy}"
SMOKE="${SMOKE_DIR:-$HOME/.cache/nioh3p/smoke}"

# Every fixture copy is checked. `set -e` is deliberately off (the script must
# report Proton's exit code), so an unchecked cp left the previous run's file in
# place and the harness then passed or failed the wrong binary.
require_file() {
    [ -f "$1" ] || { echo "stale or missing fixture: $1" >&2; exit 1; }
}

copy() {
    require_file "$1"
    cp -f -- "$1" "$2" || { echo "cannot copy $1 to $2" >&2; exit 1; }
}

# Two DLLs of the game are optional: copy them when the game has them, drop a
# stale copy when it does not, so the fixture never mixes game versions.
copy_optional() {
    if [ -f "$1" ]; then
        copy "$1" "$2/"
    else
        echo "-- optional $1 absent, not copied"
        rm -f -- "$2/$(basename "$1")"
    fi
}

mkdir -p "$SMOKE/game" "$SMOKE/pfx" || { echo "cannot create $SMOKE" >&2; exit 1; }
case "$MODE" in
  proxy|ownership)
    EFFECTIVE_TRACE="${NIOH3P_TRACE:-1}"
    EFFECTIVE_DUMP=0
    ;;
  game)
    EFFECTIVE_TRACE="${NIOH3P_TRACE:-0}"
    EFFECTIVE_DUMP="${NIOH3P_DUMP:-1}"
    ;;
  *) echo "usage: $0 proxy|game|ownership" >&2; exit 2 ;;
esac
if [ "$EFFECTIVE_TRACE" = "1" ] ||
   { [ "$MODE" = "game" ] && [ "$EFFECTIVE_DUMP" != "0" ]; }; then
    DLL="$ROOT/build/research/dinput8.dll"
    DLL_KIND=research
else
    DLL="$ROOT/build/dinput8.dll"
    DLL_KIND=player
fi
[ -f "$DLL" ] || { echo "$DLL missing; run ./build.sh first" >&2; exit 1; }
echo "--- using $DLL ($DLL_KIND build) ---"
copy "$DLL" "$SMOKE/game/"
copy "$ROOT/nioh3p.ini" "$SMOKE/game/"
# The harness modes exercise the full research instrumentation by default; the
# game run defaults to player mode with image dumps, as tools/quiet.sh game does.
# The shipped ini keeps the stock search filters (both drop_* keys are 0), so the
# harness turns the two opt-in drops on explicitly: its filter pass-through /
# drop tests must keep covering that path.
case "$MODE" in
  proxy|ownership)
    echo "trace=${NIOH3P_TRACE:-1}" >> "$SMOKE/game/nioh3p.ini"
    echo "drop_lock_filter=1" >> "$SMOKE/game/nioh3p.ini"
    echo "drop_slots_filter=1" >> "$SMOKE/game/nioh3p.ini"
    ;;
  game)
    echo "trace=${NIOH3P_TRACE:-0}" >> "$SMOKE/game/nioh3p.ini"
    echo "dump_image=${NIOH3P_DUMP:-1}" >> "$SMOKE/game/nioh3p.ini"
    echo "dump_every_s=${NIOH3P_DUMP_EVERY:-8}" >> "$SMOKE/game/nioh3p.ini"
    ;;
esac
# Optional per-run member count (the ini's last key wins in the proxy parser).
[ -n "${NIOH3P_MAX_PLAYERS:-}" ] && echo "max_players=$NIOH3P_MAX_PLAYERS" >> "$SMOKE/game/nioh3p.ini"
rm -f "$SMOKE/game/nioh3p.log"

export STEAM_COMPAT_DATA_PATH="$SMOKE/pfx"
export STEAM_COMPAT_CLIENT_INSTALL_PATH="$STEAM"
# steam_api64 is only overridden in proxy mode: the real game needs the real one
# for its Steam DRM stub to unpack.
if [ "$MODE" = "proxy" ]; then
    export WINEDLLOVERRIDES="dinput8=n,b;steam_api64=n,b"
else
    export WINEDLLOVERRIDES="dinput8=n,b"
fi
export PROTON_LOG=1
# optional wrapper (e.g. "gamescope --backend headless -W 1280 -H 720 -r 30 --")
WRAP=()
[ -n "${SMOKE_WRAPPER:-}" ] && read -r -a WRAP <<< "$SMOKE_WRAPPER"
export PROTON_LOG_DIR="$SMOKE"

case "$MODE" in
  proxy)
    copy "$ROOT/build/smoke/testgame.exe" "$SMOKE/game/"
    copy "$GAME/EOSSDK-Win64-Shipping.dll" "$SMOKE/game/"
    copy "$ROOT/build/smoke/steam_api64.dll" "$SMOKE/game/"
    copy "$ROOT/build/smoke/steamclient64.dll" "$SMOKE/game/"
    TARGET="$SMOKE/game/testgame.exe"
    ;;
  ownership)
    copy "$ROOT/build/eos-ownership/testgame.exe" "$SMOKE/game/"
    copy "$ROOT/build/eos-ownership/EOSSDK-Win64-Shipping.dll" "$SMOKE/game/"
    TARGET="$SMOKE/game/testgame.exe"
    ;;
  game)
    copy "$GAME/nioh.exe" "$SMOKE/game/"
    copy "$GAME/EOSSDK-Win64-Shipping.dll" "$SMOKE/game/"
    copy "$GAME/steam_api64.dll" "$SMOKE/game/"
    copy_optional "$GAME/GfeSDK.dll" "$SMOKE/game"
    copy_optional "$GAME/AnselSDK64.dll" "$SMOKE/game"
    [ -e "$SMOKE/game/archive" ] ||
        ln -s "$GAME/archive" "$SMOKE/game/archive" || { echo "cannot link $GAME/archive" >&2; exit 1; }
    [ -e "$SMOKE/game/movie" ] ||
        ln -s "$GAME/movie" "$SMOKE/game/movie" || { echo "cannot link $GAME/movie" >&2; exit 1; }
    [ -e "$SMOKE/game/sound" ] ||
        ln -s "$GAME/sound" "$SMOKE/game/sound" || { echo "cannot link $GAME/sound" >&2; exit 1; }
    TARGET="$SMOKE/game/nioh.exe"
    ;;
  *) echo "usage: $0 proxy|game|ownership" >&2; exit 2 ;;
esac

echo "--- running $TARGET under Proton (prefix $SMOKE/pfx) ---"
cd "$SMOKE/game"
# A wrapper (gamescope) spawns children that can hold stdout open after it is
# killed, so never run this through a pipe and always follow up with SIGKILL.
if [ "${#WRAP[@]}" -gt 0 ]; then
    echo "--- wrapper: ${WRAP[*]} ---"
    timeout --foreground -k 10 "${SMOKE_TIMEOUT:-240}" "${WRAP[@]}" "$PROTON" runinprefix "$TARGET" \
        >"$SMOKE/proton.out" 2>&1
else
    timeout --foreground -k 10 "${SMOKE_TIMEOUT:-240}" "$PROTON" runinprefix "$TARGET" \
        >"$SMOKE/proton.out" 2>&1
fi
rc=$?
echo "--- proton exit=$rc ---"
echo "=== nioh3p.log ==="
if [ -e "$SMOKE/game/nioh3p.log" ] && [ ! -r "$SMOKE/game/nioh3p.log" ]; then
    echo "!! nioh3p.log exists but cannot be read: $SMOKE/game/nioh3p.log" >&2
    rc=1
elif [ -f "$SMOKE/game/nioh3p.log" ]; then
    cat "$SMOKE/game/nioh3p.log"
else
    echo "(no log produced)"
fi
echo "=== artifacts ==="
if ls "$SMOKE/game"/nioh3p_image_* >/dev/null 2>&1; then
    mkdir -p "$SMOKE/dumps" || { echo "cannot create $SMOKE/dumps" >&2; exit 1; }
    for f in "$SMOKE/game"/nioh3p_image_*; do
        cp -f -- "$f" "$SMOKE/dumps/" || { echo "cannot copy $f" >&2; exit 1; }
    done
    ls -la "$SMOKE/dumps" | tail -5
    echo "(kept in $SMOKE/dumps)"
else
    echo "(no image dump)"
fi

exit $rc
