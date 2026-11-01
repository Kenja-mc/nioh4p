#!/usr/bin/env bash
# Builds the nioh4p mod artifacts as freestanding Windows DLLs:
#   ./build.sh            core -> build/dinput8.dll and build/nioh4p.dll,
#                         add-ons -> build/addons/, research -> build/research/
#   ./build.sh --harness  also the smoke-test artifacts -> build/smoke/
#
# The DLLs must not link the UCRT: Proton's Wine ships no api-ms-win-crt-*
# apisets, so a CRT-linked dinput8.dll stops nioh.exe from starting.
# src/common/freestanding.c provides the few libc bodies; Kernel32 only.
set -euo pipefail

cd "$(dirname "$0")"

# Only --harness is accepted, and only as the sole argument.
HARNESS=0
case "${1:-}" in
"") ;;
--harness) HARNESS=1 ;;
*) echo "usage: $0 [--harness]" >&2; exit 2 ;;
esac
[ $# -le 1 ] || { echo "usage: $0 [--harness]" >&2; exit 2; }

mkdir -p build

ZIG_BIN="${ZIG:-}"
if [ -z "$ZIG_BIN" ]; then
    if command -v zig >/dev/null; then
        ZIG_BIN=zig
    elif [ -x "$HOME/.local/opt/ziglang/ziglang/zig" ]; then
        ZIG_BIN="$HOME/.local/opt/ziglang/ziglang/zig"
    fi
fi

CC=()
SYSINC=()
if [ -n "$ZIG_BIN" ]; then
    INC="$(dirname "$ZIG_BIN")/lib"
    echo "== building with $ZIG_BIN =="
    CC=("$ZIG_BIN" cc -target x86_64-windows-gnu)
    SYSINC=(-isystem "$INC/libc/include/any-windows-any" -isystem "$INC/include")
elif command -v x86_64-w64-mingw32-gcc >/dev/null; then
    echo "== building with mingw-w64 =="
    CC=(x86_64-w64-mingw32-gcc)
else
    echo "no cross compiler found; install zig or mingw-w64" >&2
    exit 1
fi

# Every DLL carries the same resource object (version info + manifest): an
# unsigned, resource-less image is what generic AV heuristics flag. Both LLVM
# tools are required; stopping here beats shipping a DLL without .rsrc.
RC_BIN="${LLVM_RC:-$(command -v llvm-rc || true)}"
CVTRES_BIN="${LLVM_CVTRES:-$(command -v llvm-cvtres || true)}"
if [ -z "$RC_BIN" ] || [ -z "$CVTRES_BIN" ]; then
    echo "need llvm-rc and llvm-cvtres (llvm package) for the resource section" >&2
    exit 1
fi
"$RC_BIN" /FO build/nioh3p.res src/proxy/nioh3p.rc
"$CVTRES_BIN" /MACHINE:X64 /OUT:build/nioh3p.rc.obj build/nioh3p.res

DLLFLAGS=(-shared -O2 -Wall -nostdlib "${SYSINC[@]}" -Wl,--entry=DllMain -lkernel32)

# build_dll <output> [defines...] <sources/defs...>
build_dll() {
    local out=$1
    shift
    "${CC[@]}" "${DLLFLAGS[@]}" "$@" -o "$out" build/nioh3p.rc.obj
}

# The core: the proxy modules (see src/proxy/nioh3p_internal.h), the add-on
# loader and the shared freestanding helpers. The player build never contains
# the research instrumentation.
CORE_SOURCES=(src/proxy/util.c src/proxy/log.c src/proxy/config.c src/proxy/pe.c \
    src/proxy/patch.c src/proxy/eos.c src/proxy/steam.c src/proxy/rules.c \
    src/proxy/gamepatch.c src/proxy/gamehooks.c src/proxy/hud.c src/proxy/prep.c \
    src/proxy/main.c src/proxy/addons.c src/common/freestanding.c)
build_dll build/dinput8.dll "${CORE_SOURCES[@]}" src/proxy/dinput8.def
# The plugin artifact is the same bytes under the loader-facing name; a copy,
# not a second link, so the two stay byte-identical.
cp build/dinput8.dll build/nioh4p.dll

# The research build is the same source with the trace instrumentation and the
# image dumper compiled in (trace=1, dump_image=1).
mkdir -p build/research
build_dll build/research/dinput8.dll -DNIOH3P_RESEARCH=1 \
    "${CORE_SOURCES[@]}" src/proxy/dinput8.def
# The research counterpart of the player plugin copy above: same bytes,
# plugin name, or the pechecksum sweep below walks off a missing file.
cp build/research/dinput8.dll build/research/nioh4p.dll

# The five gameplay add-ons: each links its feature source, the shared runtime
# (which provides DllMain) and the freestanding helpers. The descriptor ID
# order in src/common/nioh4p_addon.h fixes this list; the output name maps the
# underscore source stems to hyphenated DLL names.
ADDONS="shrines kodama enemy_health assist_gauge shrine_respawn"
mkdir -p build/addons build/research/addons
for feature in $ADDONS; do
    src="src/addons/$feature.c"
    [ -f "$src" ] || { echo "missing add-on source: $src" >&2; exit 1; }
    name="nioh4p-${feature//_/-}"
    build_dll "build/addons/$name.dll" \
        "$src" src/addons/runtime.c src/common/freestanding.c src/addons/addon.def
    build_dll "build/research/addons/$name.dll" -DNIOH3P_RESEARCH=1 \
        "$src" src/addons/runtime.c src/common/freestanding.c src/addons/addon.def
done

# A zero optional-header checksum is a generic-heuristic signal; recompute it.
python3 tools/pechecksum.py build/dinput8.dll
python3 tools/pechecksum.py build/nioh4p.dll
python3 tools/pechecksum.py build/research/dinput8.dll
python3 tools/pechecksum.py build/research/nioh4p.dll
for dll in build/addons/*.dll build/research/addons/*.dll; do
    python3 tools/pechecksum.py "$dll"
done

echo "== built the core (build/dinput8.dll = build/nioh4p.dll),"
echo "   five add-ons (build/addons/) and the research counterparts =="
ls -l build/dinput8.dll build/nioh4p.dll build/addons/*.dll

[ "$HARNESS" = 1 ] || exit 0

DLLTOOL="${DLLTOOL:-$(command -v llvm-dlltool || command -v x86_64-w64-mingw32-dlltool || true)}"
[ -n "$DLLTOOL" ] || { echo "harness needs llvm-dlltool (llvm) or mingw-w64-dlltool" >&2; exit 1; }

SMOKE=build/smoke
mkdir -p "$SMOKE"

# fake steam_api64: the two matchmaking thunks (assembly, byte-identical to the
# shipped DLL) plus the interface factory in the "steamclient" module.
"${CC[@]}" "${DLLFLAGS[@]}" \
    -o "$SMOKE/steam_api64.dll" \
    tools/testgame/fake_steam.c tools/testgame/fake_steam_thunks.S tools/testgame/fake_steam.def
"${CC[@]}" "${DLLFLAGS[@]}" \
    -o "$SMOKE/steamclient64.dll" \
    tools/testgame/fake_steamclient.c tools/testgame/fake_steamclient.def

# import libraries and the harness itself; the .a files are regenerated every
# run because a stale one silently hides new .def entries
"$DLLTOOL" -m i386:x86-64 -d tools/testgame/fake_steam.def       -l "$SMOKE/libfakesteam.a"
"$DLLTOOL" -m i386:x86-64 -d tools/testgame/eossdk.def           -l "$SMOKE/libeossdk.a"
"$DLLTOOL" -m i386:x86-64 -d src/proxy/dinput8.def               -l "$SMOKE/libdinput8.a"

"${CC[@]}" -O2 -Wall -nostdlib "${SYSINC[@]}" \
    -Wl,--entry=mainCRTStartup -Wl,--subsystem,console \
    -o "$SMOKE/testgame.exe" tools/testgame/testgame.c \
    "$SMOKE/libeossdk.a" "$SMOKE/libdinput8.a" "$SMOKE/libfakesteam.a" -lkernel32

# Separate fixture: caller-owned CopyInfo lifetime regression.
OWNERSHIP=build/eos-ownership
mkdir -p "$OWNERSHIP"
"${CC[@]}" "${DLLFLAGS[@]}" \
    -o "$OWNERSHIP/EOSSDK-Win64-Shipping.dll" tools/testgame/fake_eos_info.c
"$DLLTOOL" -m i386:x86-64 -d tools/testgame/fake_eos_info.def -l "$OWNERSHIP/libeos.a"
"${CC[@]}" -O2 -Wall -nostdlib "${SYSINC[@]}" \
    -Wl,--entry=mainCRTStartup -Wl,--subsystem,console \
    -o "$OWNERSHIP/testgame.exe" tools/testgame/eos_ownership.c \
    "$OWNERSHIP/libeos.a" "$SMOKE/libdinput8.a" -lkernel32

# Separate fixture: wounded HP across native status recalculations.
ENEMY_HEALTH=build/enemy-health
mkdir -p "$ENEMY_HEALTH"
"${CC[@]}" -O2 -Wall -Wextra -nostdlib "${SYSINC[@]}" \
    -Wl,--entry=mainCRTStartup -Wl,--subsystem,console \
    -o "$ENEMY_HEALTH/testgame.exe" tools/testgame/enemy_health.c \
    src/common/freestanding.c -lkernel32

echo "== built harness =="
ls -l "$SMOKE"
