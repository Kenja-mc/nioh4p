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
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GAME="$HOME/.local/share/Steam/steamapps/common/Nioh"
PROTON="$HOME/.local/share/Steam/steamapps/common/Proton - Experimental/proton"
STEAM="$HOME/.local/share/Steam"
MODE="${1:-proxy}"
SMOKE="${SMOKE_DIR:-$HOME/.cache/nioh3p/smoke}"

mkdir -p "$SMOKE/game" "$SMOKE/pfx"
cp -f "$ROOT/build/dinput8.dll" "$SMOKE/game/"
cp -f "$ROOT/nioh3p.ini" "$SMOKE/game/"
# The harness modes exercise the full research instrumentation by default; the
# game run defaults to player mode with image dumps, as tools/quiet.sh game does.
case "$MODE" in
  proxy|ownership)
    echo "trace=${NIOH3P_TRACE:-1}" >> "$SMOKE/game/nioh3p.ini"
    ;;
  game)
    echo "trace=${NIOH3P_TRACE:-0}" >> "$SMOKE/game/nioh3p.ini"
    echo "dump_image=${NIOH3P_DUMP:-1}" >> "$SMOKE/game/nioh3p.ini"
    echo "dump_every_s=${NIOH3P_DUMP_EVERY:-8}" >> "$SMOKE/game/nioh3p.ini"
    ;;
esac
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
    cp -f "$ROOT/build/smoke/testgame.exe" "$SMOKE/game/"
    cp -f "$GAME/EOSSDK-Win64-Shipping.dll" "$SMOKE/game/"
    cp -f "$ROOT/build/smoke/steam_api64.dll" "$SMOKE/game/"
    cp -f "$ROOT/build/smoke/steamclient64.dll" "$SMOKE/game/"
    TARGET="$SMOKE/game/testgame.exe"
    ;;
  ownership)
    cp -f "$ROOT/build/eos-ownership/testgame.exe" "$SMOKE/game/"
    cp -f "$ROOT/build/eos-ownership/EOSSDK-Win64-Shipping.dll" "$SMOKE/game/"
    TARGET="$SMOKE/game/testgame.exe"
    ;;
  game)
    cp -f "$GAME/nioh.exe" "$SMOKE/game/"
    cp -f "$GAME/EOSSDK-Win64-Shipping.dll" "$SMOKE/game/"
    cp -f "$GAME/steam_api64.dll" "$SMOKE/game/"
    cp -f "$GAME/GfeSDK.dll" "$SMOKE/game/" 2>/dev/null
    cp -f "$GAME/AnselSDK64.dll" "$SMOKE/game/" 2>/dev/null
    [ -e "$SMOKE/game/archive" ] || ln -s "$GAME/archive" "$SMOKE/game/archive"
    [ -e "$SMOKE/game/movie" ]   || ln -s "$GAME/movie"   "$SMOKE/game/movie"
    [ -e "$SMOKE/game/sound" ]   || ln -s "$GAME/sound"   "$SMOKE/game/sound"
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
[ -f "$SMOKE/game/nioh3p.log" ] && cat "$SMOKE/game/nioh3p.log" || echo "(no log produced)"
echo "=== artifacts ==="
if ls "$SMOKE/game"/nioh3p_image_* >/dev/null 2>&1; then
    mkdir -p "$SMOKE/dumps"
    for f in "$SMOKE/game"/nioh3p_image_*; do
        cp -f "$f" "$SMOKE/dumps/"
    done
    ls -la "$SMOKE/dumps" | tail -5
    echo "(kept in $SMOKE/dumps)"
else
    echo "(no image dump)"
fi

exit $rc
