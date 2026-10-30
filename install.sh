#!/usr/bin/env bash
# Installs / removes the nioh4p core and its gameplay add-ons inside the
# Steam installation of Nioh: Complete Edition (appid 485510) on this machine.
#
#   ./install.sh                      install the core only: dinput8.dll (the
#                                     proxy) and nioh3p.ini next to nioh.exe
#   ./install.sh plugin               install the core as DLLMods/nioh4p.dll
#                                     for an external loader you already run;
#                                     a dinput8.dll is never touched
#   ./install.sh addon <name>         install one gameplay add-on into
#                                     DLLMods/ (its DLL and its ini)
#   ./install.sh remove-addon <name>  remove one gameplay add-on (its ini and
#                                     your settings stay)
#   ./install.sh uninstall            remove the core and every add-on
#
# Add-on names (exactly these five; the core loads no others):
#   nioh4p-shrines nioh4p-kodama nioh4p-enemy-health
#   nioh4p-assist-gauge nioh4p-shrine-respawn
#
# The add-ons only do something when the core is installed: the core finds
# them in the game-root DLLMods folder, no matter where the core itself is
# loaded from. An add-on never loads anything on its own.
#
# nioh3p.sites.txt is NOT installed any more: the call-site names it provides
# only appear with trace=1 (see nioh3p.ini, "developer options"). Developers
# who run with trace=1 can copy it into the game folder themselves:
#   tools/sites.py > nioh3p.sites.txt && cp nioh3p.sites.txt "$NIOH_DIR/"
set -euo pipefail

cd "$(dirname "$0")"
GAME="${NIOH_DIR:-$HOME/.local/share/Steam/steamapps/common/Nioh}"
MODS="$GAME/DLLMods"

# The public add-on ids, without the nioh4p- prefix. The source stems in
# src/addons use underscores where the id uses a hyphen; only build.sh maps
# between them when it names the DLL.
ADDONS="shrines kodama enemy-health assist-gauge shrine-respawn"

# An existing DLL is ours if it carries the marker string below: the core (as
# dinput8.dll or nioh4p.dll) carries it in its proxy code, an add-on carries
# the exported Nioh4pAddonInit name. Anything else at one of our file names
# belongs to somebody else and is never deleted or overwritten.
is_core() {
    [ -e "$1" ] && grep -qa "nioh3p proxy" "$1"
}
is_addon() {
    [ -e "$1" ] && grep -qa "Nioh4pAddonInit" "$1"
}

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

# Install a DLL by rename: a running game keeps its old mapping and is never
# corrupted.
install_dll() {
    local tmp="$(dirname "$2")/.$(basename "$2").new"
    install -m 0755 "$1" "$tmp"
    mv -f "$tmp" "$2"
}

# Install the core config next to nioh.exe. The ini is replaced when it
# differs from the repo one, because a stale ini is the usual reason an
# install behaves differently from the tested rig. The old file is kept as
# <name>.bak; if that name is already taken, a timestamp is added so an
# earlier backup is never overwritten.
install_ini() {
    local target=$1 bak stamp n=0
    if [ -f "$target" ] && ! same_text "nioh3p.ini" "$target"; then
        bak="$target.bak"
        if [ -e "$bak" ]; then
            stamp=$(date +%Y%m%d-%H%M%S)
            bak="$target.bak.$stamp"
            while [ -e "$bak" ]; do
                n=$((n + 1))
                bak="$target.bak.$stamp-$n"
            done
        fi
        mv -f "$target" "$bak"
        echo "existing config differed; old file kept as $bak"
    fi
    if [ ! -f "$target" ] || ! same_text "nioh3p.ini" "$target"; then
        install -m 0644 nioh3p.ini "$(dirname "$target")/.$(basename "$target").new"
        mv -f "$(dirname "$target")/.$(basename "$target").new" "$target"
    fi
}

# Rebuild when the artifact is missing or older than any input: build.sh links
# in place and stops before the link on a compile error, so gating the rebuild
# on the DLL existing would install the DLL from before the edit. -type f: the
# mtime of a source directory itself says nothing about the sources. The core
# links src/proxy and the shared src/common helpers; an add-on links
# src/addons and src/common.
core_stale() {
    [ ! -f build/dinput8.dll ] || [ ! -f build/nioh4p.dll ] ||
        [ -n "$(find build.sh src/proxy src/common -type f -newer build/dinput8.dll -print -quit)" ]
}
addon_stale() {
    local out="build/addons/nioh4p-$1.dll"
    [ ! -f "$out" ] ||
        [ -n "$(find build.sh src/addons src/common -type f -newer "$out" -print -quit)" ]
}

