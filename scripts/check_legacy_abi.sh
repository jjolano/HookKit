#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
binary="${1:?usage: check_legacy_abi.sh <HookKit binary|framework> [baseline-dir] [compare options...] }"
shift
baseline_dir="${1:-$root/Tests/LegacyABI/Baselines}"
if [[ $# -gt 0 ]]; then
    shift
fi

if [[ -d "$binary" ]]; then
    binary="$binary/HookKit"
fi

tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT

python3 "$root/Tools/abi/extract_abi.py" \
    --binary "$binary" \
    --tag candidate \
    --header "$root/Headers/HookKit.h" \
    --enum-header "$root/Headers/HookKit.h" \
    --repo-root "$root" \
    --out "$tmp_dir/candidate.json"

found=0
for baseline in "$baseline_dir"/*.json; do
    [[ -e "$baseline" ]] || continue
    found=1
    python3 "$root/Tools/abi/compare_abi.py" \
        --old "$baseline" \
        --new "$tmp_dir/candidate.json" \
        --required-header Headers/HookKit.h \
        "$@"
done

if [[ "$found" -eq 0 ]]; then
    echo "no ABI baselines found under $baseline_dir" >&2
    exit 1
fi
