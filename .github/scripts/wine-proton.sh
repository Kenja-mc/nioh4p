#!/usr/bin/env bash
# Proton-shaped shim around plain wine, for CI where no Steam/Proton exists.
# Proton's calling convention is:  proton runinprefix <exe> [args...]
# WINE selects the loader. Ubuntu's wine64 package ships no /usr/bin/wine*;
# the real loader is /usr/lib/wine/wine64 (wineserver64 sits next to it).
set -euo pipefail
[ "$1" = runinprefix ] || { echo "wine-proton shim: unsupported subcommand '$1'" >&2; exit 2; }
shift
loader="${WINE:-}"
if [ -z "$loader" ]; then
    for c in /usr/lib/wine/wine64 wine64 wine; do
        command -v "$c" >/dev/null 2>&1 && { loader="$c"; break; }
    done
fi
[ -n "$loader" ] || { echo "wine-proton shim: no wine loader found" >&2; exit 127; }
exec "$loader" "$@"
