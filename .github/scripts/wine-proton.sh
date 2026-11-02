#!/usr/bin/env bash
# Proton-shaped shim around plain wine, for CI where no Steam/Proton exists.
# Proton's calling convention is:  proton runinprefix <exe> [args...]
# WINE selects the loader. Ubuntu's wine64 package ships no /usr/bin/wine*;
# the real loader is /usr/lib/wine/wine64 (wineserver64 sits next to it).
set -euo pipefail

find_loader() {
    local c
    if [ -n "${WINE:-}" ]; then printf '%s' "$WINE"; return 0; fi
    for c in /usr/lib/wine/wine64 wine64 wine; do
        if command -v "$c" >/dev/null 2>&1; then printf '%s' "$c"; return 0; fi
    done
    return 1
}

find_wineserver() {
    local c
    for c in /usr/lib/wine/wineserver64 wineserver64 wineserver; do
        if command -v "$c" >/dev/null 2>&1; then printf '%s' "$c"; return 0; fi
    done
    return 1
}

case "${1:-}" in
runinprefix)
    shift
    loader="$(find_loader)" || { echo "wine-proton shim: no wine loader found" >&2; exit 127; }
    exec "$loader" "$@"
    ;;
bootprefix)
    # Create or upgrade $WINEPREFIX. wine does not create missing parent
    # directories (the first wineboot fails with "chdir: No such file or
    # directory" and the fixture then boots the prefix mid-run), so the
    # directory must exist first. On a wine64-only install wineboot's wow64
    # probing can fail (no syswow64); that is harmless for the x64 fixtures,
    # so the boot itself stays best-effort and CI's canary checks the result.
    loader="$(find_loader)" || { echo "wine-proton shim: no wine loader found" >&2; exit 127; }
    mkdir -p "${WINEPREFIX:?WINEPREFIX must be set}" || exit 1
    "$loader" wineboot -i >/dev/null 2>&1 || true
    ;;
killprefix)
    # Stop this prefix's wineserver so the next fixture starts clean; a
    # lingering server wedges process creation in a reused prefix.
    ws="$(find_wineserver)" || exit 0
    "$ws" -k || true
    ;;
*)
    echo "wine-proton shim: unsupported subcommand '${1:-}'" >&2
    exit 2
    ;;
esac
