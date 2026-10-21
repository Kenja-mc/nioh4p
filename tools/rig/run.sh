#!/bin/sh
# usage: run.sh <slot> [client|login|game|direct|lan]
#
#   run.sh a client   start the rig Steam client (foreground; background it yourself)
#   run.sh a login    start the client if needed and pick its account tile
#   run.sh a game     request a launch through the live cloned Steam client
#   run.sh a direct   run the Steam-library copy directly under Proton
#   run.sh a lan      run the prepared lan-a copy in headless gamescope
#   run.sh c lan      use compat-c with slot b's cloned Proton/client files
#                     (the running native client stays on its private Xvfb)
#
# The rig client runs from its own HOME/root, so the user's foreground Steam
# client is never touched.
set -eu
. "$(dirname "$0")/env.sh"

slot="${1:?usage: run.sh <a|b|c> [client|login|game|direct|lan]}"
what="${2:-client}"
client_slot="$slot"
case "$slot" in
    a|b) ;;
    c)
        [ "$what" = lan ] || { echo "slot c supports lan only" >&2; exit 2; }
        client_slot=b
        ;;
    *) echo "unknown rig slot: $slot" >&2; exit 2 ;;
esac
root="$(slot_root "$client_slot")"
[ -x "$root/steam.sh" ] || { echo "slot $slot is not set up: run tools/rig/setup.sh" >&2; exit 1; }

rig_env "$slot"
# NOTE: deliberately no -master_ipc_name_override.  On Linux that flag is parsed
# as "host:port" by processpipe_posix.cpp and a bare name trips an assertion
# that segfaults the client; a separate HOME (its own .steampid/.steampath) is
# all the isolation this needs.
game_dir="$root/steamapps/common/Nioh"

case "$what" in
    login)
        exec "$(dirname "$0")/login.sh" "$slot"
        ;;
    game)
        # the client *binary* forwards the request to the running client;
        # steam.sh would start a second client instead and the request would
        # die with it
        exec "$root/ubuntu12_32/steam" -applaunch 485510
        ;;
    lan)
        game_dir="$RIG/lan-$slot"
        prefix="$root/steamapps/compatdata/485510"
        [ "$slot" != c ] || prefix="$RIG/compat-c"
        [ -f "$game_dir/nioh.exe" ] && [ -d "$prefix/pfx" ] || {
            echo "slot $slot needs a prepared LAN copy and prefix" >&2; exit 1;
        }
        launcher="$(realpath "$(dirname "$0")/../quiet-launch.sh")"
        export STEAM_COMPAT_CLIENT_INSTALL_PATH="$root"
        export STEAM_COMPAT_DATA_PATH="$prefix"
        export SteamAppId=485510 SteamGameId=485510 PROTON_LOG="${PROTON_LOG:-1}"
        export NIOH3P_HEADLESS=1 NIOH3P_SCOPE=0
        export NIOH3P_GPU=RAPHAEL_MENDOCINO NIOH3P_VK_DEVICE="$IGPU_ID"
        cd "$game_dir"
        exec "$launcher" "$root/steamapps/common/Proton - Experimental/proton" run "$game_dir/nioh.exe"
        ;;
    direct)
        # Nioh resolves its data files relative to the working directory (its
        # boot log prints "Boot Path:"), so it is started from its own directory
        export STEAM_COMPAT_CLIENT_INSTALL_PATH="$root"
        export STEAM_COMPAT_DATA_PATH="$root/steamapps/compatdata/485510"
        export SteamAppId=485510 SteamGameId=485510
        printf '485510' > "$game_dir/steam_appid.txt"
        cd "$game_dir"
        exec "$root/steamapps/common/Proton - Experimental/proton" run "$game_dir/nioh.exe"
        ;;
esac

# the rig UI can render in software: nothing it draws needs the GPU
LIBGL_ALWAYS_SOFTWARE=1 exec "$root/steam.sh" -silent -cef-disable-gpu
