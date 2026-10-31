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
#   run.sh d lan      same for slot d, with compat-d
#   run.sh a probe peek.exe 4 '*0x1418715e0+0x5d0'
#                     run a read-only probe from $RIG/tools-tmp inside the
#                     running lan game's prefix (Wine shares its wineserver)
#
# The rig client runs from its own HOME/root, so the user's foreground Steam
# client is never touched.
set -eu
. "$(dirname "$0")/env.sh"

APPID=485510

slot="${1:?usage: run.sh <a|b|c|d> [client|login|game|direct|lan|probe EXE ARGS]}"
what="${2:-client}"
client_slot="$slot"
case "$slot" in
    a|b) ;;
    c|d)
        case "$what" in lan|probe) ;; *) echo "slot $slot supports lan and probe only" >&2; exit 2 ;; esac
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
        exec "$root/ubuntu12_32/steam" -applaunch "$APPID"
        ;;
    lan|probe)
        game_dir="$RIG/lan-$slot"
        prefix="$root/steamapps/compatdata/$APPID"
        case "$slot" in c|d) prefix="$RIG/compat-$slot" ;; esac
        [ -f "$game_dir/nioh.exe" ] && [ -d "$prefix/pfx" ] || {
            echo "slot $slot needs a prepared LAN copy and prefix" >&2; exit 1;
        }
        launcher="$(realpath "$(dirname "$0")/../quiet-launch.sh")"
        export STEAM_COMPAT_CLIENT_INSTALL_PATH="$root"
        export STEAM_COMPAT_DATA_PATH="$prefix"
        export SteamAppId="$APPID" SteamGameId="$APPID" PROTON_LOG="${PROTON_LOG:-1}"
        export NIOH3P_HEADLESS=1 NIOH3P_SCOPE=0
        # the game must stay inaudible (standing rule): the slot's runtime dir
        # has no pulse socket, so wine falls back to raw ALSA, whose pcm device
        # reaches the desktop speakers. Both audio paths die here.
        export PULSE_SERVER="${PULSE_SERVER:-127.0.0.1:1}" ALSA_CONFIG_PATH=/dev/null
        cd "$game_dir"
        if [ "$what" = probe ]; then
            shift 2
            exe="${1:?usage: run.sh <slot> probe <exe> [args]}"; shift
            # probes are read-only tools staged in $RIG/tools-tmp: resolve the
            # name and refuse anything that lands outside (../bin/sh, or a
            # symlink leaving the directory, would run an arbitrary binary in
            # the game's prefix).  $RIG is resolved too: it may itself sit
            # behind a symlink, which would otherwise reject every honest name.
            probe_dir="$(realpath -m -- "$RIG/tools-tmp")"
            probe="$(realpath -m -- "$probe_dir/$exe")"
            case "$probe" in
                "$probe_dir"/*) ;;
                *) echo "usage: run.sh <slot> probe <exe> [args] (exe must be inside $probe_dir)" >&2; exit 2 ;;
            esac
            exec "$root/steamapps/common/Proton - Experimental/proton" runinprefix "$probe" "$@"
        fi
        exec "$launcher" "$root/steamapps/common/Proton - Experimental/proton" run "$game_dir/nioh.exe"
        ;;
    direct)
        # Nioh resolves its data files relative to the working directory (its
        # boot log prints "Boot Path:"), so it is started from its own directory
        export STEAM_COMPAT_CLIENT_INSTALL_PATH="$root"
        export STEAM_COMPAT_DATA_PATH="$root/steamapps/compatdata/$APPID"
        export SteamAppId="$APPID" SteamGameId="$APPID"
        printf '%s' "$APPID" > "$game_dir/steam_appid.txt"
        cd "$game_dir"
        # same standing rule as the lan path: no audio path may reach the desk
        export PULSE_SERVER="${PULSE_SERVER:-127.0.0.1:1}" ALSA_CONFIG_PATH=/dev/null
        exec "$root/steamapps/common/Proton - Experimental/proton" run "$game_dir/nioh.exe"
        ;;
esac


# the client must stay inaudible: its HOME is isolated, but libpulse would
# still find the desktop server through the inherited runtime dir. Point it
# at a dead local address so every sound sink fails (standing rig rule).
export PULSE_SERVER="${PULSE_SERVER:-127.0.0.1:1}"
# the rig UI can render in software: nothing it draws needs the GPU
LIBGL_ALWAYS_SOFTWARE=1 exec "$root/steam.sh" -silent -cef-disable-gpu
