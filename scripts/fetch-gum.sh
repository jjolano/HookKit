#!/usr/bin/env bash
# Fetch the small, pinned set of Frida Gum build inputs kept out of Git.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GUM_DIR="$ROOT/vendor/gum"
LOCK="$GUM_DIR/gum.lock"

die() { echo "error: $*" >&2; exit 1; }

[[ $# -eq 0 || ( $# -eq 1 && $1 == --check ) ]] ||
    die "usage: $0 [--check]"
[[ -r "$LOCK" ]] || die "missing $LOCK"
# shellcheck disable=SC1090
. "$LOCK"

for variable in GUM_VERSION GUM_ARM64_URL GUM_ARM64_SHA256 \
                GUM_ARM64E_URL GUM_ARM64E_SHA256; do
    [[ -n ${!variable:-} ]] || die "$variable is unset in $LOCK"
done
[[ "$GUM_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] ||
    die "invalid GUM_VERSION in $LOCK"
[[ "$GUM_ARM64_SHA256" =~ ^[0-9a-f]{64}$ ]] ||
    die "invalid GUM_ARM64_SHA256 in $LOCK"
[[ "$GUM_ARM64E_SHA256" =~ ^[0-9a-f]{64}$ ]] ||
    die "invalid GUM_ARM64E_SHA256 in $LOCK"
[[ "$GUM_ARM64_URL" == "https://github.com/frida/frida/releases/download/$GUM_VERSION/frida-gum-devkit-$GUM_VERSION-ios-arm64.tar.xz" ]] ||
    die "unexpected arm64 URL in $LOCK"
[[ "$GUM_ARM64E_URL" == "https://github.com/frida/frida/releases/download/$GUM_VERSION/frida-gum-devkit-$GUM_VERSION-ios-arm64e.tar.xz" ]] ||
    die "unexpected arm64e URL in $LOCK"

if [[ ${1:-} == --check ]]; then
    exit 0
fi

STAMP="$GUM_DIR/.devkit-$GUM_VERSION"
STAMP_CONTENTS="$GUM_VERSION $GUM_ARM64_SHA256 $GUM_ARM64E_SHA256"
if [[ -f "$GUM_DIR/frida-gum.h" && -f "$GUM_DIR/libfrida-gum.a" &&
      -f "$STAMP" && $(<"$STAMP") == "$STAMP_CONTENTS" ]]; then
    exit 0
fi

command -v curl >/dev/null || die "curl is required to fetch Frida Gum"
command -v tar >/dev/null || die "tar with xz support is required"
command -v shasum >/dev/null || command -v sha256sum >/dev/null ||
    die "shasum or sha256sum is required"

find_lipo() {
    local candidate dir
    candidate=$(command -v lipo 2>/dev/null || true)
    [ -n "$candidate" ] && [ -x "$candidate" ] && { printf '%s\n' "$candidate"; return; }
    for dir in "${THEOS:-/nonexistent}"/toolchain/*/bin \
               "${THEOS:-/nonexistent}"/toolchain/*/*/bin \
               "${THEOS:-/nonexistent}"/toolchain/*/*/*/bin; do
        [ -x "$dir/lipo" ] && { printf '%s\n' "$dir/lipo"; return; }
    done
    return 1
}

LIPO=$(find_lipo) || die "Xcode's or Theos's lipo is required"

sha256() {
    if command -v shasum >/dev/null; then
        shasum -a 256 "$1" | awk '{print $1}'
    else
        sha256sum "$1" | awk '{print $1}'
    fi
}

TMP="$(mktemp -d "${TMPDIR:-/tmp}/hookkit-gum.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

fetch() {
    local arch=$1 url=$2 expected=$3 archive actual
    archive="$TMP/$arch.tar.xz"
    curl --fail --location --proto '=https' --retry 3 --silent --show-error \
        --output "$archive" "$url"
    actual="$(sha256 "$archive")"
    [[ "$actual" == "$expected" ]] ||
        die "$arch devkit SHA-256 mismatch: expected $expected, got $actual"
    mkdir -p "$TMP/$arch"
    tar -xJf "$archive" -C "$TMP/$arch"
}

find_devkit_file() {
    local arch=$1 name=$2 file
    file="$(find "$TMP/$arch" -type f -name "$name" -print -quit)"
    [[ -n "$file" ]] || die "$arch devkit does not contain $name"
    printf '%s\n' "$file"
}

fetch arm64 "$GUM_ARM64_URL" "$GUM_ARM64_SHA256"
fetch arm64e "$GUM_ARM64E_URL" "$GUM_ARM64E_SHA256"

ARM64_HEADER="$(find_devkit_file arm64 frida-gum.h)"
ARM64E_HEADER="$(find_devkit_file arm64e frida-gum.h)"
ARM64_LIBRARY="$(find_devkit_file arm64 libfrida-gum.a)"
ARM64E_LIBRARY="$(find_devkit_file arm64e libfrida-gum.a)"
cmp -s "$ARM64_HEADER" "$ARM64E_HEADER" ||
    die "Frida Gum headers differ between devkits"
"$LIPO" -create "$ARM64_LIBRARY" "$ARM64E_LIBRARY" -output "$TMP/libfrida-gum.a"

cp "$ARM64_HEADER" "$GUM_DIR/frida-gum.h.tmp"
mv "$GUM_DIR/frida-gum.h.tmp" "$GUM_DIR/frida-gum.h"
mv "$TMP/libfrida-gum.a" "$GUM_DIR/libfrida-gum.a"
printf '%s\n' "$STAMP_CONTENTS" > "$STAMP.tmp"
mv "$STAMP.tmp" "$STAMP"
