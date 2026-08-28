#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "$0")" && pwd)
cd "$ROOT"
: "${THEOS:?THEOS must point to Theos}"
PACKAGE_VERSION=3.0.0-1
STAGE=$(mktemp -d "${TMPDIR:-/tmp}/hookkit-build.XXXXXX")
MAKE_ARGS=("THEOS_LIBRARY_PATH=$STAGE/lib")
trap 'rm -rf "$STAGE"' EXIT

rm -rf build
mkdir -p build

run_make() {
    make "${MAKE_ARGS[@]}" "$@"
}

# Modern lanes need a toolchain that stamps arm64e slices with the versioned
# ptrauth ABI (cpusubtype 0x80000002). Xcode 12+ does; theos's bundled Linux
# clang does not -- it emits 0x00000002, the preview ABI modern ld rejects as
# "arm64e.old". scripts/setup-modern-toolchain.sh assembles a Linux toolchain
# that can, and MODERN_TOOLCHAIN points at it.
MODERN_TOOLCHAIN=${MODERN_TOOLCHAIN:-$THEOS/toolchain/modern/linux/iphone}

require_modern_toolchain() {
    if [ "$(uname -s)" = Darwin ]; then
        local version major
        version=$(xcodebuild -version | awk 'NR == 1 { print $2 }')
        major=${version%%.*}
        [ "$major" -ge 12 ] || {
            echo "error: $1 requires Xcode 12 or newer (found $version)" >&2
            return 1
        }
        return
    fi

    # Probe rather than trust: a toolchain that merely *exists* here is more
    # dangerous than a missing one, because the wrong compiler produces an
    # old-ABI arm64e slice that links locally and is rejected everywhere else.
    [ -x "$MODERN_TOOLCHAIN/bin/clang" ] || {
        echo "error: $1 needs a new-ABI arm64e toolchain at $MODERN_TOOLCHAIN" >&2
        echo "       run scripts/setup-modern-toolchain.sh to assemble one" >&2
        return 1
    }
    bash "$ROOT/scripts/setup-modern-toolchain.sh" --verify "$MODERN_TOOLCHAIN" >/dev/null || {
        echo "error: $1: $MODERN_TOOLCHAIN emits old-ABI arm64e" >&2
        echo "       re-run scripts/setup-modern-toolchain.sh" >&2
        return 1
    }
}

# Modern-lane make: on Linux the toolchain above replaces theos's default.
modern_make() {
    if [ "$(uname -s)" = Darwin ]; then
        make "${MAKE_ARGS[@]}" "$@"
    else
        # Theos's Linux default predates this modern wrapper and otherwise
        # selects libroot_oldabi.a even when clang emits the new ABI.
        make "${MAKE_ARGS[@]}" SDKBINPATH="$MODERN_TOOLCHAIN/bin" IS_NEW_ABI=1 "$@"
    fi
}

require_oldabi_toolchain() {
    if [ "$(uname -s)" = Darwin ]; then
        : "${OLDABI_DEVELOPER_DIR:?set OLDABI_DEVELOPER_DIR to Xcode 11.7/Contents/Developer}"
        DEVELOPER_DIR="$OLDABI_DEVELOPER_DIR" xcodebuild -version | grep -q '^Xcode 11\.7$' || {
            echo "error: OLDABI_DEVELOPER_DIR is not Xcode 11.7" >&2
            return 1
        }
        return
    fi

    OLDABI_TOOLCHAIN=${OLDABI_TOOLCHAIN:-$THEOS/toolchain/oldabi/linux/iphone}
    OLDABI_SDKS=${OLDABI_SDKS:-$THEOS/sdks}
    [ -x "$OLDABI_TOOLCHAIN/bin/clang" ] || {
        echo "error: OLDABI_TOOLCHAIN/bin/clang is not executable" >&2
        return 1
    }
    [ -d "$OLDABI_SDKS/iPhoneOS13.7.sdk" ] || {
        echo "error: OLDABI_SDKS/iPhoneOS13.7.sdk is missing" >&2
        return 1
    }

    local version major
    version=$("$OLDABI_TOOLCHAIN/bin/clang" -dumpversion)
    major=${version%%.*}
    [ "$major" -lt 12 ] || {
        echo "error: rootful-legacy requires Clang older than 12 (found $version)" >&2
        return 1
    }
}

legacy_make() {
    if [ "$(uname -s)" = Darwin ]; then
        DEVELOPER_DIR="$OLDABI_DEVELOPER_DIR" make "${MAKE_ARGS[@]}" "$@"
    else
        make "${MAKE_ARGS[@]}" SDKBINPATH="$OLDABI_TOOLCHAIN/bin" THEOS_SDKS_PATH="$OLDABI_SDKS" "$@"
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
    case "$profile" in
        rootful-legacy)
            package=me.jjolano.fmwk.hookkit.legacy
            name='HookKit Framework (Legacy Rootful)'
            floor=9.0
            arch=iphoneos-arm
            ceiling=', firmware (<< 14.0)'
            conflicts=', me.jjolano.fmwk.hookkit, me.jjolano.fmwk.hookkit3'
            replaces=', me.jjolano.fmwk.hookkit, me.jjolano.fmwk.hookkit3'
            provides=', me.jjolano.fmwk.hookkit'
            ;;
        rootful-modern)
            package=me.jjolano.fmwk.hookkit
            name='HookKit Framework (Modern Rootful)'
            floor=14.0
            arch=iphoneos-arm
            conflicts=', me.jjolano.fmwk.hookkit.legacy, me.jjolano.fmwk.hookkit3'
            replaces=', me.jjolano.fmwk.hookkit.legacy, me.jjolano.fmwk.hookkit3'
            provides=
            ;;
        rootless)
            package=me.jjolano.fmwk.hookkit
            name='HookKit Framework (Rootless)'
            floor=15.0
            arch=iphoneos-arm64
            conflicts=', me.jjolano.fmwk.hookkit.legacy, me.jjolano.fmwk.hookkit3'
            replaces=', me.jjolano.fmwk.hookkit.legacy, me.jjolano.fmwk.hookkit3'
            provides=
            ;;
        roothide)
            package=me.jjolano.fmwk.hookkit
            name='HookKit Framework (RootHide)'
            floor=15.0
            arch=iphoneos-arm64e
            ceiling=', firmware (<< 18.0)'
            conflicts=', me.jjolano.fmwk.hookkit.legacy, me.jjolano.fmwk.hookkit3'
            replaces=', me.jjolano.fmwk.hookkit.legacy, me.jjolano.fmwk.hookkit3'
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
        control > "build/control.$profile"

    if [ -z "$conflicts" ]; then drop_field "build/control.$profile" Conflicts; fi
    if [ -z "$replaces" ]; then drop_field "build/control.$profile" Replaces; fi
    if [ -z "$provides" ]; then drop_field "build/control.$profile" Provides; fi
}

