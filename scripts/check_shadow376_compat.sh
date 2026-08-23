#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
shadow_dir="${1:-$root/../shadow}"
shadow_tag=v3.7.6
abi_tag=v2.1.1
baseline="$root/Tests/LegacyABI/Baselines/$abi_tag.json"

fail() {
    printf 'FAIL: %s\n' "$1" >&2
    exit 1
}

git -C "$shadow_dir" rev-parse --is-inside-work-tree >/dev/null 2>&1 ||
    fail "Shadow checkout not found: $shadow_dir"
[[ -f "$baseline" ]] || fail "missing HookKit ABI baseline: $baseline"

shadow_pin="$(git -C "$shadow_dir" rev-parse "$shadow_tag:vendor/HookKit.framework" 2>/dev/null)" ||
    fail "Shadow tag $shadow_tag has no HookKit submodule pin"
abi_commit="$(git -C "$root" rev-parse "$abi_tag^{commit}" 2>/dev/null)" ||
    fail "HookKit tag $abi_tag is unavailable"
git -C "$root" cat-file -e "$shadow_pin^{commit}" 2>/dev/null ||
    fail "Shadow pin $shadow_pin is unavailable in this HookKit checkout"
git -C "$root" merge-base --is-ancestor "$shadow_pin" "$abi_commit" ||
    fail "Shadow pin $shadow_pin is not covered by $abi_tag"

require_shadow() {
    git -C "$shadow_dir" grep -Fq "$1" "$shadow_tag" -- Shadow.dylib ||
        fail "Shadow $shadow_tag no longer uses expected facade marker: $1"
}

require_header() {
    grep -Fq "$1" "$root/Headers/HookKit.h" ||
        fail "HookKit facade no longer exposes: $1"
}

for marker in \
    '#import <HookKit.h>' \
    'getAvailableSubstitutorTypes' \
    'getSubstitutorTypeInfo:' \
    'defaultSubstitutor' \
    'setTypes:' \
    'initLibraries' \
    'HKEnableBatching()' \
    'HKExecuteBatch()' \
    '#define MSHookFunction' \
    'HKHookMessage' \
    'HKOpenImage' \
    'HKFindSymbol' \
    'HKCloseImage'; do
    require_shadow "$marker"
done

for marker in \
    'getAvailableSubstitutorTypes' \
    'getSubstitutorTypeInfo:' \
    'defaultSubstitutor' \
    'hookkit_lib_t types;' \
    'initLibraries' \
    'hookFunction:' \
    'HKEnableBatching' \
    'HKExecuteBatch' \
    'HKHookMessage' \
    'HKOpenImage' \
    'HKFindSymbol' \
    'HKCloseImage'; do
    require_header "$marker"
done

printf 'PASS: Shadow %s pin %s is covered by HookKit %s ABI and uses the retained facade surface\n' \
    "$shadow_tag" "$shadow_pin" "$abi_tag"
