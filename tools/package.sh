#!/usr/bin/env bash
# Builds the player package: dist/nioh3p-player.zip
#
# The zip holds exactly three files at its root:
#   dinput8.dll    the mod (built by ./build.sh, proxy only)
#   nioh3p.ini     the shipped player configuration (stock search filters)
#   README.md      the player-facing instructions (from package/README.md)
#
# The two text files are converted to CRLF line endings in the zip so that
# Windows Notepad shows them properly. Nothing outside build/ is removed;
# the staging directory goes to the trash afterwards (deleted if trash-put
# is not installed).
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT=$PWD

./build.sh

[ -f build/dinput8.dll ] || { echo "build/dinput8.dll missing" >&2; exit 1; }
[ -f nioh3p.ini ] || { echo "nioh3p.ini missing" >&2; exit 1; }
[ -f package/README.md ] || { echo "package/README.md missing" >&2; exit 1; }

mkdir -p build dist
STAGE=$(mktemp -d build/player-package.XXXXXX)
# Trash when available, delete otherwise: the staging copy is disposable, and a
# missing trash-put used to leave build/player-package.* behind.
trap 'trash-put -- "$STAGE" 2>/dev/null || rm -rf -- "$STAGE"' EXIT

cp -f build/dinput8.dll "$STAGE/dinput8.dll"
cp -f nioh3p.ini      "$STAGE/nioh3p.ini"
cp -f package/README.md "$STAGE/README.md"

# CRLF for the text files, but only when they are not already all-CRLF. Line by
# line: one stray CR used to satisfy a whole-file check and leave the rest of
# the file LF-only, which Notepad shows as a single long line. The substitution
# replaces an optional trailing CR, so already-CRLF lines gain nothing.
for f in "$STAGE/nioh3p.ini" "$STAGE/README.md"; do
    if grep -qv $'\r$' "$f"; then
        sed 's/\r\?$/\r/' "$f" > "$f.crlf"
        mv -f "$f.crlf" "$f"
    fi
done

ZIP=dist/nioh3p-player.zip
TMPZIP="$ZIP.tmp.$$"
zip -X -j -q "$TMPZIP" \
    "$STAGE/dinput8.dll" "$STAGE/nioh3p.ini" "$STAGE/README.md"
mv -f "$TMPZIP" "$ZIP"

echo
echo "== $ZIP =="
unzip -l "$ZIP"
echo
sha256sum "$ZIP" "$ROOT/build/dinput8.dll"