write_gum_control() {
    local profile=$1 name arch
    case "$profile" in
        rootful-modern)
            name='Modern Rootful'
            arch=iphoneos-arm
            ;;
        rootless)
            name='Rootless'
            arch=iphoneos-arm64
            ;;
        roothide)
            name='RootHide'
            arch=iphoneos-arm64e
            ;;
        *) echo "error: no Gum package profile for $profile" >&2; return 1 ;;
    esac

    printf '%s\n' \
        'Package: me.jjolano.fmwk.hookkit.gum' \
        "Name: HookKit Frida Gum Provider ($name)" \
        "Depends: me.jjolano.fmwk.hookkit (= $PACKAGE_VERSION), firmware (>= 15.0)" \
        "Version: $PACKAGE_VERSION" \
        "Architecture: $arch" \
        'Description: Optional Frida Gum provider for HookKit.' \
        'Maintainer: HookKit maintainers' \
        'Author: HookKit contributors' \
        'Section: Development' \
        'Tag: role::developer' > "build/control.$profile.gum"
}

copy_release_artifact() {
    local artifact=$1 name=$2
    cp -p "$artifact" "build/$name.deb"
    cp -p "$artifact" "build/$(basename "$artifact")"
}

build_rootful_legacy() {
    local lane=rootful-legacy artifact
    require_oldabi_toolchain
    write_control "$lane"
    legacy_make HOOKKIT_LANE="$lane" clean
    legacy_make HOOKKIT_LANE="$lane" test
    legacy_make HOOKKIT_LANE="$lane" package FINALPACKAGE=1 _THEOS_DEB_PACKAGE_CONTROL_PATH="build/control.$lane"
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
        _THEOS_DEB_PACKAGE_CONTROL_PATH="build/control.$lane"
    artifact=$(cat .theos/last_package)
    copy_release_artifact "$artifact" "hookkit-$lane"

    # Then stage just HKGum with its own package metadata. The base package
    # owns the shared release notices; the Gum package depends on that exact
    # base version, so duplicating those files would make dpkg reject the pair.
    # Theos normally stages layout/ for every package, so point this invocation
    # at an empty layout directory instead.
    empty_layout="$STAGE/empty-layout"
    mkdir -p "$empty_layout"
    modern_make HOOKKIT_LANE="$lane" FRAMEWORK_NAME= package FINALPACKAGE=1 \
        THEOS_LAYOUT_DIR="$empty_layout" THEOS_LAYOUT_DIR_NAME="$empty_layout" \
        _THEOS_DEB_PACKAGE_CONTROL_PATH="build/control.$lane.gum"
    gum_artifact=$(cat .theos/last_package)
    copy_release_artifact "$gum_artifact" "hookkit-$lane-gum"

    run_make check-compat COMPAT_PROFILE="$lane" COMPAT_ARTIFACT="$artifact" \
        COMPAT_GUM_ARTIFACT="$gum_artifact"
}

build_rootful_modern() {
    local lane=rootful-modern
    require_modern_toolchain "$lane"
    modern_make HOOKKIT_LANE="$lane" clean
    modern_make HOOKKIT_LANE="$lane" test
    package_modern_lane "$lane"
}

build_rootless() {
    local lane=rootless
    require_modern_toolchain "$lane"
    modern_make HOOKKIT_LANE="$lane" clean
    modern_make HOOKKIT_LANE="$lane" test
    package_modern_lane "$lane"
}

# Existing roothide profile; it shares the modern/rootless compatibility floor.
build_roothide() {
    local lane=roothide
    require_modern_toolchain roothide
    test -d "${THEOS:?}/vendor/mod/roothide"
    modern_make HOOKKIT_LANE="$lane" clean
    modern_make HOOKKIT_LANE="$lane" test
    package_modern_lane "$lane"
}

case ${1:-all} in
    rootful-legacy) build_rootful_legacy ;;
    rootful-modern) build_rootful_modern ;;
    rootless) build_rootless ;;
    roothide) build_roothide ;;
    # Theos's arm64e object cache is shared across toolchains. Build the old
    # ABI lane first so modern objects cannot be reused by the Xcode 11 lane.
    all) build_rootful_legacy; build_rootful_modern; build_rootless; build_roothide ;;
    rooted) echo "error: rooted is ambiguous; use rootful-legacy or rootful-modern" >&2; exit 2 ;;
    *) echo "usage: $0 [all|rootful-legacy|rootful-modern|rootless|roothide]" >&2; exit 2 ;;
esac
