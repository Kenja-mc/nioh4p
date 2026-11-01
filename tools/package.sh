#!/usr/bin/env bash
# Builds the player packages in dist/:
#
#   dist/nioh4p-player.zip           the core standalone: dinput8.dll,
#                                    nioh3p.ini and README.md at the zip root
#   dist/nioh4p-plugin.zip           the same core for an external loader the
#                                    user already runs: DLLMods/nioh4p.dll in
#                                    the zip, nioh3p.ini and README.md at the
#                                    zip root
#   dist/nioh4p-shrines.zip          one gameplay add-on each:
#   dist/nioh4p-kodama.zip           DLLMods/<name>.dll and
#   dist/nioh4p-enemy-health.zip     DLLMods/<name>.ini in the zip, with
#   dist/nioh4p-assist-gauge.zip     README.md at the zip root
#   dist/nioh4p-shrine-respawn.zip
#   dist/nioh4p-all.zip              the standalone core plus every add-on,
#                                    ready to extract into the game folder
#
# Each zip mirrors the paths inside the game folder: the core standalone files
# belong next to nioh.exe, while the plugin and add-on files belong in the
# game's DLLMods folder. The frozen dist/nioh3p-player.zip of the previous
# combined candidate is never written: these are all new names.
#
# The text files are converted to CRLF line endings in the zip so that
# Windows Notepad shows them properly. Nothing outside build/ is removed;
# the staging directory goes to the trash afterwards (deleted if trash-put
# is not installed).
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT=$PWD

# The public add-on ids, without the nioh4p- prefix; build.sh maps the
# underscore source stems to these hyphenated DLL names.
ADDONS="shrines kodama enemy-health assist-gauge shrine-respawn"

./build.sh

[ -f build/dinput8.dll ] || { echo "build/dinput8.dll missing" >&2; exit 1; }
[ -f build/nioh4p.dll ] || { echo "build/nioh4p.dll missing" >&2; exit 1; }
[ -f nioh3p.ini ] || { echo "nioh3p.ini missing" >&2; exit 1; }
[ -f package/README.md ] || { echo "package/README.md missing" >&2; exit 1; }
for id in $ADDONS; do
    name="nioh4p-$id"
    [ -f "build/addons/$name.dll" ] || { echo "build/addons/$name.dll missing" >&2; exit 1; }
    [ -f "package/addons/$name.ini" ] || { echo "package/addons/$name.ini missing" >&2; exit 1; }
done

mkdir -p build dist
STAGE=$(mktemp -d build/player-package.XXXXXX)
# Trash when available, delete otherwise: the staging copy is disposable, and a
# missing trash-put used to leave build/player-package.* behind.
trap 'trash-put -- "$STAGE" 2>/dev/null || rm -rf -- "$STAGE"' EXIT

# CRLF for the text files, but only when they are not already all-CRLF. Line by
# line: one stray CR used to satisfy a whole-file check and leave the rest of
# the file LF-only, which Notepad shows as a single long line. The substitution
# replaces an optional trailing CR, so already-CRLF lines gain nothing.
crlf() {
    local f
    while IFS= read -r -d '' f; do
        if grep -qv $'\r$' "$f"; then
            sed 's/\r\?$/\r/' "$f" > "$f.crlf"
            mv -f "$f.crlf" "$f"
        fi
    done < <(find "$STAGE" -type f \( -name '*.ini' -o -name '*.md' \) -print0)
}

# stage_zip <zip name> <source> <in-zip path> [<source> <in-zip path>...]:
# a fresh staging copy laid out exactly as the entries should sit in the zip,
# CRLF for the text files, then the zip.
stage_zip() {
    local zip="dist/$1" tmpzip="$ROOT/dist/$1.tmp.$$"
    shift
    local rels=()
    rm -rf -- "$STAGE"
    mkdir -p "$STAGE"
    while [ $# -ge 2 ]; do
        mkdir -p "$STAGE/$(dirname "$2")"
        cp -f -- "$1" "$STAGE/$2"
        rels+=("$2")
        shift 2
    done
    crlf
    (cd "$STAGE" && zip -X -q "$tmpzip" "${rels[@]}")
    mv -f "$tmpzip" "$zip"
    echo
    echo "== $zip =="
    unzip -l "$zip"
}

stage_zip nioh4p-player.zip \
    build/dinput8.dll dinput8.dll \
    nioh3p.ini nioh3p.ini \
    package/README.md README.md
stage_zip nioh4p-plugin.zip \
    build/nioh4p.dll DLLMods/nioh4p.dll \
    nioh3p.ini nioh3p.ini \
    package/README.md README.md
for id in $ADDONS; do
    name="nioh4p-$id"
    stage_zip "$name.zip" \
        "build/addons/$name.dll" "DLLMods/$name.dll" \
        "package/addons/$name.ini" "DLLMods/$name.ini" \
        package/README.md README.md
done

# One zip with everything: the standalone core plus every add-on, laid out for
# extraction into the game folder. Group members who install this zip need no
# other package.
all_args=(build/dinput8.dll dinput8.dll
    nioh3p.ini nioh3p.ini
    package/README.md README.md)
for id in $ADDONS; do
    name="nioh4p-$id"
    all_args+=("build/addons/$name.dll" "DLLMods/$name.dll"
        "package/addons/$name.ini" "DLLMods/$name.ini")
done
stage_zip nioh4p-all.zip "${all_args[@]}"

echo
echo "== sha256 =="
sha256sum dist/nioh4p-*.zip "$ROOT/build/dinput8.dll" "$ROOT/build/nioh4p.dll"
