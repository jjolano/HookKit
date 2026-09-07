#!/usr/bin/env bash
# Standalone source-linked probe only: no framework deployment or global writes.
set -euo pipefail
obj=${1:?usage: bash tools/run-native-coverage.sh BUILD_OBJECT_DIR [mobile@host] [MODE ...]}
host=${2:-mobile@10.0.1.160}
modes=("${@:3}")
if [[ ${#modes[@]} == 0 ]]; then
    modes=(baseline exhaust-fallback exhaust-static fail-alloc fail-seal fail-write abandoned-reuse stale-cleanup concurrent-distinct concurrent-same-page)
fi
ssh_command=(ssh -o StrictHostKeyChecking=accept-new -o IdentitiesOnly=yes)
if [[ -n ${SSHPASS:-} ]]; then
    ssh_command=(sshpass -e "${ssh_command[@]}" -o PreferredAuthentications=password -o PubkeyAuthentication=no)
fi
remote() { "${ssh_command[@]}" "$host" "$@"; }
binary=device_static_smoke
test -f "$obj/$binary"
local_hash=$(sha256sum "$obj/$binary")
printf 'SOURCE_LINKED_PROBE sha256=%s framework-under-test=none\n' "${local_hash%% *}"
remote 'ls -ld /var/jb/tmp'
directory=$(remote 'mktemp -d /var/jb/tmp/hookkit-native.XXXXXX')
[[ $directory == /var/jb/tmp/hookkit-native.* && $directory != *[!a-zA-Z0-9/.-]* ]]
printf 'REMOTE_DIRECTORY=%s\n' "$directory"
cleanup() {
    remote "rm -rf '$directory' && test ! -e '$directory'" || return 1
    printf 'CLEANUP removed=%s\n' "$directory"
}
trap 'status=$?; trap - EXIT; cleanup || { printf "CLEANUP FAILED directory=%s; remove when connectivity returns\n" "$directory" >&2; status=1; }; exit "$status"' EXIT
# Fresh vnodes, never overwrite a previously executed Mach-O.
tar -C "$obj" -cf - "$binary" | remote "tar -xf - -C '$directory'"
remote_hash=$(remote "sha256sum '$directory/$binary'")
[[ ${local_hash%% *} == "${remote_hash%% *}" ]]
for run in 1 2 3 4 5; do
    for mode in "${modes[@]}"; do
        [[ $mode != *[!a-z-]* && -n $mode ]] || exit 2
        printf 'RUN repeat=%s mode=%s\n' "$run" "$mode"
        status=0
        remote "cd '$directory' && ulimit -c 0 && exec './$binary' '$mode'" || status=$?
        printf 'EXIT repeat=%s mode=%s status=%s\n' "$run" "$mode" "$status"
        # No skip-to-pass conversion: real native refusal is a failed gate.
        [[ $status == 0 ]] || exit "$status"
    done
done
