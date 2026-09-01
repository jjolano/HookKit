#!/usr/bin/env bash
# Verify packaged products, slices, deployment targets, and notices without
# modifying Mach-O files.
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "$0")/../.." && pwd)
PROFILE=${1:-}
ARTIFACT=${2:-}
GUM_ARTIFACT=${3:-}

case "$PROFILE" in
    rootful-legacy)
        package=me.jjolano.fmwk.hookkit.legacy
        package_arch=iphoneos-arm
        floor=9.0
        ceiling=14.0
        hookkit_expected='armv7=9.0,armv7s=9.0,arm64=9.0,arm64e=12.0'
        gum_package=
        gum_arch=
        gum_expected=
        arm64e_abi=00
        hookkit_install_name='@rpath/HookKit.framework/HookKit'
        ;;
    rootful-modern)
        package=me.jjolano.fmwk.hookkit
        package_arch=iphoneos-arm
        floor=14.0
        ceiling=
        hookkit_expected='arm64=14.0,arm64e=14.0'
        gum_package=me.jjolano.fmwk.hookkit.gum
        gum_arch=iphoneos-arm
        gum_expected="$hookkit_expected"
        arm64e_abi=80
        hookkit_install_name='@rpath/HookKit.framework/HookKit'
        ;;
    rootless)
        package=me.jjolano.fmwk.hookkit
        package_arch=iphoneos-arm64
        floor=15.0
        ceiling=
        hookkit_expected='arm64=15.0,arm64e=15.0'
        gum_package=me.jjolano.fmwk.hookkit.gum
        gum_arch=iphoneos-arm64
        gum_expected="$hookkit_expected"
        arm64e_abi=80
        hookkit_install_name='@rpath/HookKit.framework/HookKit'
        ;;
    roothide)
        package=me.jjolano.fmwk.hookkit
        package_arch=iphoneos-arm64e
        floor=15.0
        ceiling=
        hookkit_expected='arm64=15.0,arm64e=15.0'
        gum_package=me.jjolano.fmwk.hookkit.gum
        gum_arch=iphoneos-arm64e
        gum_expected="$hookkit_expected"
        arm64e_abi=80
        hookkit_install_name='@loader_path/.jbroot/Library/Frameworks/HookKit.framework/HookKit'
        ;;
    *) echo "usage: $0 rootful-legacy|rootful-modern|rootless|roothide CORE.deb [GUM.deb]" >&2; exit 2 ;;
esac

[ -f "$ARTIFACT" ] || { echo "error: artifact not found: $ARTIFACT" >&2; exit 2; }
if [ -n "$gum_package" ]; then
    [ -f "$GUM_ARTIFACT" ] || { echo "error: Gum artifact not found: $GUM_ARTIFACT" >&2; exit 2; }
elif [ -n "$GUM_ARTIFACT" ]; then
    echo "error: legacy package must not have a Gum artifact" >&2
    exit 2
fi

# Tool lookup, in order: PATH, Xcode, then any Theos toolchain. The toolchain
# layout varies per install (darwin/iphone/bin, linux/iphone/bin, and the
# rootful-legacy lane's hand-installed oldabi/linux/iphone/bin), so glob for
# the bin directory rather than hardcoding the two Theos ships. Multiple names
# may be given; the first tool found wins.
find_tool() {
    local name candidate dir
    for name in "$@"; do
        candidate=$(command -v "$name" 2>/dev/null || true)
        [ -n "$candidate" ] && [ -x "$candidate" ] && { printf '%s\n' "$candidate"; return; }

        candidate=$(xcrun --find "$name" 2>/dev/null || true)
        [ -n "$candidate" ] && [ -x "$candidate" ] && { printf '%s\n' "$candidate"; return; }

        for dir in "${THEOS:-/nonexistent}"/toolchain/*/bin \
                   "${THEOS:-/nonexistent}"/toolchain/*/*/bin \
                   "${THEOS:-/nonexistent}"/toolchain/*/*/*/bin; do
            [ -x "$dir/$name" ] && { printf '%s\n' "$dir/$name"; return; }
        done
    done
    return 1
}

DPKG_DEB=$(find_tool dpkg-deb) || { echo "error: dpkg-deb not found" >&2; exit 1; }
LIPO=$(find_tool lipo llvm-lipo) || { echo "error: lipo not found" >&2; exit 1; }
OD=$(find_tool od) || { echo "error: od not found" >&2; exit 1; }
VTOOL=$(find_tool vtool || true)
OTOOL=$(find_tool otool || true)
OBJDUMP=$(find_tool llvm-objdump \
    || find ~/.local/share/mise/installs/swift -path '*/usr/bin/llvm-objdump' -type f 2>/dev/null | sort | tail -n 1 \
    || true)
