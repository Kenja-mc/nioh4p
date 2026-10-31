#!/bin/sh
# Build (or refresh) the headless test rig.
#
# Per slot (a = modded, b = stock host by default):
#   $RIG/home-<slot>/.local/share/Steam        Steam root: COW clone of the
#       steamapps/common/<game, Proton, runtimes>   client files AND of the
#       steamapps/compatdata/485510                 game itself, in the root
#       config/config.vdf                           library (see NOTE 1)
#   $RIG/logs-<slot>                           mod logs (nioh3p.ini out_dir)
#
# Everything is a btrfs reflink clone: full isolation, no shared inodes, no
# measurable disk use.  Re-running is cheap and leaves existing clones alone.
#
# Hard-won details this script encodes (each one cost a debugging session):
#   NOTE 1  A library entry other than "0" is only honoured if the library
#           folder carries its own steamapps/libraryfolder.vdf marker; entry
#           "0" is the root library and its path is ignored.  Steam also
#           rewrites libraryfolders.vdf on startup.  Cloning into the root
#           library sidesteps all of it (and the app is then adopted with
#           "Fully Installed" and no download).
#   NOTE 2  Steam keys userdata/ by accountid, while loginusers.vdf is keyed
#           by SteamID64. Resolve the accountid from the selected login record.
#   NOTE 3  The per-game compat tool mapping lives in config/config.vdf
#           ("Steam" -> "CompatToolMapping"), not in userdata.  Proton 11.0's
#           own tool prefix fails to build here (missing amdxcffx64.dll), so
#           the rig pins proton_experimental, which works.
#   NOTE 4  The game must be started with its own directory as the working
#           directory (it resolves data files relative to CWD) and needs a
#           config.xml (SCR_REZ/DISP_MODE/REN_REZ); a fresh prefix has none.
#   NOTE 5  Proton refuses to build a prefix unless compatdata/<appid> already
#           exists (it tries to create compatdata/<appid>/pfx.lock).
set -eu
. "$(dirname "$0")/env.sh"

say() { printf '== %s\n' "$*"; }

[ -d "$SRC_LIB/common/Nioh" ] || { echo "no Nioh install in $SRC_LIB" >&2; exit 1; }
[ -n "$ACCOUNT_A" ] && [ -n "$ACCOUNT_B" ] ||
    { echo "setup: set ACCOUNT_A and ACCOUNT_B locally" >&2; exit 1; }

mkdir -p "$RIG"/shots
rig_display || { echo "cannot start Xvfb $XVFB_DISPLAY" >&2; exit 1; }

clone() {       # clone <src> <dst>  - never shares an inode
    [ -e "$2" ] && return 0
    cp -a --reflink=always "$1" "$2"
}

to_trash() {    # removals go to the desktop trash, never to the void
    for f in "$@"; do
        [ -e "$f" ] || continue
        if command -v trash >/dev/null 2>&1; then trash "$f"; else trash-put "$f"; fi
    done
}

