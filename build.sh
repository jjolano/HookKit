#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "$0")" && pwd)
cd "$ROOT"
: "${THEOS:?THEOS must point to Theos}"
PACKAGE_VERSION=3.0.0-1
MAKE_COMMAND=${MAKE_COMMAND:-make}
STAGE=$(mktemp -d "${TMPDIR:-/tmp}/hookkit-build.XXXXXX")
MAKE_ARGS=("THEOS_LIBRARY_PATH=$STAGE/lib")
RELEASE_DIR="$ROOT/.theos/release"
trap 'rm -rf "$STAGE"' EXIT

rm -rf "$RELEASE_DIR"
mkdir -p "$RELEASE_DIR"

run_make() {
    "$MAKE_COMMAND" "${MAKE_ARGS[@]}" "$@"
}

# Toolchain selection and validation for both arm64e ABIs live in
# $THEOS/bin/lane.sh, shared with Shadow so the two cannot disagree about which
# compiler a lane needs. On Linux the new-ABI lanes need a toolchain that stamps
# arm64e slices with the versioned ptrauth ABI (cpusubtype 0x80000002); theos's
# bundled clang emits 0x00000002, which modern ld rejects as "arm64e.old".
# $THEOS/bin/setup-modern-toolchain.sh assembles one that can.
. "$THEOS/bin/lane.sh"

require_modern_toolchain() { theos_abi_require new "$1"; }
require_oldabi_toolchain()  { theos_abi_require old rootful-legacy; }

# Lane make wrappers: on Linux the ABI's cross toolchain replaces theos's
# default, on macOS Xcode already emits the right ABI and needs no overrides.
modern_make() {
    local args; mapfile -t args < <(theos_abi_args new)
    "$MAKE_COMMAND" "${MAKE_ARGS[@]}" ${args[@]+"${args[@]}"} "$@"
}

legacy_make() {
    if [ "$(uname -s)" = Darwin ]; then
        DEVELOPER_DIR="$OLDABI_DEVELOPER_DIR" "$MAKE_COMMAND" "${MAKE_ARGS[@]}" "$@"
    else
        local args; mapfile -t args < <(theos_abi_args old)
        "$MAKE_COMMAND" "${MAKE_ARGS[@]}" ${args[@]+"${args[@]}"} "$@"
    fi
}

# Remove a field line from a control file. grep instead of `sed -i`, whose
# BSD form requires a backup-suffix argument (GNU accepts none) -- the
# mismatch is what broke the macOS CI lanes.
drop_field() {
    local file=$1 field=$2
    grep -v "^$field:" "$file" > "$file.tmp"
    mv "$file.tmp" "$file"
}

write_control() {
    local profile=$1 package name floor arch ceiling conflicts replaces provides
    ceiling=
    replaces=
    name='HookKit Framework'
    case "$profile" in
        rootful-legacy)
            package=me.jjolano.fmwk.hookkit.legacy
            name='HookKit Framework (Legacy)'
            floor=9.0
            arch=iphoneos-arm
            ceiling=', firmware (<< 14.0)'
            conflicts=', me.jjolano.fmwk.hookkit'
            replaces=', me.jjolano.fmwk.hookkit'
            provides=', me.jjolano.fmwk.hookkit'
            ;;
        rootful-modern)
            package=me.jjolano.fmwk.hookkit
            floor=14.0
            arch=iphoneos-arm
            conflicts=', me.jjolano.fmwk.hookkit.legacy'
            replaces=', me.jjolano.fmwk.hookkit.legacy'
            provides=
            ;;
        rootless)
            package=me.jjolano.fmwk.hookkit
            floor=15.0
            arch=iphoneos-arm64
            conflicts=', me.jjolano.fmwk.hookkit.legacy'
            replaces=', me.jjolano.fmwk.hookkit.legacy'
            provides=
            ;;
        roothide)
            package=me.jjolano.fmwk.hookkit
            floor=15.0
            arch=iphoneos-arm64e
            conflicts=', me.jjolano.fmwk.hookkit.legacy'
            replaces=', me.jjolano.fmwk.hookkit.legacy'
            provides=
            ;;
        *) echo "error: no control profile for $profile" >&2; return 1 ;;
    esac

    # The shared template carries the legacy provider fields. Empty profile
    # values mean the field must be absent, not silently inherited. Filtered
    # via grep, not `sed -i`: BSD sed's -i demands a suffix argument and ate
    # the script, breaking every macOS CI lane.
    sed -E \
        -e "s/^Package:.*/Package: $package/" \
        -e "s/^Name:.*/Name: $name/" \
        -e "s/firmware \(>= [^)]+\)/firmware (>= $floor)$ceiling/" \
        -e "s/^Architecture:.*/Architecture: $arch/" \
        -e "s/^(Conflicts:.*)/\\1$conflicts/" \
        -e "s/^(Replaces:.*)/\\1$replaces/" \
        -e "s/^(Provides:.*)/\\1$provides/" \
        -e "s/^Version:.*/Version: $PACKAGE_VERSION/" \
        packaging/layout/DEBIAN/control > "$RELEASE_DIR/control.$profile"

    if [ -z "$conflicts" ]; then drop_field "$RELEASE_DIR/control.$profile" Conflicts; fi
    if [ -z "$replaces" ]; then drop_field "$RELEASE_DIR/control.$profile" Replaces; fi
    if [ -z "$provides" ]; then drop_field "$RELEASE_DIR/control.$profile" Provides; fi
}