usage() {
    echo "usage: $0 | $0 plugin | $0 addon <name> | $0 remove-addon <name> | $0 uninstall" >&2
    echo "       <name>: nioh4p-shrines nioh4p-kodama nioh4p-enemy-health" >&2
    echo "               nioh4p-assist-gauge nioh4p-shrine-respawn" >&2
}

MODE=${1:-core}
ARG=${2:-}
case "$MODE" in
core)
    [ $# -eq 0 ] || { usage; exit 2; }
    ;;
plugin | uninstall)
    [ $# -eq 1 ] || { usage; exit 2; }
    ;;
addon | remove-addon)
    [ $# -eq 2 ] || { usage; exit 2; }
    case " $ADDONS " in
    *" $ARG "*) ;;
    *) echo "unknown add-on: $ARG" >&2; usage; exit 2 ;;
    esac
    ;;
*)
    usage
    exit 2
    ;;
esac

# The directory must hold the game, not merely exist: NIOH_DIR=$HOME used to
# pass a -d test and get the mod written into the home directory.
[ -d "$GAME" ] || { echo "game directory not found: $GAME" >&2; exit 1; }
[ -f "$GAME/nioh.exe" ] && [ -f "$GAME/EOSSDK-Win64-Shipping.dll" ] ||
    { echo "no nioh.exe/EOSSDK-Win64-Shipping.dll in $GAME; not a Nioh install" >&2; exit 1; }

removed=()

if [ "$MODE" = uninstall ]; then
    # Our own proxy goes; a foreign dinput8.dll belongs to somebody else's
    # mod and is neither deleted nor a reason to stop: our plugin and add-ons
    # are removed regardless of what else lives in the game directory.
    if [ -e "$GAME/dinput8.dll" ]; then
        if is_core "$GAME/dinput8.dll"; then
            put_away "$GAME/dinput8.dll"
        else
            echo "leaving the foreign dinput8.dll in place: not ours"
        fi
    fi
    put_away "$GAME/nioh3p.ini"
    put_away "$GAME/nioh3p.sites.txt"
    if [ -e "$MODS/nioh4p.dll" ]; then
        if is_core "$MODS/nioh4p.dll"; then
            put_away "$MODS/nioh4p.dll"
        else
            echo "leaving $MODS/nioh4p.dll in place: not ours"
        fi
    fi
    for id in $ADDONS; do
        name="nioh4p-$id"
        if [ -e "$MODS/$name.dll" ]; then
            if is_addon "$MODS/$name.dll"; then
                put_away "$MODS/$name.dll"
            else
                echo "leaving $MODS/$name.dll in place: not ours"
            fi
        fi
        put_away "$MODS/$name.ini"
    done
    if [ "${#removed[@]}" -gt 0 ]; then
        list=$(printf '%s, ' "${removed[@]}")
        echo "removed ${list%, } from $GAME"
    else
        echo "nothing to remove from $GAME"
    fi
    echo "(logs left in place: $GAME/nioh3p.log, $GAME/nioh3p.prev.log)"
    exit 0
fi

if [ "$MODE" = core ]; then
    # A foreign dinput8.dll means another mod owns the proxy slot; installing
    # ours would destroy it. The plugin mode installs the same core without
    # touching dinput8.dll.
    if [ -e "$GAME/dinput8.dll" ] && ! is_core "$GAME/dinput8.dll"; then
        echo "!! $GAME/dinput8.dll already exists and is not nioh4p"
        echo "   to keep it, install the core as a plugin instead:"
        echo "     ./install.sh plugin"
        echo "   or move it aside first, or back it up as dinput8.dll.nioh4p-backup"
        exit 1
    fi
    if [ -e "$MODS/nioh4p.dll" ]; then
        echo "!! $MODS/nioh4p.dll already exists"
        if is_core "$MODS/nioh4p.dll"; then
            echo "   the core is already installed as a plugin; running both copies"
            echo "   would install every hook twice. Remove it first:"
            echo "     ./install.sh uninstall"
        else
            echo "   it is not ours; move it aside first"
        fi
        exit 1
    fi
    if core_stale; then ./build.sh; fi
    install_dll build/dinput8.dll "$GAME/dinput8.dll"
    install_ini "$GAME/nioh3p.ini"
    echo "installed:"
    ls -l "$GAME/dinput8.dll" "$GAME/nioh3p.ini"
    cat <<'EOF'