[ -n "$VTOOL$OTOOL$OBJDUMP" ] || { echo "error: vtool, otool, or llvm-objdump is required" >&2; exit 1; }
[ -n "$OTOOL$OBJDUMP" ] || { echo "error: otool or llvm-objdump is required to verify LC_ID_DYLIB" >&2; exit 1; }

tmp=$(mktemp -d "${TMPDIR:-/tmp}/hookkit-compat.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
"$DPKG_DEB" -x "$ARTIFACT" "$tmp/root"

if [ "$PROFILE" = rootless ]; then
    notice_prefix=var/jb
else
    notice_prefix=
fi
NOTICE_FILES=(
    LICENSE
    THIRD_PARTY_NOTICES
    Frida-Gum-17.17.0-SBOM.md
    licenses/Apache-2.0-Dobby.txt
    licenses/Capstone-BSD-3-Clause.txt
    licenses/Frida-Gum-wxWindows-3.1.txt
    licenses/GLib-LGPL-2.1-or-later.txt
    licenses/libffi-MIT.txt
    licenses/PCRE2-BSD-3-Clause.txt
    licenses/XZ-Utils.txt
    licenses/zlib.txt
)

check_notice_tree() {
    local package_root=$1 rel source installed
    for rel in "${NOTICE_FILES[@]}"; do
        source="$ROOT/packaging/layout/usr/share/doc/hookkit/$rel"
        installed="$package_root/$notice_prefix/usr/share/doc/hookkit/$rel"
        [ -s "$source" ] || {
            echo "FAIL release notice source is missing: $source" >&2
            return 1
        }
        [ -s "$installed" ] || {
            echo "FAIL package is missing release notice: $installed" >&2
            return 1
        }
        cmp -s "$source" "$installed" || {
            echo "FAIL packaged release notice differs: $rel" >&2
            return 1
        }
    done
}

actual_package=$("$DPKG_DEB" -f "$ARTIFACT" Package)
actual_arch=$("$DPKG_DEB" -f "$ARTIFACT" Architecture)
actual_version=$("$DPKG_DEB" -f "$ARTIFACT" Version)
depends=$("$DPKG_DEB" -f "$ARTIFACT" Depends)
[ "$actual_package" = "$package" ] || { echo "FAIL package '$actual_package' != '$package'" >&2; exit 1; }
[ "$actual_arch" = "$package_arch" ] || { echo "FAIL architecture '$actual_arch' != '$package_arch'" >&2; exit 1; }
[ "$actual_version" = 3.0.0-1 ] || { echo "FAIL version '$actual_version' != '3.0.0-1'" >&2; exit 1; }
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
    echo "FAIL package must contain exactly one expected framework binary" >&2; exit 1;
}
framework=$(dirname "$HOOKKIT")
[ -f "$framework/Headers/HookKit.h" ] || {
    echo "FAIL package is missing <HookKit.h>" >&2; exit 1;
}
[ -f "$framework/Headers/HookKit/HookKit.h" ] || {
    echo "FAIL package is missing new <HookKit/HookKit.h>" >&2; exit 1;
}
[ -z "$(find "$tmp/root" -path '*/HookKit3.framework/*' -print)" ] || {
    echo "FAIL package contains retired HookKit3.framework files" >&2; exit 1;
}
[ -z "$(find "$tmp/root" -name 'Info-HookKit3.plist' -print)" ] || {
    echo "FAIL package contains a retired HookKit3 bundle plist" >&2; exit 1;
}
HKGUM=$(find "$tmp/root" -name HKGum.dylib -type f -print)
if [ -n "$HKGUM" ]; then
    echo "FAIL core package must not contain HKGum.dylib" >&2
    exit 1
fi

