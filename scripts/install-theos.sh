#!/usr/bin/env bash
# Build every lane and stage its package-verified framework in Theos.
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "$0")/.." && pwd)
: "${THEOS:?THEOS must point to Theos}"

# A parent `make` that has already included Theos exports computed state for
# its own target. Each lane below must calculate that state afresh.
if [ "${HOOKKIT_THEOS_INSTALL_CLEAN_ENV:-}" != 1 ]; then
    clean_env=(env -i "PATH=$PATH" "HOME=${HOME:-/tmp}" "THEOS=$THEOS"
               "TMPDIR=${TMPDIR:-/tmp}" HOOKKIT_THEOS_INSTALL_CLEAN_ENV=1)
    for variable in MODERN_TOOLCHAIN OLDABI_TOOLCHAIN OLDABI_SDKS OLDABI_DEVELOPER_DIR DEVELOPER_DIR; do
        [ -z "${!variable:-}" ] || clean_env+=("$variable=${!variable}")
    done
    exec "${clean_env[@]}" bash "$0" "$@"
fi
THEOS_LIB="$THEOS/lib"

DPKG_DEB=$(command -v dpkg-deb) || {
    echo "error: dpkg-deb is required" >&2
    exit 1
}

tmp=$(mktemp -d "${TMPDIR:-/tmp}/hookkit-theos-install.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

verify_framework() {
    local framework=$1
    [ -f "$framework/HookKit" ] || {
        echo "error: missing framework binary: $framework/HookKit" >&2
        return 1
    }
    [ -d "$framework/Headers" ] || {
        echo "error: missing framework Headers/: $framework" >&2
        return 1
    }
    [ -f "$framework/Headers/HookKit.h" ] || {
        echo "error: missing framework header: $framework/Headers/HookKit.h" >&2
        return 1
    }
    [ -f "$framework/Headers/HookKit/HookKit.h" ] || {
        echo "error: missing framework header: $framework/Headers/HookKit/HookKit.h" >&2
        return 1
    }
}

install_framework() {
    local source=$1 destination=$2 parent staging backup=
    parent=$(dirname "$destination")
    mkdir -p "$parent"
    [ ! -e "$destination" ] || [ -d "$destination" ] || {
        echo "error: refusing to replace non-directory $destination" >&2
        return 1
    }

    staging=$(mktemp -d "$parent/.hookkit-install.XXXXXX")
    cp -R "$source" "$staging/HookKit.framework"
    verify_framework "$staging/HookKit.framework"

    if [ -e "$destination" ] || [ -L "$destination" ]; then
        backup="$staging/previous"
        mv "$destination" "$backup"
    fi
    if ! mv "$staging/HookKit.framework" "$destination"; then
        [ -z "$backup" ] || mv "$backup" "$destination"
        rm -rf "$staging"
        return 1
    fi

    verify_framework "$destination"
    cmp -s "$source/HookKit" "$destination/HookKit"
    cmp -s "$source/Headers/HookKit.h" "$destination/Headers/HookKit.h"
    cmp -s "$source/Headers/HookKit/HookKit.h" "$destination/Headers/HookKit/HookKit.h"
    rm -rf "$staging"
}

install_lane() {
    local lane=$1 artifact=$2 payload=$3 destination=$4 root framework
    root="$tmp/$lane"
    "$DPKG_DEB" -x "$ROOT/build/$artifact" "$root"
    framework="$root/$payload"
    verify_framework "$framework"
    install_framework "$framework" "$THEOS_LIB/$destination"
    printf 'Installed %s: %s\n' "$lane" "$THEOS_LIB/$destination"
}

cd "$ROOT"
./build.sh all

install_lane rootful-modern hookkit-rootful-modern.deb Library/Frameworks/HookKit.framework HookKit.framework
install_lane rootful-legacy hookkit-rootful-legacy.deb Library/Frameworks/HookKit.framework iphone/rootful-legacy/HookKit.framework
install_lane rootless hookkit-rootless.deb var/jb/Library/Frameworks/HookKit.framework iphone/rootless/HookKit.framework
install_lane roothide hookkit-roothide.deb Library/Frameworks/HookKit.framework iphone/roothide/HookKit.framework
