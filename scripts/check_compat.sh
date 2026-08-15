#!/usr/bin/env bash
# Verify packaged slices and deployment targets without modifying Mach-O files.
set -euo pipefail

PROFILE=${1:-}
ARTIFACT=${2:-}

case "$PROFILE" in
    rootful-legacy)
        package=me.jjolano.fmwk.hookkit.legacy
        package_arch=iphoneos-arm
        floor=9.0
        ceiling=14.0
        hookkit_expected='armv7=9.0,armv7s=9.0,arm64=9.0,arm64e=12.0'
        hkgum_expected=
        ;;
    rootful-modern)
        package=me.jjolano.fmwk.hookkit
        package_arch=iphoneos-arm
        floor=14.0
        ceiling=
        hookkit_expected='arm64=14.0,arm64e=14.0'
        hkgum_expected="$hookkit_expected"
        ;;
    rootless)
        package=me.jjolano.fmwk.hookkit
        package_arch=iphoneos-arm64
        floor=15.0
        ceiling=
        hookkit_expected='arm64=15.0,arm64e=15.0'
        hkgum_expected="$hookkit_expected"
        ;;
    roothide)
        package=me.jjolano.fmwk.hookkit
        package_arch=iphoneos-arm64e
        floor=15.0
        ceiling=18.0
        hookkit_expected='arm64=15.0,arm64e=15.0'
        hkgum_expected="$hookkit_expected"
        ;;
    *) echo "usage: $0 rootful-legacy|rootful-modern|rootless|roothide ARTIFACT.deb" >&2; exit 2 ;;
esac

[ -f "$ARTIFACT" ] || { echo "error: artifact not found: $ARTIFACT" >&2; exit 2; }

find_tool() {
    local candidate
    for candidate in "$@"; do
        [ -n "$candidate" ] && [ -x "$candidate" ] && { printf '%s\n' "$candidate"; return; }
    done
    return 1
}

DPKG_DEB=$(find_tool \
    "$(command -v dpkg-deb 2>/dev/null || true)" \
    "${THEOS:-}/toolchain/darwin/iphone/bin/dpkg-deb" \
    "${THEOS:-}/toolchain/linux/iphone/bin/dpkg-deb") || {
    echo "error: dpkg-deb not found" >&2; exit 1;
}
LIPO=$(find_tool \
    "$(command -v lipo 2>/dev/null || true)" \
    "$(xcrun --find lipo 2>/dev/null || true)" \
    "${THEOS:-}/toolchain/darwin/iphone/bin/lipo" \
    "${THEOS:-}/toolchain/linux/iphone/bin/lipo") || {
    echo "error: lipo not found" >&2; exit 1;
}
VTOOL=$(find_tool \
    "$(command -v vtool 2>/dev/null || true)" \
    "$(xcrun --find vtool 2>/dev/null || true)" \
    "${THEOS:-}/toolchain/darwin/iphone/bin/vtool" \
    "${THEOS:-}/toolchain/linux/iphone/bin/vtool" || true)
OTOOL=$(find_tool \
    "$(command -v otool 2>/dev/null || true)" \
    "$(xcrun --find otool 2>/dev/null || true)" \
    "${THEOS:-}/toolchain/darwin/iphone/bin/otool" \
    "${THEOS:-}/toolchain/linux/iphone/bin/otool" || true)
OBJDUMP=$(find_tool \
    "$(command -v llvm-objdump 2>/dev/null || true)" \
    "$(find ~/.local/share/mise/installs/swift -path '*/usr/bin/llvm-objdump' -type f 2>/dev/null | sort | tail -n 1)" || true)
[ -n "$VTOOL$OTOOL$OBJDUMP" ] || { echo "error: vtool, otool, or llvm-objdump is required" >&2; exit 1; }

