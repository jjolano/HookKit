#!/usr/bin/env bash
# Check the pinned Dobby archive and source patch. --rebuild writes only an
# ignored candidate archive under .theos/; replacing vendor/dobby/libdobby.a
# remains a deliberate review step.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LOCK="$ROOT/vendor/dobby/dobby.lock"
PATCH="$ROOT/vendor/dobby/patches/0001-publish-original-before-activation.patch"
ARCHIVE="$ROOT/vendor/dobby/libdobby.a"
MODE="${1:---check}"

die() { echo "error: $*" >&2; exit 1; }
[[ "$MODE" == --check || "$MODE" == --rebuild ]] ||
    die "usage: $0 [--check|--rebuild]"
[[ -r "$LOCK" && -r "$PATCH" && -r "$ARCHIVE" ]] ||
    die "missing Dobby lock, patch, or archive"
# shellcheck disable=SC1090
. "$LOCK"

for variable in DOBBY_COMMIT DOBBY_SOURCE_URL DOBBY_SOURCE_SHA256 \
                DOBBY_ARCHIVE_SHA256 DOBBY_ARM64_MIN DOBBY_ARM64E_MIN \
                DOBBY_MEMBER_COUNT; do
    [[ -n ${!variable:-} ]] || die "$variable is unset in $LOCK"
done
[[ "$DOBBY_COMMIT" =~ ^[0-9a-f]{40}$ ]] || die "invalid DOBBY_COMMIT"
[[ "$DOBBY_SOURCE_SHA256" =~ ^[0-9a-f]{64}$ ]] || die "invalid source SHA-256"
[[ "$DOBBY_ARCHIVE_SHA256" =~ ^[0-9a-f]{64}$ ]] || die "invalid archive SHA-256"
[[ "$DOBBY_SOURCE_URL" == "https://codeload.github.com/jmpews/Dobby/tar.gz/$DOBBY_COMMIT" ]] ||
    die "unexpected DOBBY_SOURCE_URL"

find_tool() {
    local name candidate dir
    for name in "$@"; do
        for dir in "${THEOS:-/nonexistent}"/toolchain/*/bin \
                   "${THEOS:-/nonexistent}"/toolchain/*/*/bin \
                   "${THEOS:-/nonexistent}"/toolchain/*/*/*/bin; do
            [[ -x "$dir/$name" ]] && { echo "$dir/$name"; return; }
        done
        candidate="$(command -v "$name" 2>/dev/null || true)"
        [[ -n "$candidate" && -x "$candidate" ]] && { echo "$candidate"; return; }
    done
    return 1
}

SHA256="$(find_tool shasum sha256sum)" || die "shasum or sha256sum is required"
LIPO="$(find_tool lipo llvm-lipo)" || die "lipo is required"
AR="$(find_tool ar llvm-ar)" || die "a Mach-O-aware ar is required"
NM="$(find_tool nm llvm-nm)" || die "a Mach-O-aware nm is required"
VTOOL="$(find_tool vtool)" || die "vtool is required"

sha256() {
    case "$(basename "$SHA256")" in
        shasum) "$SHA256" -a 256 "$1" | awk '{print $1}' ;;
        *) "$SHA256" "$1" | awk '{print $1}' ;;
    esac
}

TMP="$(mktemp -d "${TMPDIR:-/tmp}/hookkit-dobby.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

check_shape() {
    local archive=$1 arch archs thin members member object minimum actual subtype exports
    archs="$("$LIPO" -archs "$archive")"
    [[ "$(echo "$archs" | wc -w | tr -d ' ')" == 2 &&
       " $archs " == *" arm64 "* && " $archs " == *" arm64e "* ]] ||
        die "$archive must contain arm64 and arm64e"
    for arch in arm64 arm64e; do
        thin="$TMP/$arch.a"
        "$LIPO" -thin "$arch" "$archive" -output "$thin"
        members="$("$AR" -t "$thin" | grep -v '^__.SYMDEF' | sed '/^$/d' | wc -l | tr -d ' ')"
        [[ "$members" == "$DOBBY_MEMBER_COUNT" ]] ||
            die "$arch member count $members != $DOBBY_MEMBER_COUNT"
        member="$("$AR" -t "$thin" | grep -v '^__.SYMDEF' | sed -n '1p')"
        [[ -n "$member" ]] || die "$arch archive has no object members"
        mkdir -p "$TMP/$arch"
        (
            cd "$TMP/$arch"
            "$AR" -x "$thin" "$member"
        )
        object="$(find "$TMP/$arch" -type f -print -quit)"
        [[ -n "$object" ]] || die "could not extract an $arch object"
        if [[ "$arch" == arm64 ]]; then minimum="$DOBBY_ARM64_MIN"; else minimum="$DOBBY_ARM64E_MIN"; fi
        actual="$("$VTOOL" -show-build "$object" | awk '$1 == "minos" { print $2; exit }')"
        [[ "$actual" == "$minimum" ]] || die "$arch minimum iOS $actual != $minimum"
    done
    subtype="$(od -An -tx1 -j 11 -N 1 "$object" | tr -d '[:space:]')"
    [[ "$subtype" == 80 ]] || die "arm64e archive does not use the arm64e ABI"
    exports="$("$NM" -gU -arch arm64 "$archive" | awk '{print $NF}')"
    for symbol in _DobbyHook _DobbyDestroy _DobbyCodePatch _DobbyInstrument _DobbySymbolResolver; do
        echo "$exports" | grep -Fxq "$symbol" ||
            die "$archive is missing $symbol"
    done
}

