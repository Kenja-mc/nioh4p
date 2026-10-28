#!/usr/bin/env bash
# Installs / removes the nioh3p mod inside the Steam installation of
# Nioh: Complete Edition (appid 485510) on this machine.
#
#   ./install.sh            install (rebuild the proxy first when build/dinput8.dll
#                           is missing or older than src/proxy/* or build.sh)
#   ./install.sh uninstall  remove the mod, leave the game stock
#
# The game directory gets two files: dinput8.dll (the proxy) and nioh3p.ini
# (config). Remove them and the game is stock again.
#
# nioh3p.sites.txt is NOT installed any more: the call-site names it provides
# only appear with trace=1 (see nioh3p.ini, "developer options"). Developers
# who run with trace=1 can copy it into the game folder themselves:
#   tools/sites.py > nioh3p.sites.txt && cp nioh3p.sites.txt "$NIOH_DIR/"
set -euo pipefail

cd "$(dirname "$0")"
GAME="${NIOH_DIR:-$HOME/.local/share/Steam/steamapps/common/Nioh}"

# Remove a file from the game directory without deleting it outright: prefer
# the trash, otherwise park it in a subfolder of the game directory.
put_away() {
    local f=$1
    [ -e "$f" ] || return 0
    if command -v trash-put >/dev/null; then
        trash-put -- "$f"
    else
        mkdir -p "$GAME/.nioh3p-removed"
        mv -f "$f" "$GAME/.nioh3p-removed/"
    fi
    removed+=("$(basename "$f")")
}

# Compare two text files ignoring CR line endings, so a CRLF copy of the same
# content counts as identical. Hashes, not cmp on process substitutions: an
# unreadable file feeds both streams no bytes and cmp then reports "identical".
same_text() {
    local a b
    [ -r "$1" ] || { echo "cannot read $1" >&2; exit 1; }
    [ -r "$2" ] || { echo "cannot read $2" >&2; exit 1; }
    a=$(tr -d '\r' < "$1" | sha256sum) || { echo "cannot hash $1" >&2; exit 1; }
    b=$(tr -d '\r' < "$2" | sha256sum) || { echo "cannot hash $2" >&2; exit 1; }
    [ "$a" = "$b" ]
}

# An existing dinput8.dll is fine if it is one of ours (the proxy carries the
# marker string below); anything else belongs to somebody else's mod.
is_foreign_dll() {
    [ -e "$1" ] && ! grep -qa "nioh3p proxy" "$1"
}

foreign_dll_error() {
    echo "!! $GAME/dinput8.dll already exists and is not nioh3p"
    echo "   move it aside first, or back it up as dinput8.dll.nioh3p-backup"
}

# The directory must hold the game, not merely exist: NIOH_DIR=$HOME used to
# pass a -d test and get the mod written into the home directory.
[ -d "$GAME" ] || { echo "game directory not found: $GAME" >&2; exit 1; }
[ -f "$GAME/nioh.exe" ] && [ -f "$GAME/EOSSDK-Win64-Shipping.dll" ] ||
    { echo "no nioh.exe/EOSSDK-Win64-Shipping.dll in $GAME; not a Nioh install" >&2; exit 1; }

is_foreign_dll "$GAME/dinput8.dll" && { foreign_dll_error; exit 1; }

if [ "${1:-install}" = "uninstall" ]; then
    # The same scan again, right before the removal: a foreign DLL that
    # appeared after the check above must never be deleted as ours.
    if is_foreign_dll "$GAME/dinput8.dll"; then
        foreign_dll_error
        exit 1
    fi
    removed=()
    put_away "$GAME/dinput8.dll"
    put_away "$GAME/nioh3p.ini"
    put_away "$GAME/nioh3p.sites.txt"
    if [ "${#removed[@]}" -gt 0 ]; then
        list=$(printf '%s, ' "${removed[@]}")
        echo "removed ${list%, } from $GAME"
    else
        echo "nothing to remove from $GAME"
    fi
    echo "(logs left in place: $GAME/nioh3p.log, $GAME/nioh3p.prev.log)"
    exit 0
fi

# Rebuild when the artifact is missing or older than any input: build.sh links in
# place and stops before the link on a compile error, so gating the rebuild on
# the DLL existing would install the DLL from before the edit. -type f: the
# mtime of the src/proxy directory itself says nothing about the sources.
if [ ! -f build/dinput8.dll ] ||
    [ -n "$(find build.sh src/proxy -type f -newer build/dinput8.dll -print -quit)" ]; then
    ./build.sh
fi

# Install by rename: a running game keeps its old mapping and is never corrupted.
install -m 0755 build/dinput8.dll "$GAME/.dinput8.dll.new"
mv -f "$GAME/.dinput8.dll.new" "$GAME/dinput8.dll"

# The ini is replaced when it differs from the repo one, because a stale ini is
# the usual reason an install behaves differently from the tested rig. The old
# file is kept as nioh3p.ini.bak; if that name is already taken, a timestamp is
# added so an earlier backup is never overwritten.
if [ -f "$GAME/nioh3p.ini" ] && ! same_text nioh3p.ini "$GAME/nioh3p.ini"; then
    bak="$GAME/nioh3p.ini.bak"
    if [ -e "$bak" ]; then
        stamp=$(date +%Y%m%d-%H%M%S)
        bak="$GAME/nioh3p.ini.bak.$stamp"
        n=0
        while [ -e "$bak" ]; do
            n=$((n + 1))
            bak="$GAME/nioh3p.ini.bak.$stamp-$n"
        done
    fi
    mv -f "$GAME/nioh3p.ini" "$bak"
    echo "existing config differed; old file kept as $bak"
fi
if [ ! -f "$GAME/nioh3p.ini" ] || ! same_text nioh3p.ini "$GAME/nioh3p.ini"; then
    install -m 0644 nioh3p.ini "$GAME/.nioh3p.ini.new"
    mv -f "$GAME/.nioh3p.ini.new" "$GAME/nioh3p.ini"
fi

echo "installed:"
ls -l "$GAME/dinput8.dll" "$GAME/nioh3p.ini"
cat <<'EOF'

Launch the game through Steam as usual. The mod writes its log to the game
directory:
  nioh3p.log       this launch
  nioh3p.prev.log  the launch before it (kept automatically)

No image dumps are written: dump_image=0 in nioh3p.ini. For research runs set
dump_image=1 (see the "developer options" section of the ini).

Uninstall with ./install.sh uninstall
EOF
