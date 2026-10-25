#!/usr/bin/env bash
# Runs Nioh through Steam and switches the game folder between the mod and stock.
#
#   ./play.sh           start Nioh through Steam, whatever is installed
#   ./play.sh status    what the game folder holds, and the launch options
#   ./play.sh stock     remove the mod so the game runs unmodified again
#   ./play.sh mod       install the built mod again
#
# The mod is files next to nioh.exe: dinput8.dll and nioh3p.ini, plus
# nioh3p.sites.txt for trace=1. "stock" sends those, their backups and the mod
# logs to the trash, then reports what is left. It never touches the in-game
# settings in the Proton prefix or the Steam launch options: those are yours,
# and this script only reports them.
set -euo pipefail

cd "$(dirname "$0")"

APPID=485510
GAME="${NIOH_DIR:-$HOME/.local/share/Steam/steamapps/common/Nioh}"
USERDATA="${STEAM_USERDATA:-$HOME/.local/share/Steam/userdata}"
removed=()

die() { echo "$*" >&2; exit 1; }

[ -d "$GAME" ] || die "game directory not found: $GAME (set NIOH_DIR)"

# The proxy carries this marker; a dinput8.dll without it is somebody else's.
is_ours() { [ -f "$1" ] && LC_ALL=C grep -qa "nioh3p proxy" "$1"; }

game_running() {
    pgrep -x nioh.exe >/dev/null 2>&1 ||
        pgrep -f -- "nioh[.]exe" >/dev/null 2>&1
}

put_away() {
    local f=$1
    [ -e "$f" ] || return 0
    if command -v trash-put >/dev/null 2>&1; then
        trash-put -- "$f"
    else
        mkdir -p "$GAME/.nioh3p-removed"
        mv -f -- "$f" "$GAME/.nioh3p-removed/"
    fi
    removed+=("$(basename "$f")")
}

# LaunchOptions of one appid, one per line. A VDF file is brace blocks, so walk
# the appid's block instead of matching the first "LaunchOptions" in the file.
launch_options() {
    python3 - "$1" "$APPID" <<'PY'
import re, sys

path, appid = sys.argv[1], sys.argv[2]
text = open(path, encoding="utf-8", errors="replace").read()
pattern = re.compile(r'"%s"\s*\{' % re.escape(appid))
pos, out = 0, []
while True:
    m = pattern.search(text, pos)
    if not m:
        break
    start, depth, i = m.end() - 1, 0, m.end() - 1
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                break
        i += 1
    block = text[start:i]
    for lo in re.finditer(r'"LaunchOptions"\s*"((?:[^"\\]|\\.)*)"', block):
        out.append(lo.group(1))
    pos = i
print("\n".join(out))
PY
}

show_launch_options() {
    local found=0 f account option
    [ -d "$USERDATA" ] || { echo "launch options: no Steam userdata at $USERDATA"; return; }
    for f in "$USERDATA"/*/config/localconfig.vdf; do
        [ -f "$f" ] || continue
        account=$(basename "$(dirname "$(dirname "$f")")")
        while IFS= read -r option; do
            [ -n "$option" ] || continue
            found=1
            echo "launch options ($account): $option"
            case $option in
            *quiet-launch* | *nioh3p*)
                echo "  ^ this is a test wrapper; clear it in Steam > Nioh > Properties > Launch Options"
                ;;
            esac
        done < <(launch_options "$f")
    done
    [ "$found" = 1 ] || echo "launch options: none for appid $APPID (Steam's default)"
}

show_state() {
    local dll="$GAME/dinput8.dll" ini="$GAME/nioh3p.ini"
    if is_ours "$dll"; then
        echo "state   : mod installed (dinput8.dll $(sha256sum "$dll" | cut -c1-8))"
    elif [ -e "$dll" ]; then
        echo "state   : foreign dinput8.dll, not nioh3p"
    else
        echo "state   : stock"
    fi
    [ -f "$ini" ] && echo "ini     : $ini"
    if game_running; then echo "game    : running"; else echo "game    : not running"; fi
    local logs=("$GAME"/nioh3p.log "$GAME"/nioh3p.prev.log)
    local line=""
    for f in "${logs[@]}"; do
        [ -f "$f" ] && line+="$(basename "$f") "
    done
    [ -n "$line" ] && echo "logs    : ${line% }"
    show_launch_options
}

case "${1:-play}" in
play)
    command -v steam >/dev/null 2>&1 || die "steam is not in PATH"
    if is_ours "$GAME/dinput8.dll"; then
        echo "note: the mod is installed; run ./play.sh stock for an unmodified game"
    fi
    [ -f "$GAME/nioh.exe" ] || die "nioh.exe not found in $GAME"
    exec steam -applaunch "$APPID"
    ;;
status)
    echo "game    : $GAME"
    show_state
    ;;
stock)
    if game_running; then
        die "Nioh is running. Close it first: a running game keeps the mod loaded."
    fi
    if [ -e "$GAME/dinput8.dll" ] && ! is_ours "$GAME/dinput8.dll"; then
        die "$GAME/dinput8.dll is not the nioh3p proxy. Leaving it alone."
    fi
    # The three install files go through install.sh, which owns that list.
    ./install.sh uninstall
    shopt -s nullglob
    for f in "$GAME"/nioh3p.ini.bak* "$GAME"/nioh3p.log "$GAME"/nioh3p.prev.log \
        "$GAME"/nioh3p.invite.txt "$GAME"/nioh3p.invite.used.txt "$GAME"/.dinput8.dll.new; do
        put_away "$f"
    done
    shopt -u nullglob
    if [ "${#removed[@]}" -gt 0 ]; then
        printf 'trashed : %s\n' "${removed[*]}"
    fi
    echo "state   : stock; the next launch is the unmodified game"
    show_launch_options
    echo "bring it back with ./play.sh mod"
    ;;
mod)
    exec ./install.sh
    ;;
*)
    sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