write_gum_control() {
    local profile=$1 arch floor
    case "$profile" in
        rootful-modern)
            arch=iphoneos-arm
            floor=14.0
            ;;
        rootless)
            arch=iphoneos-arm64
            floor=15.0
            ;;
        roothide)
            arch=iphoneos-arm64e
            floor=15.0
            ;;
        *) echo "error: no Gum package profile for $profile" >&2; return 1 ;;
    esac

    printf '%s\n' \
        'Package: me.jjolano.fmwk.hookkit.gum' \
        'Name: HookKit Frida Gum Provider' \
        "Depends: me.jjolano.fmwk.hookkit (= $PACKAGE_VERSION), firmware (>= $floor)" \
        "Version: $PACKAGE_VERSION" \
        "Architecture: $arch" \
        'Description: Optional Frida Gum provider for HookKit.' \
        'Maintainer: jjolano <jjolano@me.com>' \
        'Author: jjolano <jjolano@me.com>' \
        'Section: Development' \
        'Tag: role::developer' > "$RELEASE_DIR/control.$profile.gum"
}

copy_release_artifact() {
    local artifact=$1 name=$2
    cp -p "$artifact" "$RELEASE_DIR/$name.deb"
}

build_rootful_legacy() {
    local lane=rootful-legacy artifact
    require_oldabi_toolchain
    write_control "$lane"
    legacy_make HOOKKIT_LANE="$lane" clean
    [ -z "${HOOKKIT_SKIP_LANE_TEST:-}" ] && legacy_make HOOKKIT_LANE="$lane" test
    legacy_make HOOKKIT_LANE="$lane" package FINALPACKAGE=1 _THEOS_DEB_PACKAGE_CONTROL_PATH="$RELEASE_DIR/control.$lane"
    artifact=$(cat .theos/last_package)
    run_make check-compat COMPAT_PROFILE="$lane" COMPAT_ARTIFACT="$artifact"
    copy_release_artifact "$artifact" hookkit-rootful-legacy
}

package_modern_lane() {
    local lane=$1 artifact gum_artifact empty_layout
    write_control "$lane"
    write_gum_control "$lane"

    # The framework has no link-time dependency on HKGum. Ask Theos for the
    # framework product alone first so the core package stays small and does
    # not fetch the Gum devkit.
    modern_make HOOKKIT_LANE="$lane" LIBRARY_NAME= package FINALPACKAGE=1 \
        _THEOS_DEB_PACKAGE_CONTROL_PATH="$RELEASE_DIR/control.$lane"
    artifact=$(cat .theos/last_package)
    copy_release_artifact "$artifact" "hookkit-$lane"

    # Then stage just HKGum with its own package metadata. The base package
    # owns the shared release notices; the Gum package depends on that exact
    # base version, so duplicating those files would make dpkg reject the pair.
    # Theos normally stages the package layout for every package, so point this invocation
    # at an empty layout directory instead.
    empty_layout="$STAGE/empty-layout"
    mkdir -p "$empty_layout"
    modern_make HOOKKIT_LANE="$lane" FRAMEWORK_NAME= package FINALPACKAGE=1 \
        THEOS_LAYOUT_DIR="$empty_layout" THEOS_LAYOUT_DIR_NAME="$empty_layout" \
        _THEOS_DEB_PACKAGE_CONTROL_PATH="$RELEASE_DIR/control.$lane.gum"
    gum_artifact=$(cat .theos/last_package)
    copy_release_artifact "$gum_artifact" "hookkit-$lane-gum"

    run_make check-compat COMPAT_PROFILE="$lane" COMPAT_ARTIFACT="$artifact" \
        COMPAT_GUM_ARTIFACT="$gum_artifact"
}

build_rootful_modern() {
    local lane=rootful-modern
    require_modern_toolchain "$lane"
    modern_make HOOKKIT_LANE="$lane" clean
    [ -z "${HOOKKIT_SKIP_LANE_TEST:-}" ] && modern_make HOOKKIT_LANE="$lane" test
    package_modern_lane "$lane"
}

build_rootless() {
    local lane=rootless
    require_modern_toolchain "$lane"
    modern_make HOOKKIT_LANE="$lane" clean
    [ -z "${HOOKKIT_SKIP_LANE_TEST:-}" ] && modern_make HOOKKIT_LANE="$lane" test
    package_modern_lane "$lane"
}

# Existing roothide profile; it shares the modern/rootless compatibility floor.
build_roothide() {
    local lane=roothide
    require_modern_toolchain roothide
    test -d "${THEOS:?}/vendor/mod/roothide"
    modern_make HOOKKIT_LANE="$lane" clean
    [ -z "${HOOKKIT_SKIP_LANE_TEST:-}" ] && modern_make HOOKKIT_LANE="$lane" test
    package_modern_lane "$lane"
}

case ${1:-all} in
    rootful-legacy) build_rootful_legacy ;;
    rootful-modern) build_rootful_modern ;;
    rootless) build_rootless ;;
    roothide) build_roothide ;;
    # Theos's arm64e object cache is shared across toolchains. Build the old
    # ABI lane first so modern objects cannot be reused by the Xcode 11 lane.
    # Host tests use the host clang and are identical for every lane, so run
    # them once for the whole `all` build instead of once per lane (~3x
    # redundant test time cut).
    all) run_make test; export HOOKKIT_SKIP_LANE_TEST=1
        build_rootful_legacy; build_rootful_modern; build_rootless; build_roothide ;;
    rooted) echo "error: rooted is ambiguous; use rootful-legacy or rootful-modern" >&2; exit 2 ;;
    *) echo "usage: $0 [all|rootful-legacy|rootful-modern|rootless|roothide]" >&2; exit 2 ;;
esac