for slot in a b; do
    root="$(slot_root "$slot")"
    sa="$root/steamapps"
    say "slot $slot"

    # --- Steam root: client files, minus the library (NOTE 1) ---
    if [ ! -x "$root/steam.sh" ]; then
        mkdir -p "$root"
        for f in "$SRC_STEAM"/*; do
            base=$(basename "$f")
            case "$base" in steamapps|logs) continue;; esac
            clone "$f" "$root/$base"
        done
    fi
    mkdir -p "$sa/common" "$sa/compatdata/485510" "$sa/downloading" "$sa/temp"

    # --- the game, Proton, the container runtimes and their manifests, in the
    #     root library where Steam always looks ---
    for d in "$SRC_LIB"/common/*; do
        name=$(basename "$d")
        case "$name" in
            Nioh|Proton*|SteamLinuxRuntime*|"Steamworks Shared") : ;;
            *) continue ;;
        esac
        clone "$d" "$sa/common/$name"
        for m in "$SRC_LIB"/appmanifest_*.acf; do
            [ -e "$m" ] || continue
            if grep -q "\"installdir\"[[:space:]]*\"$name\"" "$m"; then
                clone "$m" "$sa/$(basename "$m")"
                break
            fi
        done
    done
    if [ -d "$SRC_STEAM/compatibilitytools.d" ]; then
        mkdir -p "$root/compatibilitytools.d"
        for d in "$SRC_STEAM/compatibilitytools.d"/*; do
            [ -e "$d" ] || continue
            clone "$d" "$root/compatibilitytools.d/$(basename "$d")"
        done
    fi

    # --- login (NOTE 2) and the compat tool (NOTE 3) ---
    account="$ACCOUNT_A"; [ "$slot" = b ] && account="$ACCOUNT_B"
    python3 - "$root" "$account" <<'PY'
import os, re, shutil, sys
root, account = sys.argv[1], sys.argv[2]

# Resolve the slot's account from the clone's login records. Never guess a
# userdata directory: an ambiguous or absent name must fail before any writes.
login_path = os.path.join(root, "config", "loginusers.vdf")
with open(login_path) as f:
    login_text = f.read()
records = list(re.finditer(r'"(\d{17})"\s*\{([^{}]*)\}', login_text))
matches = []
for m in records:
    name = re.search(r'"AccountName"\s*"([^"]*)"', m.group(2))
    if name and name.group(1) == account:
        matches.append(int(m.group(1)) - 76561197960265728)
if len(matches) != 1 or not 0 < matches[0] <= 0xffffffff:
    sys.exit("setup: selected account missing or ambiguous in cloned login records")
account_id = str(matches[0])
localconfig_path = os.path.join(root, "userdata", account_id, "config", "localconfig.vdf")
if not os.path.exists(localconfig_path):
    sys.exit("setup: selected account has no cloned userdata configuration")


def load(p):
    with open(p) as f:
        return f.read()


def save_atomic(p, s):
    # Steam reads these vdf files at startup, so a half-written file costs the
    # clone its login.  Keep the pristine clone copy once, then replace by
    # rename: the file is either the old or the new one, never in between.
    bak = p + ".nioh3p-bak"
    if not os.path.exists(bak):
        shutil.copy2(p, bak)
    tmp = p + ".nioh3p-tmp"
    with open(tmp, "w") as f:
        f.write(s)
    os.replace(tmp, p)

# Keep the account picker used by login.sh: disable automatic login for every
# saved account, regardless of whitespace in its record. Preserve remembered
# credentials and the existing account ordering rather than selecting a user.
def disable_auto_login(m):
    block = m.group(0)
    block, count = re.subn(r'("AllowAutoLogin"\s*)"[^"]*"',
                           lambda x: x.group(1) + '"0"', block)
    if count != 1:
        sys.exit("setup: login record missing or has duplicate AllowAutoLogin")
    return block


s = re.sub(r'"\d{17}"\s*\{[^{}]*\}', disable_auto_login, login_text)
save_atomic(login_path, s)

# Compat tool: the per-user mapping, where Steam itself keeps it.
# config/config.vdf is deliberately NOT edited: a bad regex in it stops the
# client's window from ever mapping (learned the hard way).
p = localconfig_path
s = load(p)
entry = ('\t\t\t\t\t"485510"\n\t\t\t\t\t{\n\t\t\t\t\t\t"name"\t\t"proton_experimental"\n'
         '\t\t\t\t\t\t"config"\t\t""\n\t\t\t\t\t\t"Priority"\t\t"250"\n\t\t\t\t\t}\n')
if '"CompatToolMapping"' not in s:
    s = s.replace('"UserLocalConfigStore"\n{',
                  '"UserLocalConfigStore"\n{\n\t\t\t\t"CompatToolMapping"\n\t\t\t\t{\n\t\t\t\t}\n',
                  1)
s = re.sub(r'"485510"\s*\n\s*\{[^{}]*\}', entry.strip(), s)
if not re.search(r'"CompatToolMapping"\s*\n\s*\{[^}]*"485510"', s, re.S):
    s = re.sub(r'("CompatToolMapping"\s*\n\s*\{)',
               lambda m: m.group(1) + '\n' + entry, s, count=1)
save_atomic(p, s)

print("   login configured, compat tool proton_experimental")
PY

    # --- the game only runs from its own directory, and wants an appid hint ---
    printf '485510' > "$sa/common/Nioh/steam_appid.txt"
    mkdir -p "$(slot_log "$slot")"
done

# --- the mod goes into slot a only; slot b stays stock ---
say "installing the mod into slot a"
A_LIB="$(slot_root a)/steamapps/common/Nioh"
cp -a build/dinput8.dll "$A_LIB/dinput8.dll"
# nioh3p.sites.txt is generated, not installed (see install.sh): the mod runs
# without it and only trace=1 loses the call-site names.
if [ -f nioh3p.sites.txt ]; then
    cp -a nioh3p.sites.txt "$A_LIB/nioh3p.sites.txt"
else
    say "no nioh3p.sites.txt to install - generated, not shipped (install.sh:11-14); run tools/sites.py > nioh3p.sites.txt for trace=1"
fi
# the ini is read by the game under Wine, so out_dir has to be the Z: path of
# this rig's log directory, wherever RIG points
out_dir="Z:$(printf '%s' "$(slot_log a)" | sed 's|/|\\|g')"
cat > "$A_LIB/nioh3p.ini" <<EOF
out_dir=$out_dir

max_players=4
dump_image=0
redump_after_s=0
dump_every_s=0
hook_eos=1
drop_lock_filter=1
drop_slots_filter=1
force_unlock=0
EOF
B_LIB="$(slot_root b)/steamapps/common/Nioh"
to_trash "$B_LIB/dinput8.dll" "$B_LIB/nioh3p.ini"

# --- video settings for a fresh prefix (NOTE 4): 720p windowed, low detail ---
seed_config() {
    slot="$1"
    docs="$(slot_root "$slot")/steamapps/compatdata/485510/pfx/drive_c/users/steamuser/Documents/KoeiTecmo/NIOH"
    mkdir -p "$docs"
    cat > "$docs/config.xml" <<'EOF'
<?xml version="1.0" encoding="utf-8" ?>
<settings>
    <CONFIG_TYPE>CUSTOM</CONFIG_TYPE>
    <DISP_MODE>WINDOW</DISP_MODE>
    <REN_REZ>LOW</REN_REZ>
    <SCR_REZ>1280x720</SCR_REZ>
    <FPS>60</FPS>
    <SHADOW_QUAL>OFF</SHADOW_QUAL>
    <AO>OFF</AO>
    <RT_REF>OFF</RT_REF>
    <MOT_BLUR>OFF</MOT_BLUR>
</settings>
EOF
    [ -f "$HOME/Documents/KoeiTecmo/NIOH/nioh.ini" ] && \
        cp -a "$HOME/Documents/KoeiTecmo/NIOH/nioh.ini" "$docs/nioh.ini"
}
seed_config a
seed_config b

say "done"
df -h "$RIG" | tail -1
echo "-- apparent size (real cost is far lower: all reflinks)"
du -sh --apparent-size "$RIG" 2>/dev/null | tail -1
echo "-- real disk cost (exclusive bytes)"
btrfs filesystem du -s "$RIG" 2>/dev/null | tail -1
