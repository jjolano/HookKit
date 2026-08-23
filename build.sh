#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "$0")" && pwd)
cd "$ROOT"
: "${THEOS:?THEOS must point to Theos}"
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

    sed -E \
        -e "s/^Package:.*/Package: $package/" \
        -e "s/^Name:.*/Name: $name/" \
        -e "s/firmware \(>= [^)]+\)/firmware (>= $floor)$ceiling/" \
        -e "s/^Architecture:.*/Architecture: $arch/" \
        -e "s/^(Conflicts:.*)/\\1$conflicts/" \
        -e "s/^(Replaces:.*)/\\1$replaces/" \
        -e "s/^(Provides:.*)/\\1$provides/" \
        -e "s/^Version:.*/Version: 3.0.0-1/" \
        control > "build/control.$profile"

    # The shared template carries the legacy provider fields. Empty profile
    # values mean the field must be absent, not silently inherited.
    if [ -z "$conflicts" ]; then
        sed -i '/^Conflicts:/d' "build/control.$profile"
    fi
    if [ -z "$replaces" ]; then
        sed -i '/^Replaces:/d' "build/control.$profile"
    fi
    if [ -z "$provides" ]; then
        sed -i '/^Provides:/d' "build/control.$profile"
    fi
}

check_legacy_abi() {
    local expected_install_name=${1:-} binary
    binary=$(find .theos/obj -path '*/HookKit.framework/HookKit' -type f \
        ! -path '*/arm64/*' ! -path '*/arm64e/*' \
        ! -path '*/armv7/*' ! -path '*/armv7s/*' | head -n 1)
    [ -n "$binary" ] || {
        echo "error: built HookKit framework binary not found" >&2
        return 1
    }
    if [ -n "$expected_install_name" ]; then
        bash scripts/check_legacy_abi.sh "$binary" Tests/LegacyABI/Baselines \
            --expected-install-name "$expected_install_name"
    else
        bash scripts/check_legacy_abi.sh "$binary"
    fi
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
    legacy_make HOOKKIT_LANE="$lane" check-exports
    check_legacy_abi
    cp -p "$artifact" build/hookkit-rootful-legacy.deb
    # Canonical name too: the release step must upload the theos-produced
    # <package>_<version>_<arch>.deb name, not the short lane alias above.
    cp -p "$artifact" "build/$(basename "$artifact")"
}

build_rootful_modern() {
    local lane=rootful-modern artifact
    require_modern_toolchain "$lane"
    write_control "$lane"
    modern_make HOOKKIT_LANE="$lane" clean
    modern_make HOOKKIT_LANE="$lane" test
    modern_make HOOKKIT_LANE="$lane" package FINALPACKAGE=1 _THEOS_DEB_PACKAGE_CONTROL_PATH="build/control.$lane"
    artifact=$(cat .theos/last_package)
    run_make check-compat COMPAT_PROFILE="$lane" COMPAT_ARTIFACT="$artifact"
    modern_make HOOKKIT_LANE="$lane" check-exports
    check_legacy_abi
    cp -p "$artifact" build/hookkit-rootful-modern.deb
    # Canonical name too: the release step must upload the theos-produced
    # <package>_<version>_<arch>.deb name, not the short lane alias above.
    cp -p "$artifact" "build/$(basename "$artifact")"
}

build_rootless() {
    local lane=rootless artifact
    require_modern_toolchain "$lane"
    write_control "$lane"
    modern_make HOOKKIT_LANE="$lane" clean
    modern_make HOOKKIT_LANE="$lane" test
    modern_make HOOKKIT_LANE="$lane" package FINALPACKAGE=1 _THEOS_DEB_PACKAGE_CONTROL_PATH="build/control.$lane"
    artifact=$(cat .theos/last_package)
    run_make check-compat COMPAT_PROFILE="$lane" COMPAT_ARTIFACT="$artifact"
    modern_make HOOKKIT_LANE="$lane" check-exports
    check_legacy_abi
    cp -p "$artifact" build/hookkit-rootless.deb
    # Canonical name too: the release step must upload the theos-produced
    # <package>_<version>_<arch>.deb name, not the short lane alias above.
    cp -p "$artifact" "build/$(basename "$artifact")"
}

# Existing roothide profile; it shares the modern/rootless compatibility floor.
build_roothide() {
    local lane=roothide artifact
    require_modern_toolchain roothide
    test -d "${THEOS:?}/vendor/mod/roothide"
    write_control "$lane"
    modern_make HOOKKIT_LANE="$lane" clean
    modern_make HOOKKIT_LANE="$lane" test
    modern_make HOOKKIT_LANE="$lane" package FINALPACKAGE=1 _THEOS_DEB_PACKAGE_CONTROL_PATH="build/control.$lane"
    artifact=$(cat .theos/last_package)
    run_make check-compat COMPAT_PROFILE="$lane" COMPAT_ARTIFACT="$artifact"
    modern_make HOOKKIT_LANE="$lane" check-exports
    check_legacy_abi '@loader_path/.jbroot/Library/Frameworks/HookKit.framework/HookKit'
    cp -p "$artifact" build/hookkit-roothide.deb
    # Canonical name too: the release step must upload the theos-produced
    # <package>_<version>_<arch>.deb name, not the short lane alias above.
    cp -p "$artifact" "build/$(basename "$artifact")"
}

case ${1:-all} in
    rootful-legacy) build_rootful_legacy ;;
    rootful-modern) build_rootful_modern ;;
    rootless) build_rootless ;;
    roothide) build_roothide ;;
    all) build_rootless; build_rootful_modern; build_rootful_legacy; build_roothide ;;
    rooted) echo "error: rooted is ambiguous; use rootful-legacy or rootful-modern" >&2; exit 2 ;;
    *) echo "usage: $0 [all|rootful-legacy|rootful-modern|rootless|roothide]" >&2; exit 2 ;;
esac