check_notice_tree "$tmp/root"

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
    local bin=$1 expected=$2 scratch=${3:-$tmp} expected_install_name=${4:-} actual_archs expected_archs old_ifs item arch wanted slice output actual sdk abi install_name
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
        slice="$scratch/$arch-$(basename "$bin")"
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
        fi
        if [ "$arch" = arm64e ]; then
            abi=$("$OD" -An -tx1 -j11 -N1 "$slice" | tr -d '[:space:]')
            [ "$abi" = "$arm64e_abi" ] || {
                echo "FAIL $bin [$arch] arm64e ABI '$abi' != '$arm64e_abi'" >&2
                IFS=$old_ifs
                return 1
            }
            echo "PASS $bin [$arch] arm64e ABI $abi"
        fi
        if [ -n "$expected_install_name" ]; then
            if [ -n "$OTOOL" ]; then
                install_name=$("$OTOOL" -l "$slice" | awk '
                    /cmd LC_ID_DYLIB/ { found = 1; next }
                    found && $1 == "name" { print $2; exit }
                ')
            else
                install_name=$("$OBJDUMP" --macho --all-headers "$slice" | awk '
                    /cmd LC_ID_DYLIB/ { found = 1; next }
                    found && $1 == "name" { print $2; exit }
                ')
            fi
            [ "$install_name" = "$expected_install_name" ] || {
                echo "FAIL $bin [$arch] LC_ID_DYLIB '$install_name' != '$expected_install_name'" >&2
                IFS=$old_ifs
                return 1
            }
            echo "PASS $bin [$arch] LC_ID_DYLIB $install_name"
        fi
        echo "PASS $bin [$arch] iOS $actual+"
    done
    IFS=$old_ifs
}

check_gum_package() {
    local gum_root actual_package actual_arch actual_version depends field expected_path hkgum_rel count actual_files expected_files
    [ -n "$gum_package" ] || return 0

    gum_root="$tmp/gum"
    "$DPKG_DEB" -x "$GUM_ARTIFACT" "$gum_root"
    actual_package=$("$DPKG_DEB" -f "$GUM_ARTIFACT" Package)
    actual_arch=$("$DPKG_DEB" -f "$GUM_ARTIFACT" Architecture)
    actual_version=$("$DPKG_DEB" -f "$GUM_ARTIFACT" Version)
    depends=$("$DPKG_DEB" -f "$GUM_ARTIFACT" Depends)
    [ "$actual_package" = "$gum_package" ] || {
        echo "FAIL Gum package '$actual_package' != '$gum_package'" >&2; return 1;
    }
    [ "$actual_arch" = "$gum_arch" ] || {
        echo "FAIL Gum architecture '$actual_arch' != '$gum_arch'" >&2; return 1;
    }
    [ "$actual_version" = 3.0.0-1 ] || {
        echo "FAIL Gum version '$actual_version' != '3.0.0-1'" >&2; return 1;
    }
    printf '%s\n' "$depends" | grep -Fq 'me.jjolano.fmwk.hookkit (= 3.0.0-1)' || {
        echo "FAIL Gum package must depend on the matching HookKit package" >&2; return 1;
    }
    printf '%s\n' "$depends" | grep -Fq 'firmware (>= 15.0)' || {
        echo "FAIL Gum package must require firmware >= 15.0" >&2; return 1;
    }
    for field in Recommends Conflicts Replaces Provides; do
        [ -z "$("$DPKG_DEB" -f "$GUM_ARTIFACT" "$field" 2>/dev/null || true)" ] || {
            echo "FAIL Gum package must not declare $field" >&2; return 1;
        }
    done
    [ -z "$(find "$gum_root" -path '*/HookKit.framework/*' -print)" ] || {
        echo "FAIL Gum package must not contain HookKit.framework" >&2; return 1;
    }
    hkgum_rel="${notice_prefix:+$notice_prefix/}usr/lib/HKGum.dylib"
    expected_path="$gum_root/$hkgum_rel"
    [ -f "$expected_path" ] || {
        echo "FAIL Gum package is missing $expected_path" >&2; return 1;
    }
    count=$(find "$gum_root" -name HKGum.dylib -type f -print | sed '/^$/d' | wc -l | tr -d ' ')
    [ "$count" = 1 ] || {
        echo "FAIL Gum package must contain exactly one HKGum.dylib" >&2; return 1;
    }
    actual_files=$(find "$gum_root" -type f -print | sed "s|^$gum_root/||" | sort)
    expected_files=$hkgum_rel
    [ "$actual_files" = "$expected_files" ] || {
        echo "FAIL Gum package must contain only HKGum.dylib" >&2; return 1;
    }
    echo "Gum package: $actual_package ($actual_arch), firmware >= 15.0"
    check_binary "$expected_path" "$gum_expected" "$gum_root"
    bash "$ROOT/tools/release/check_exports.sh" "$expected_path"
}

echo "Package: $actual_package ($actual_arch), firmware >= $floor"
check_binary "$HOOKKIT" "$hookkit_expected" "$tmp" "$hookkit_install_name"
bash "$ROOT/tools/release/check_exports.sh" "$HOOKKIT"
check_gum_package
echo "OK: $PROFILE artifacts have exactly the expected products, metadata, notices, slices, and exports"
