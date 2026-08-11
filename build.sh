#!/usr/bin/env bash
set -e

cd -- "$(dirname -- "$0")"

# create fresh build directory
rm -rf build
mkdir -p build

# Rooted: one fat package spanning armv7 through arm64e, matching how 1.0.x
# shipped. The Makefile defaults already cover this, so no overrides here.
# theos records the deb it just built in .theos/last_package and derives the
# name from control, so version and arch are never repeated in this script.
build_rooted() {
    make clean &&
    make test &&
    make package FINALPACKAGE=1 &&
    cp -p "$(cat .theos/last_package)" build/ &&
    make check-exports

    rm -rf "${THEOS:?}/lib/HookKit.framework"
}

# Rootless: modern jailbreaks only, so 64-bit slices and a 12.0 floor. The
# Architecture field (iphoneos-arm64) is what actually keeps this deb away
# from rooted devices, so control's shared 9.0 Depends floor is harmless here.
build_rootless() {
    make clean &&
    make test &&
    THEOS_PACKAGE_SCHEME=rootless ARCHS="arm64 arm64e" TARGET=iphone:clang:latest:12.0 make package FINALPACKAGE=1 &&
    cp -p "$(cat .theos/last_package)" build/ &&
    make check-exports

    rm -rf "${THEOS:?}/lib/HookKit.framework"
}

# roothide: iOS 15-17, random-named jbroot (no /var/jb). Requires the
# roothide theos fork (THEOS_PACKAGE_SCHEME=roothide) + libroothide; the
# Makefile defines HK_ROOTHIDE for this scheme.
build_roothide() {
    # The roothide deb needs firmware (>= 15.0) to match the 15.0 deploy
    # target, but the shared control says 9.0. Never rewrite the tracked
    # control in place (a kill -9 mid-build would leave the tree modified):
    # generate a scheme-specific control in the build dir and hand it to the
    # package stage. theos has no public override for the deb control path,
    # so pass its internal variable on the command line (command-line
    # assignments beat the := in deb.mk); the grep below fails loudly if a
    # theos upgrade renames it and the stock 9.0 control gets used instead.
    make clean &&
    test -d "${THEOS:?}/vendor/mod/roothide" &&
    sed -e 's/firmware (>= 9.0)/firmware (>= 15.0)/' control > build/control.roothide &&
    make test &&
    THEOS_PACKAGE_SCHEME=roothide ARCHS="arm64 arm64e" TARGET=iphone:clang:latest:15.0 make package FINALPACKAGE=1 _THEOS_DEB_PACKAGE_CONTROL_PATH=build/control.roothide &&
    grep -q 'firmware (>= 15.0)' .theos/_/DEBIAN/control &&
    cp -p "$(cat .theos/last_package)" build/ &&
    make check-exports

    rm -rf "${THEOS:?}/lib/HookKit.framework"
}

case ${1:-all} in
    rootless) build_rootless ;;
    rooted) build_rooted ;;
    roothide) build_roothide ;;
    all) build_rootless; build_rooted; build_roothide ;;
    *) echo "usage: $0 [all|rootless|rooted|roothide]" >&2; exit 1 ;;
esac
