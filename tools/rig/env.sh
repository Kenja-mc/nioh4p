# Shared environment for the headless test rig (tools/rig/).
#
# The rig runs extra Nioh instances with no window on the user's desktop and no
# use of the dGPU: Xvfb for Steam's UI, headless gamescope for the game, and
# separate Steam roots and game libraries per account. DXVK's device filter
# and gamescope's --prefer-vk-device select the iGPU. The user's own Steam
# root, library and Proton prefix are never written to.

RIG="${RIG:-$HOME/Projects/gaming/nioh-mp-mod-rig}"
XVFB_DISPLAY="${XVFB_DISPLAY:-:9}"
IGPU_ID="${IGPU_ID:-1002:13c0}"          # Radeon 610M iGPU; dGPU is 1002:7550
SRC_STEAM="${SRC_STEAM:-$HOME/.local/share/Steam}"
SRC_LIB="$SRC_STEAM/steamapps"
ACCOUNT_A="${ACCOUNT_A:-}"             # slot a account, supplied locally
ACCOUNT_B="${ACCOUNT_B:-}"             # slot b account, supplied locally
export RIG XVFB_DISPLAY IGPU_ID SRC_STEAM SRC_LIB ACCOUNT_A ACCOUNT_B

slot_root() { printf '%s/home-%s/.local/share/Steam' "$RIG" "$1"; }
slot_display() {                      # one Xvfb per slot: screens never overlap
    case "$1" in a) printf ':9' ;; b) printf ':10' ;; *) printf '%s' "$XVFB_DISPLAY" ;; esac
}
slot_lib()  { printf '%s/lib-%s/steamapps' "$RIG" "$1"; }
slot_log()  { printf '%s/logs-%s' "$RIG" "$1"; }

# Everything a rig process needs.  Call with the slot name (a|b).
rig_env() {
    HOME="$RIG/home-$1"
    XDG_RUNTIME_DIR="$RIG/run-$1"
    XDG_CONFIG_HOME="$HOME/.config"
    XDG_DATA_HOME="$HOME/.local/share"
    DISPLAY="$(slot_display "$1")"
    MESA_VK_DEVICE_SELECT="$IGPU_ID!"           # RADV device selection
    DXVK_FILTER_DEVICE_NAME="RAPHAEL_MENDOCINO" # and DXVK's own filter
    VK_LOADER_LAYERS_DISABLE='~implicit~'   # no lsfg/mangohud hooks in the rig
    export HOME XDG_RUNTIME_DIR XDG_CONFIG_HOME XDG_DATA_HOME \
           DISPLAY MESA_VK_DEVICE_SELECT DXVK_FILTER_DEVICE_NAME \
           VK_LOADER_LAYERS_DISABLE
    mkdir -p "$HOME"
    mkdir -p "$XDG_RUNTIME_DIR"
    chmod 700 "$XDG_RUNTIME_DIR"
}

# A rig display, started on demand.  usage: rig_display [:9]
rig_display() {
    d="${1:-$XVFB_DISPLAY}"
    pgrep -f "Xvfb $d " >/dev/null && return 0
    Xvfb "$d" -screen 0 1280x720x24 -nolisten tcp \
        >"$RIG/xvfb$(echo "$d" | tr -d ':').log" 2>&1 &
    sleep 2
    pgrep -f "Xvfb $d " >/dev/null
}
