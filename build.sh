#!/usr/bin/env bash
# Builds the nioh3p mod (dinput8 proxy) as a freestanding Windows DLL.
#
#   ./build.sh            the proxy only -> build/dinput8.dll
#   ./build.sh --harness  also the smoke-test artifacts -> build/smoke/
#
# Needs a cross compiler. Any of these work:
#   zig cc  (used by default when found in PATH or at ~/.local/opt/ziglang)
#   x86_64-w64-mingw32-gcc (mingw-w64)
#
# The DLL must NOT link the UCRT: Proton's Wine ships no api-ms-win-crt-*
# apisets, so a CRT-linked dinput8.dll makes nioh.exe fail to start.
set -euo pipefail

cd "$(dirname "$0")"
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

# The proxy is loaded as dinput8.dll, so it carries a .def with the forwarded
# DirectInput8Create and the other DLL entry points the loader expects.
"${CC[@]}" -shared -O2 -Wall -nostdlib "${SYSINC[@]}" \
    -Wl,--entry=DllMain -lkernel32 \
    -o build/dinput8.dll src/proxy/nioh3p_proxy.c src/proxy/dinput8.def

echo "== built build/dinput8.dll =="
ls -l build/dinput8.dll

[ "${1:-}" = "--harness" ] || exit 0

DLLTOOL="${DLLTOOL:-$(command -v llvm-dlltool || command -v x86_64-w64-mingw32-dlltool || true)}"
[ -n "$DLLTOOL" ] || { echo "harness needs llvm-dlltool (llvm) or mingw-w64-dlltool" >&2; exit 1; }

SMOKE=build/smoke
mkdir -p "$SMOKE"
DLLFLAGS=(-shared -O2 -Wall -nostdlib "${SYSINC[@]}" -Wl,--entry=DllMain -lkernel32)

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

echo "== built harness =="
ls -l "$SMOKE"