Launch the game through Steam as usual. The mod writes its log to the game
directory:
  nioh3p.log       this launch
  nioh3p.prev.log  the launch before it (kept automatically)

No image dumps are written: dump_image=0 in nioh3p.ini. For research runs set
dump_image=1 (see the "developer options" section of the ini).

The core alone gives lobby capacity and network synchronization. The gameplay
add-ons (shrines, kodama, enemy health, assist gauge, shrine respawn) are
separate DLLs in the game's DLLMods folder; install them with
  ./install.sh addon <name>

Uninstall with ./install.sh uninstall
EOF
    exit 0
fi

if [ "$MODE" = plugin ]; then
    # The plugin artifact is for a loader that is already installed and that
    # the user maintains. This script never touches dinput8.dll here: the
    # foreign proxy keeps working, and installing our core twice (proxy and
    # plugin) would install every hook twice.
    if is_core "$GAME/dinput8.dll"; then
        echo "!! the core is already installed as $GAME/dinput8.dll"
        echo "   remove it first so the core is not loaded twice:"
        echo "     ./install.sh uninstall"
        exit 1
    fi
    if [ -e "$GAME/dinput8.dll" ]; then
        echo "leaving the foreign dinput8.dll in place: not ours"
    fi
    if [ -e "$MODS/nioh4p.dll" ] && ! is_core "$MODS/nioh4p.dll"; then
        echo "!! $MODS/nioh4p.dll already exists and is not nioh4p; move it aside first" >&2
        exit 1
    fi
    if core_stale; then ./build.sh; fi
    mkdir -p "$MODS"
    install_dll build/nioh4p.dll "$MODS/nioh4p.dll"
    install_ini "$GAME/nioh3p.ini"
    echo "installed:"
    ls -l "$MODS/nioh4p.dll" "$GAME/nioh3p.ini"
    cat <<'EOF'

DLLMods/nioh4p.dll is the core for an external loader. This mod ships no
loader and has not been tested with any; your loader must load it, and the
gameplay add-ons still come from DLLMods/. If your dinput8.dll is the mod
proxy itself, use the plain install instead:
  ./install.sh

Uninstall with ./install.sh uninstall
EOF
    exit 0
fi

# addon <name> / remove-addon <name>
id=$ARG
name="nioh4p-$id"

if [ "$MODE" = addon ]; then
    if ! is_core "$GAME/dinput8.dll" && ! is_core "$MODS/nioh4p.dll"; then
        echo "note: no nioh4p core is installed; $name does nothing until the"
        echo "      core is installed (./install.sh or ./install.sh plugin)"
    fi
    if [ -e "$MODS/$name.dll" ] && ! is_addon "$MODS/$name.dll"; then
        echo "!! $MODS/$name.dll already exists and is not ours; move it aside first" >&2
        exit 1
    fi
    [ -f "package/addons/$name.ini" ] ||
        { echo "missing package/addons/$name.ini" >&2; exit 1; }
    if addon_stale "$id"; then ./build.sh; fi
    mkdir -p "$MODS"
    install_dll "build/addons/$name.dll" "$MODS/$name.dll"
    if [ -f "$MODS/$name.ini" ]; then
        echo "keeping the existing $name.ini (your settings; missing keys use"
        echo "the shipped defaults)"
    else
        install -m 0644 "package/addons/$name.ini" "$MODS/.$name.ini.new"
        mv -f "$MODS/.$name.ini.new" "$MODS/$name.ini"
    fi
    echo "installed:"
    ls -l "$MODS/$name.dll" "$MODS/$name.ini"
    echo "config changes take effect at the next launch."
    exit 0
fi

# remove-addon: only our own DLL at that exact name is removed. The ini stays,
# so reinstalling the add-on later keeps the user's settings.
if [ -e "$MODS/$name.dll" ]; then
    if is_addon "$MODS/$name.dll"; then
        put_away "$MODS/$name.dll"
    else
        echo "leaving $MODS/$name.dll in place: not ours"
    fi
else
    echo "$name is not installed"
fi
if [ -e "$MODS/$name.ini" ]; then
    echo "kept $MODS/$name.ini with your settings; delete it to reset the"
    echo "shipped defaults"
fi
exit 0
