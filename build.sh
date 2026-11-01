#!/usr/bin/env bash
# Builds the nioh4p mod artifacts as freestanding Windows DLLs.
#
#   ./build.sh            player build: the core -> build/dinput8.dll and an
#                         identical copy build/nioh4p.dll, the five gameplay
#                         add-ons -> build/addons/, and the research
#                         counterparts -> build/research/
#   ./build.sh --harness  also the smoke-test artifacts -> build/smoke/
#
# Needs a cross compiler. Any of these work:
#   zig cc  (used by default when found in PATH or at ~/.local/opt/ziglang)
#   x86_64-w64-mingw32-gcc (mingw-w64)
#
# The DLLs must NOT link the UCRT: Proton's Wine ships no api-ms-win-crt-*
# apisets, so a CRT-linked dinput8.dll makes nioh.exe fail to start. The
# shared helpers in src/common/freestanding.c provide the few libc bodies the
# code needs and import Kernel32 only.
set -euo pipefail

cd "$(dirname "$0")"

# Only --harness is accepted, and only as the sole argument. An unknown or extra
# argument used to fall through the "[ $1 = --harness ] || exit 0" line at the
# end: ./build.sh --harnes printed nothing and exited 0.
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

# Every DLL carries the same resource object: version info and the application
# manifest. The proxy is loaded as dinput8.dll, so it carries a .def with the
# forwarded DirectInput8Create and the other DLL entry points the loader
# expects; the add-ons share src/addons/addon.def, which exports only
# Nioh4pAddonInit.
#
# Version info and the application manifest live in a resource section. An
# unsigned, resource-less image is what generic AV heuristics (Evo-gen,
# TR/W64.Evo) flag; the manifest also marks the DLL asInvoker with Win10/11
# compatibility and per-monitor DPI awareness. Both LLVM tools are required:
# stopping here is better than silently shipping a DLL without .rsrc.
# Shared resource strings identify the project, not one particular DLL name;
# the same accurate metadata can therefore accompany the core and add-ons.
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

# The core: the dinput8 proxy split into per-feature modules (see
# src/proxy/nioh3p_internal.h for the layout), the optional add-on loader
# (src/proxy/addons.c) and the shared freestanding helpers. The player build
# never contains the research instrumentation.
CORE_SOURCES=(src/proxy/util.c src/proxy/log.c src/proxy/config.c src/proxy/pe.c \
    src/proxy/patch.c src/proxy/eos.c src/proxy/steam.c src/proxy/rules.c \
    src/proxy/gamepatch.c src/proxy/gamehooks.c src/proxy/hud.c src/proxy/prep.c \
    src/proxy/main.c src/proxy/addons.c src/common/freestanding.c)
build_dll build/dinput8.dll "${CORE_SOURCES[@]}" src/proxy/dinput8.def
# The same core bytes as the plugin artifact for an external loader; the core
# finds its add-ons in the game-root DLLMods folder wherever it is loaded
# from. A copy, not a second link: the two artifacts stay byte-identical.
cp build/dinput8.dll build/nioh4p.dll

# The research build is the same source with the trace-only instrumentation and
# the image dumper compiled in (trace=1, dump_image=1). build/dinput8.dll and
# build/nioh4p.dll - the player builds that tools/package.sh and install.sh
# ship - never contain them.
mkdir -p build/research
build_dll build/research/dinput8.dll -DNIOH3P_RESEARCH=1 \
    "${CORE_SOURCES[@]}" src/proxy/dinput8.def

# The five gameplay add-ons. Each links only its own feature source, the
# shared add-on runtime (which also provides DllMain) and the freestanding
# helpers. The research build uses the same NIOH3P_RESEARCH define as the
# core research build, which compiles the observation-only diagnostics in.
# The descriptor ID order in src/common/nioh4p_addon.h fixes this list. The
# source stems use underscores, the shipped DLL names use hyphens; only the
# output name maps "_" to "-".
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

# A zero optional-header checksum is a common property of hand-built binaries
# and one more generic-heuristic signal; recompute it after the link.
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
# shipped DLL) plus the interface factory, implementation in the "steamclient"
# module next to it - the same split the real DLLs have.
"${CC[@]}" "${DLLFLAGS[@]}" \
    -o "$SMOKE/steam_api64.dll" \
    tools/testgame/fake_steam.c tools/testgame/fake_steam_thunks.S tools/testgame/fake_steam.def
"${CC[@]}" "${DLLFLAGS[@]}" \
    -o "$SMOKE/steamclient64.dll" \
    tools/testgame/fake_steamclient.c tools/testgame/fake_steamclient.def

# import libraries and the harness itself
# regenerated every run: a stale .a silently hides new .def entries
"$DLLTOOL" -m i386:x86-64 -d tools/testgame/fake_steam.def       -l "$SMOKE/libfakesteam.a"
"$DLLTOOL" -m i386:x86-64 -d tools/testgame/eossdk.def           -l "$SMOKE/libeossdk.a"
"$DLLTOOL" -m i386:x86-64 -d src/proxy/dinput8.def               -l "$SMOKE/libdinput8.a"

"${CC[@]}" -O2 -Wall -nostdlib "${SYSINC[@]}" \
    -Wl,--entry=mainCRTStartup -Wl,--subsystem,console \
    -o "$SMOKE/testgame.exe" tools/testgame/testgame.c \
    "$SMOKE/libeossdk.a" "$SMOKE/libdinput8.a" "$SMOKE/libfakesteam.a" -lkernel32

# Separate SDK fixture for the caller-owned CopyInfo lifetime regression.
OWNERSHIP=build/eos-ownership
mkdir -p "$OWNERSHIP"
"${CC[@]}" "${DLLFLAGS[@]}" \
    -o "$OWNERSHIP/EOSSDK-Win64-Shipping.dll" tools/testgame/fake_eos_info.c
"$DLLTOOL" -m i386:x86-64 -d tools/testgame/fake_eos_info.def -l "$OWNERSHIP/libeos.a"
"${CC[@]}" -O2 -Wall -nostdlib "${SYSINC[@]}" \
    -Wl,--entry=mainCRTStartup -Wl,--subsystem,console \
    -o "$OWNERSHIP/testgame.exe" tools/testgame/eos_ownership.c \
    "$OWNERSHIP/libeos.a" "$SMOKE/libdinput8.a" -lkernel32

# Separate fixture for wounded HP across native status recalculations.
ENEMY_HEALTH=build/enemy-health
mkdir -p "$ENEMY_HEALTH"
"${CC[@]}" -O2 -Wall -Wextra -nostdlib "${SYSINC[@]}" \
    -Wl,--entry=mainCRTStartup -Wl,--subsystem,console \
    -o "$ENEMY_HEALTH/testgame.exe" tools/testgame/enemy_health.c \
    src/common/freestanding.c -lkernel32

echo "== built harness =="
ls -l "$SMOKE"
