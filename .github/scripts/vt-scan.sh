#!/usr/bin/env bash
# Upload a GitHub release's mod files to VirusTotal with the free public API
# and write a per-file verdict table to the job summary.
#
#   .github/scripts/vt-scan.sh <tag>
#
# Env: VT_API_KEY (required; VT_DRY_RUN=1 lists the files and exits),
#      GH_TOKEN + GITHUB_REPOSITORY to download the release assets.
# Public-API limits: 4 requests/min and 500/day, 32 MB per file
# (https://docs.virustotal.com/reference/public-vs-premium-api). Uploads and
# polls are spaced 20 s apart to stay under the per-minute rate.
set -euo pipefail

TAG="${1:?usage: vt-scan.sh <tag>}"
API="https://www.virustotal.com/api/v3"
DRY="${VT_DRY_RUN:-0}"

# Every release zip plus the DLLs inside them: engines verdict the binaries,
# and the zips are what players actually download.
WORK="$(mktemp -d)"
gh release download "$TAG" --repo "${GITHUB_REPOSITORY:?GITHUB_REPOSITORY not set}" --pattern '*.zip' --dir "$WORK/assets" --clobber

mkdir -p "$WORK/inner"
for z in "$WORK"/assets/*.zip; do
    name="$(basename "$z" .zip)"
    unzip -qo "$z" -d "$WORK/inner/$name"
done

# One entry per unique binary: the same DLL ships in several zips.
declare -A seen=()
files=()
while IFS= read -r f; do
    sha=$(sha256sum "$f" | awk '{print $1}')
    [ -n "${seen[$sha]:-}" ] && continue
    seen[$sha]=1
    files+=("$f")
done < <(printf '%s\n' "$WORK"/assets/*.zip; find "$WORK/inner" -type f -name '*.dll' | sort)

if [ "$DRY" = 1 ]; then
    for f in "${files[@]}"; do printf 'DRY %s %s\n' "$(sha256sum "$f" | cut -c1-16)" "$(basename "$f")"; done
    exit 0
fi
if [ -z "${VT_API_KEY:-}" ]; then
    echo "::warning::VT_API_KEY secret is not set; add a free VirusTotal key to run this scan"
    exit 0
fi
auth=(-H "x-apikey: $VT_API_KEY")

vt() { curl -s --fail-with-body --retry 3 --retry-delay 20 "$@"; }

results=()
first=1
for f in "${files[@]}"; do
    sha=$(sha256sum "$f" | awk '{print $1}')
    base=$(basename "$f")
    [ "$(stat -c%s "$f")" -lt 32000000 ] || { echo "::error::$base is over the 32 MB public-API limit"; exit 1; }
    # A file VT already knows returns its report directly; only upload once.
    if ! vt "$auth" -o "$WORK/r-$sha.json" "$API/files/$sha"; then
        [ "$first" = 1 ] || sleep 20
        first=0
        vt "$auth" -o "$WORK/up-$sha.json" -F "file=@$f" "$API/files"
        aid=$(jq -r '.data.id' "$WORK/up-$sha.json")
        # Poll the analysis; the verdicts land on the file object afterwards.
        for _ in $(seq 1 20); do
            sleep 20
            vt "$auth" -o "$WORK/an-$sha.json" "$API/analyses/$aid" || continue
            [ "$(jq -r '.data.attributes.status' "$WORK/an-$sha.json")" = completed ] && break
        done
        sleep 20
        vt "$auth" -o "$WORK/r-$sha.json" "$API/files/$sha" || { echo "::warning::no report for $base"; continue; }
    fi
    m=$(jq -r '.data.attributes.last_analysis_stats.malicious // "unknown"' "$WORK/r-$sha.json")
    s=$(jq -r '.data.attributes.last_analysis_stats.suspicious // "unknown"' "$WORK/r-$sha.json")
    u=$(jq -r '.data.attributes.last_analysis_stats.undetected // "unknown"' "$WORK/r-$sha.json")
    results+=("| $base | \`${sha:0:16}…\` | $m | $s | $u | [report](https://www.virustotal.com/gui/file/$sha) |")
done

{
    echo "## VirusTotal — release \`$TAG\`"
    echo
    echo "| file | sha256 | malicious | suspicious | undetected | report |"
    echo "|---|---|---|---|---|---|"
    printf '%s\n' "${results[@]}"
    echo
    echo "Free public API: counts come from the engines' generic heuristics; see docs/FP_REPORTS.md for the known false-positive context."
} | tee -a "$GITHUB_STEP_SUMMARY"
