#!/usr/bin/env bash
# Denylist scan before publishing: personal identifiers must not reach the
# public line, in the tree or in its history. The denylist lives in this
# private-line file on purpose; do not copy it to the public line.
#
#   tools/publish-scan.sh [ref] [--history]
#
# Default ref is origin/master when it exists, else HEAD. --history scans
# every blob reachable from the ref, not just the tip tree. Exits non-zero
# on any hit outside the allowlisted contexts.
set -euo pipefail
cd "$(dirname "$0")/.."

ref=HEAD
history=0
for a in "$@"; do
    case "$a" in
    --history) history=1 ;;
    *) ref=$a ;;
    esac
done
if [ "$ref" = HEAD ] && git rev-parse -q --verify origin/master >/dev/null; then
    ref=origin/master
fi

DENY='kenja|a3delta|acad1988|/home/[a-z0-9]+|7656119[0-9]{10}|[A-Za-z]:\\\\(home|[Uu]sers)|[a-z0-9._%+-]+@[a-z0-9-]+\.[a-z]{2,}'
# 76561197960265728 is 2^56, Steam's universal ID base - a constant, not an ID.
ALLOW='Kenja-mc|support@nexusmods\.com|76561197960265728'

# Placeholder paths (examples and genericized lines) are not identifiers.
allow_fixed() {
    grep -ivF -e 'home\\<you>' -e 'home\\me' -e 'home\\user' \
              -e '/home/me' -e '/home/you' -e '/home/user'
}

fail=0
if git grep -inE "$DENY" "$ref" -- ':(exclude)tools/publish-scan.sh' 2>/dev/null \
        | grep -ivE "$ALLOW" | allow_fixed > /tmp/publish-scan.hits; then
    echo "tree hits in $ref:" >&2
    cat /tmp/publish-scan.hits >&2
    fail=1
fi
if [ "$history" = 1 ]; then
    git rev-list "$ref" --objects | awk '{print $1}' \
        | git cat-file --batch-check='%(objectname) %(objecttype)' \
        | awk '$2=="blob"{print $1}' \
    | while read -r b; do
        if git cat-file blob "$b" 2>/dev/null | grep -inE "$DENY" \
                | grep -ivE "$ALLOW" | allow_fixed > /tmp/publish-scan.blob; then
            echo "history blob $b:" >&2
            cat /tmp/publish-scan.blob >&2
            exit 1
        fi
    done || fail=1
fi
if [ "$fail" = 0 ]; then
    echo "publish-scan: $ref clean$([ $history = 1 ] && echo ' (tree + history)')"
else
    echo "publish-scan: fix the hits above on the private line, then re-port" >&2
fi
exit $fail
