#!/usr/bin/env bash
# Run only the owned-process probe. Never installs a framework globally.
set -euo pipefail
obj=${1:?usage: bash tools/run-observability.sh BUILD_OBJECT_DIR [mobile@host]}
host=${2:-mobile@10.0.1.160}
ssh_command=(ssh -o StrictHostKeyChecking=accept-new -o IdentitiesOnly=yes)
if [[ -n ${SSHPASS:-} ]]; then
    ssh_command=(sshpass -e "${ssh_command[@]}" -o PreferredAuthentications=password -o PubkeyAuthentication=no)
fi
remote() { "${ssh_command[@]}" "$host" "$@"; }
uuid() {
    "$THEOS/toolchain/linux/iphone/bin/otool" -l "$1" |
        awk '$1 == "uuid" { print tolower($2) }' | tr -d '-'
}
framework_uuid=$(uuid "$obj/HookKit.framework/HookKit")
probe_uuid=$(uuid "$obj/device_observability")
[[ $framework_uuid =~ ^[0-9a-f]{32}$ && $probe_uuid =~ ^[0-9a-f]{32}$ ]]
installed_before=$(remote 'sha256sum /var/jb/Library/Frameworks/HookKit.framework/HookKit')
remote 'ls -ld /var/jb/tmp'
directory=$(remote 'mktemp -d /var/jb/tmp/hookkit-observability.XXXXXX')
[[ $directory == /var/jb/tmp/hookkit-observability.* && $directory != *[!a-zA-Z0-9/.-]* ]]
printf 'REMOTE_DIRECTORY=%s\n' "$directory"
cleanup() {
    remote "rm -rf '$directory' && test ! -e '$directory'" || return 1
    local installed_after
    installed_after=$(remote 'sha256sum /var/jb/Library/Frameworks/HookKit.framework/HookKit') || return 1
    [[ $installed_before == "$installed_after" ]] || return 1
    printf 'CLEANUP removed=%s installed-framework-unchanged=%s\n' "$directory" "$installed_after"
}
trap 'status=$?; trap - EXIT; cleanup || { printf "CLEANUP FAILED directory=%s; verify/remove this directory when connectivity returns\n" "$directory" >&2; status=1; }; exit "$status"' EXIT
# Fresh directory/vnodes every run avoids cached code signatures on overwrite.
tar -C "$obj" -cf - HookKit.framework device_observability | remote "tar -xf - -C '$directory'"
for file in HookKit.framework/HookKit device_observability; do
    local_hash=$(sha256sum "$obj/$file")
    remote_hash=$(remote "sha256sum '$directory/$file'")
    [[ ${local_hash%% *} == "${remote_hash%% *}" ]]
    printf 'SHA256 %s %s\n' "${local_hash%% *}" "$file"
done
for run in 1 2 3 4 5; do
    for mode in memory objc inline static prepare-refusal; do
        printf 'RUN repeat=%s mode=%s\n' "$run" "$mode"
        status=0
        remote "HK_EXPECT_FRAMEWORK='$directory/HookKit.framework/HookKit' HK_EXPECT_FRAMEWORK_UUID='$framework_uuid' HK_EXPECT_PROBE='$directory/device_observability' HK_EXPECT_PROBE_UUID='$probe_uuid' '$directory/device_observability' '$mode'" || status=$?
        printf 'EXIT repeat=%s mode=%s status=%s\n' "$run" "$mode" "$status"
        if [[ $mode == prepare-refusal && $status != 0 ]]; then exit "$status"; fi
        [[ $status == 0 || $status == 77 ]] || exit "$status"
    done
done
