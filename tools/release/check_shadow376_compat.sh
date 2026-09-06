#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
shadow_dir="${1:-$root/../shadow}"
shadow_tag=v3.7.6

# The facade's retained markers, verified two ways: Shadow v3.7.6 still uses
# them, and this tree's compatibility headers still expose them. The check is
# deliberately tag-agnostic about HookKit history: Shadow's pin (a40f515, a
# dangling 2023 commit outside every tag's ancestry) predates the tag lineage
# entirely, so an ancestor proof against any abi_tag can never pass. The
# markers below ARE the contract -- if the facade drops one, Shadow stops
# compiling, regardless of which commit it pins.

fail() {
    printf 'FAIL: %s\n' "$1" >&2
    exit 1
}

git -C "$shadow_dir" rev-parse --is-inside-work-tree >/dev/null 2>&1 ||
    fail "Shadow checkout not found: $shadow_dir"
shadow_pin="$(git -C "$shadow_dir" rev-parse "$shadow_tag:vendor/HookKit.framework" 2>/dev/null)" ||
    fail "Shadow tag $shadow_tag has no HookKit submodule pin"
# The pin must at least exist locally (fetch it first in CI); no ancestry is
# asserted -- see the header note for why an ancestor proof cannot pass.

require_shadow() {
    git -C "$shadow_dir" grep -Fq "$1" "$shadow_tag" -- Shadow.dylib ||
        fail "Shadow $shadow_tag no longer uses expected facade marker: $1"
}

require_header() {
    grep -Fq "$1" "$root/include/HookKit.h" ||
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

printf 'PASS: Shadow %s pin %s facade markers all present in this tree\n' \
    "$shadow_tag" "$shadow_pin"
