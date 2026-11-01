#!/usr/bin/env bash
# Proton-shaped shim around plain wine, for CI where no Steam/Proton exists.
# Proton's calling convention is:  proton runinprefix <exe> [args...]
set -euo pipefail
[ "$1" = runinprefix ] || { echo "wine-proton shim: unsupported subcommand '$1'" >&2; exit 2; }
shift
exec wine "$@"