tmp=$(mktemp -d "${TMPDIR:-/tmp}/hookkit-compat.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
"$DPKG_DEB" -x "$ARTIFACT" "$tmp/root"

actual_package=$("$DPKG_DEB" -f "$ARTIFACT" Package)
actual_arch=$("$DPKG_DEB" -f "$ARTIFACT" Architecture)
depends=$("$DPKG_DEB" -f "$ARTIFACT" Depends)
[ "$actual_package" = "$package" ] || { echo "FAIL package '$actual_package' != '$package'" >&2; exit 1; }
[ "$actual_arch" = "$package_arch" ] || { echo "FAIL architecture '$actual_arch' != '$package_arch'" >&2; exit 1; }
printf '%s\n' "$depends" | grep -Fq "firmware (>= $floor)" || {
    echo "FAIL package dependency does not require firmware >= $floor" >&2; exit 1;
}
[ -z "$ceiling" ] || printf '%s\n' "$depends" | grep -Fq "firmware (<< $ceiling)" || {
    echo "FAIL package dependency does not require firmware << $ceiling" >&2; exit 1;
}

if [ "$PROFILE" = rootful-legacy ]; then
    for field in Conflicts Replaces Provides; do
        "$DPKG_DEB" -f "$ARTIFACT" "$field" | grep -Fq me.jjolano.fmwk.hookkit || {
            echo "FAIL legacy package $field does not include me.jjolano.fmwk.hookkit" >&2
            exit 1
        }
    done
else
    for field in Conflicts Replaces; do
        "$DPKG_DEB" -f "$ARTIFACT" "$field" | grep -Fq me.jjolano.fmwk.hookkit.legacy || {
            echo "FAIL modern package $field does not include me.jjolano.fmwk.hookkit.legacy" >&2
            exit 1
        }
    done
fi

HOOKKIT=$(find "$tmp/root" -path '*/HookKit.framework/HookKit' -type f -print)
[ "$(printf '%s\n' "$HOOKKIT" | sed '/^$/d' | wc -l | tr -d ' ')" = 1 ] || {
    echo "FAIL package must contain exactly one HookKit framework binary" >&2; exit 1;
}
HKGUM=$(find "$tmp/root" -name HKGum.dylib -type f -print)
if [ -n "$hkgum_expected" ]; then
    [ "$(printf '%s\n' "$HKGUM" | sed '/^$/d' | wc -l | tr -d ' ')" = 1 ] || {
        echo "FAIL package must contain exactly one HKGum.dylib" >&2; exit 1;
    }
elif [ -n "$HKGUM" ]; then
    echo "FAIL legacy package must not contain HKGum.dylib" >&2
    exit 1
fi

load_commands() {
    local slice=$1 raw
    if [ -n "$VTOOL" ]; then
        raw=$("$VTOOL" -show-build "$slice" 2>/dev/null || true)
        if printf '%s\n' "$raw" | grep -qE '^[[:space:]]*minos[[:space:]]'; then
            printf 'vtool -show-build\n%s\n' "$raw"
            return
        fi
    fi
    if [ -n "$OTOOL" ]; then
        raw=$("$OTOOL" -l "$slice")
        printf 'otool -l\n'
        printf '%s\n' "$raw" | awk '
            /cmd LC_BUILD_VERSION|cmd LC_VERSION_MIN_IPHONEOS/ { lines = 6 }
            lines > 0 { print; lines-- }
        '
        return
    fi
    raw=$("$OBJDUMP" --macho --all-headers "$slice")
    printf 'llvm-objdump --macho --all-headers\n'
    printf '%s\n' "$raw" | awk '
        /cmd LC_BUILD_VERSION|cmd LC_VERSION_MIN_IPHONEOS/ { lines = 6 }
        lines > 0 { print; lines-- }
    '
}

check_binary() {
    local bin=$1 expected=$2 actual_archs expected_archs old_ifs item arch wanted slice output actual sdk header
    echo "lipo -info $bin"
    "$LIPO" -info "$bin"
    actual_archs=$("$LIPO" -archs "$bin" | tr ' ' '\n' | sort | xargs)
    expected_archs=$(printf '%s' "$expected" | tr ',' '\n' | cut -d= -f1 | sort | xargs)
    [ "$actual_archs" = "$expected_archs" ] || {
        echo "FAIL $bin architectures '$actual_archs' != '$expected_archs'" >&2
        return 1
    }

    old_ifs=$IFS
    IFS=,
    for item in $expected; do
        arch=${item%%=*}
        wanted=${item#*=}
        slice="$tmp/$arch-$(basename "$bin")"
        "$LIPO" -thin "$arch" "$bin" -output "$slice"
        output=$(load_commands "$slice")
        echo "$bin [$arch]"
        printf '%s\n' "$output"
        actual=$(printf '%s\n' "$output" | awk '
            /cmd LC_BUILD_VERSION/ { mode = "build"; next }
            mode == "build" && $1 == "minos" { print $2; exit }
            /cmd LC_VERSION_MIN_IPHONEOS/ { mode = "legacy"; next }
            mode == "legacy" && $1 == "version" { print $2; exit }
            $1 == "minos" { print $2; exit }
        ')
        [ "$actual" = "$wanted" ] || {
            echo "FAIL $bin [$arch] minimum iOS '$actual' != '$wanted'" >&2
            IFS=$old_ifs
            return 1
        }
        if [ "$PROFILE:$arch" = rootful-legacy:arm64e ]; then
            sdk=$(printf '%s\n' "$output" | awk '$1 == "sdk" { print $2; exit }')
            [ "$sdk" = 13.7 ] || {
                echo "FAIL $bin [$arch] SDK '$sdk' != Xcode 11.7 SDK 13.7" >&2
                IFS=$old_ifs
                return 1
            }
            [ -n "$OTOOL" ] || {
                echo "FAIL otool is required to verify old-ABI arm64e" >&2
                IFS=$old_ifs
                return 1
            }
            header=$("$OTOOL" -hv "$slice" | tail -n 1)
            printf '%s\n' "$header" | grep -Eq '[[:space:]]E[[:space:]]+0x00[[:space:]]' || {
                echo "FAIL $bin [$arch] is not an old-ABI arm64e Mach-O" >&2
                IFS=$old_ifs
                return 1
            }
        fi
        echo "PASS $bin [$arch] iOS $actual+"
    done
    IFS=$old_ifs
}

echo "Package: $actual_package ($actual_arch), firmware >= $floor"
check_binary "$HOOKKIT" "$hookkit_expected"
[ -z "$hkgum_expected" ] || check_binary "$HKGUM" "$hkgum_expected"
echo "OK: $PROFILE artifact has exactly the expected products, slices, and deployment targets"
