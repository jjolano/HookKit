#!/usr/bin/env bash
# Build the fixed Mach-O fixtures the CI conformance step runs the resolver
# harness over. Checked in as a script, not as binaries: the fixtures are
# Apple-targeted Mach-O files a Linux git checkout should not carry, and
# rebuilding them from two tiny C sources needs only the Theos cross clang
# (already required for every lane build).
#
# Fixtures:
#   hkhello.dylib   minimal dylib exporting hk_hello_add (export-trie +
#                   symtab resolve path, file and loaded layouts must agree)
#   hkhello2.dylib  dylib importing printf (import-slot path: >=1 slot, and
#                   the export-trie resolve of the local symbol still works)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-$ROOT/.theos/conformance-fixtures}"
TC="${THEOS:?THEOS must point to Theos}/toolchain/linux/iphone/bin"
SDK="$THEOS/sdks/iPhoneOS16.5.sdk"

mkdir -p "$OUT"
"$TC/clang" -target arm64-apple-ios15.0 -isysroot "$SDK" \
    -dynamiclib -o "$OUT/hkhello.dylib" "$ROOT/tools/conformance/fixtures/hkhello.c"
"$TC/clang" -target arm64-apple-ios15.0 -isysroot "$SDK" \
    -dynamiclib -o "$OUT/hkhello2.dylib" "$ROOT/tools/conformance/fixtures/hkhello2.c"
printf '%s\n' "$OUT/hkhello.dylib" "$OUT/hkhello2.dylib"