check_archive() {
    local actual
    actual="$(sha256 "$ARCHIVE")"
    [[ "$actual" == "$DOBBY_ARCHIVE_SHA256" ]] ||
        die "archive SHA-256 $actual != $DOBBY_ARCHIVE_SHA256"
    check_shape "$ARCHIVE"
}

fetch_source() {
    local downloaded actual
    command -v curl >/dev/null || die "curl is required to verify source"
    command -v tar >/dev/null || die "tar is required to verify source"
    command -v git >/dev/null || die "git is required to verify the patch"
    downloaded="$TMP/dobby.tar.gz"
    curl --fail --location --proto '=https' --retry 3 --silent --show-error \
        --output "$downloaded" "$DOBBY_SOURCE_URL"
    actual="$(sha256 "$downloaded")"
    [[ "$actual" == "$DOBBY_SOURCE_SHA256" ]] ||
        die "source SHA-256 $actual != $DOBBY_SOURCE_SHA256"
    mkdir -p "$TMP/source"
    tar -xzf "$downloaded" -C "$TMP/source"
    SOURCE_DIR="$(find "$TMP/source" -mindepth 1 -maxdepth 1 -type d -print -quit)"
    [[ -n "$SOURCE_DIR" ]] || die "source archive did not contain a directory"
    git -C "$SOURCE_DIR" init -q
    git -C "$SOURCE_DIR" apply --check "$PATCH"
}

check_archive
fetch_source
echo "OK: Dobby archive and patch match $DOBBY_COMMIT"

if [[ "$MODE" == --check ]]; then
    exit 0
fi

[[ -n ${DOBBY_SDK:-} && -d ${DOBBY_SDK:-} ]] ||
    die "set DOBBY_SDK to the iPhoneOS SDK directory"
[[ -n ${DOBBY_TOOLCHAIN:-} && -x ${DOBBY_TOOLCHAIN:-}/clang ]] ||
    die "set DOBBY_TOOLCHAIN to the Theos iPhone toolchain bin directory"
git -C "$SOURCE_DIR" apply "$PATCH"

build_one() {
    local arch=$1 min=$2 build="$TMP/build-$1"
    cmake -S "$SOURCE_DIR" -B "$build" \
        -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_SYSTEM_PROCESSOR=arm64 \
        -DCMAKE_OSX_ARCHITECTURES="$arch" -DCMAKE_OSX_SYSROOT="$DOBBY_SDK" \
        -DCMAKE_C_COMPILER="$DOBBY_TOOLCHAIN/clang" \
        -DCMAKE_CXX_COMPILER="$DOBBY_TOOLCHAIN/clang++" \
        -DCMAKE_ASM_COMPILER="$DOBBY_TOOLCHAIN/clang" \
        -DCMAKE_AR="$DOBBY_TOOLCHAIN/ar" -DCMAKE_RANLIB="$DOBBY_TOOLCHAIN/ranlib" \
        -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY -DCMAKE_BUILD_TYPE=Release \
        -DDOBBY_BUILD_EXAMPLE=OFF -DDOBBY_BUILD_TEST=OFF \
        "-DCMAKE_C_FLAGS=-target $arch-apple-ios$min -isysroot $DOBBY_SDK" \
        "-DCMAKE_CXX_FLAGS=-target $arch-apple-ios$min -isysroot $DOBBY_SDK" \
        "-DCMAKE_ASM_FLAGS=-target $arch-apple-ios$min -isysroot $DOBBY_SDK -x assembler-with-cpp"
    cmake --build "$build" --target dobby_static -j8
}

command -v cmake >/dev/null || die "cmake is required to rebuild Dobby"
build_one arm64 "$DOBBY_ARM64_MIN"
build_one arm64e "$DOBBY_ARM64E_MIN"
REBUILT="$TMP/libdobby.a"
"$LIPO" -create "$TMP/build-arm64/libdobby.a" "$TMP/build-arm64e/libdobby.a" \
    -output "$REBUILT"
check_shape "$REBUILT"
mkdir -p "$ROOT/.theos/dobby-rebuild"
cp "$REBUILT" "$ROOT/.theos/dobby-rebuild/libdobby.a"
echo "Candidate: .theos/dobby-rebuild/libdobby.a ($(sha256 "$REBUILT"))"
echo "Review and update dobby.lock before replacing vendor/dobby/libdobby.a."
