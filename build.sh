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

require_modern_xcode() {
    [ "$(uname -s)" = Darwin ] || {
        echo "error: $1 requires macOS/Xcode for the new arm64e ABI" >&2
        return 1
    }
    local version major
    version=$(xcodebuild -version | awk 'NR == 1 { print $2 }')
    major=${version%%.*}
    [ "$major" -ge 12 ] || {
        echo "error: $1 requires Xcode 12 or newer (found $version)" >&2
        return 1
    }
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
            conflicts=', me.jjolano.fmwk.hookkit'
            replaces=', me.jjolano.fmwk.hookkit'
            provides=', me.jjolano.fmwk.hookkit'
            ;;
        rootful-modern)
            package=me.jjolano.fmwk.hookkit
            name='HookKit Framework (Modern Rootful)'
            floor=14.0
            arch=iphoneos-arm
            conflicts=', me.jjolano.fmwk.hookkit.legacy'
            replaces=', me.jjolano.fmwk.hookkit.legacy'
            provides=
            ;;
        rootless)
            package=me.jjolano.fmwk.hookkit
            name='HookKit Framework (Rootless)'
            floor=15.0
            arch=iphoneos-arm64
            conflicts=', me.jjolano.fmwk.hookkit.legacy'
            replaces=', me.jjolano.fmwk.hookkit.legacy'
            provides=
            ;;
        roothide)
            package=me.jjolano.fmwk.hookkit
            name='HookKit Framework (RootHide)'
            floor=15.0
            arch=iphoneos-arm64e
            ceiling=', firmware (<< 18.0)'
            conflicts=', me.jjolano.fmwk.hookkit.legacy'
            replaces=', me.jjolano.fmwk.hookkit.legacy'
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
        control > "build/control.$profile"
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
    legacy_make check-exports
    cp -p "$artifact" build/hookkit-rootful-legacy.deb
}

build_rootful_modern() {
    local lane=rootful-modern artifact
    require_modern_xcode "$lane"
    write_control "$lane"
    run_make HOOKKIT_LANE="$lane" clean
    run_make HOOKKIT_LANE="$lane" test
    run_make HOOKKIT_LANE="$lane" package FINALPACKAGE=1 _THEOS_DEB_PACKAGE_CONTROL_PATH="build/control.$lane"
    artifact=$(cat .theos/last_package)
    run_make check-compat COMPAT_PROFILE="$lane" COMPAT_ARTIFACT="$artifact"
    run_make check-exports
    cp -p "$artifact" build/hookkit-rootful-modern.deb
}

build_rootless() {
    local lane=rootless artifact
    require_modern_xcode "$lane"
    write_control "$lane"
    run_make HOOKKIT_LANE="$lane" clean
    run_make HOOKKIT_LANE="$lane" test
    run_make HOOKKIT_LANE="$lane" package FINALPACKAGE=1 _THEOS_DEB_PACKAGE_CONTROL_PATH="build/control.$lane"
    artifact=$(cat .theos/last_package)
    run_make check-compat COMPAT_PROFILE="$lane" COMPAT_ARTIFACT="$artifact"
    run_make check-exports
    cp -p "$artifact" build/hookkit-rootless.deb
}

# Existing roothide profile; it shares the modern/rootless compatibility floor.
build_roothide() {
    local artifact
    require_modern_xcode roothide
    test -d "${THEOS:?}/vendor/mod/roothide"
    write_control roothide
    run_make clean
    run_make test
    run_make THEOS_PACKAGE_SCHEME=roothide ARCHS="arm64 arm64e" TARGET=iphone:clang:latest:15.0 package FINALPACKAGE=1 _THEOS_DEB_PACKAGE_CONTROL_PATH=build/control.roothide
    artifact=$(cat .theos/last_package)
    run_make check-compat COMPAT_PROFILE=roothide COMPAT_ARTIFACT="$artifact"
    run_make check-exports
    cp -p "$artifact" build/hookkit-roothide.deb
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
