// Host test for Sources/Resolvers/HKChainedFixups.c -- the metadata half of
// the modern (iOS 15+) import mechanism, against synthetic payloads.
//
// This file does something the other resolver tests could not: it includes
// Apple's own vendored definitions (vendor/litehook/fixup-chains.h, which
// needs only <stdint.h> and so builds here) and cross-checks the parser
// against them -- both the structure sizes/offsets and, more usefully, the
// BIT LAYOUTS: values are built through Apple's bitfield structs and then
// decoded with the parser's shifts and masks, which must agree.

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../Sources/Resolvers/HKChainedFixups.h"
#include "../../Sources/Resolvers/HKMachO.h"
#include "../../vendor/litehook/fixup-chains.h"

// ---- cross-check against Apple's definitions ----------------------------

_Static_assert(sizeof(struct dyld_chained_fixups_header) == HK_CHAINED_HEADER_SIZE,
               "header size must match Apple's struct");
_Static_assert(offsetof(struct dyld_chained_fixups_header, fixups_version) == 0, "");
_Static_assert(offsetof(struct dyld_chained_fixups_header, starts_offset) == 4, "");
_Static_assert(offsetof(struct dyld_chained_fixups_header, imports_offset) == 8, "");
_Static_assert(offsetof(struct dyld_chained_fixups_header, symbols_offset) == 12, "");
_Static_assert(offsetof(struct dyld_chained_fixups_header, imports_count) == 16, "");
_Static_assert(offsetof(struct dyld_chained_fixups_header, imports_format) == 20, "");
_Static_assert(offsetof(struct dyld_chained_fixups_header, symbols_format) == 24, "");

_Static_assert(DYLD_CHAINED_IMPORT == HK_CHAINED_IMPORT, "");
_Static_assert(DYLD_CHAINED_IMPORT_ADDEND == HK_CHAINED_IMPORT_ADDEND, "");
_Static_assert(DYLD_CHAINED_IMPORT_ADDEND64 == HK_CHAINED_IMPORT_ADDEND64, "");

_Static_assert(sizeof(struct dyld_chained_import) == HK_CHAINED_IMPORT_SIZE, "");
_Static_assert(sizeof(struct dyld_chained_import_addend) == HK_CHAINED_IMPORT_ADDEND_SIZE, "");
_Static_assert(sizeof(struct dyld_chained_import_addend64) == HK_CHAINED_IMPORT_ADDEND64_SIZE, "");

// ---- payload building ---------------------------------------------------

static void put_u32(uint8_t *b, size_t off, uint32_t v) { memcpy(b + off, &v, sizeof(v)); }

// Encodes a DYLD_CHAINED_IMPORT word the way the linker does, via Apple's own
// bitfield struct -- so the parser is decoding a value it did not itself lay
// out. A disagreement between the two is exactly what this catches.
static uint32_t encode_import(uint8_t lib_ordinal, bool weak, uint32_t name_offset) {
    struct dyld_chained_import imp;
    memset(&imp, 0, sizeof(imp));
    imp.lib_ordinal = lib_ordinal;
    imp.weak_import = weak ? 1u : 0u;
    imp.name_offset = name_offset;
    uint32_t word;
    memcpy(&word, &imp, sizeof(word));
    return word;
}

//   [0,28)   header
//   [28,40)  3 imports (DYLD_CHAINED_IMPORT, 4 bytes each)
//   [40,61)  symbol pool "\0_malloc\0_free\0_weak"
#define IMPORTS_OFF 28u
#define SYMBOLS_OFF 40u
#define BLOB_SIZE   61u
#define NAME_MALLOC 1u
#define NAME_FREE   9u
#define NAME_WEAK   15u

static void write_header(uint8_t *b, uint32_t imports_count, uint32_t imports_format,
                         uint32_t symbols_off, uint32_t version, uint32_t symbols_format) {
    put_u32(b, 0, version);
    put_u32(b, 4, 0);              // starts_offset (traversal half; unused here)
    put_u32(b, 8, IMPORTS_OFF);
    put_u32(b, 12, symbols_off);
    put_u32(b, 16, imports_count);
    put_u32(b, 20, imports_format);
    put_u32(b, 24, symbols_format);
}

static void build_blob(uint8_t *b) {
    memset(b, 0, BLOB_SIZE);
    write_header(b, 3, HK_CHAINED_IMPORT, SYMBOLS_OFF, 0, HK_CHAINED_SYMBOLS_UNCOMPRESSED);
    put_u32(b, IMPORTS_OFF + 0, encode_import(1, false, NAME_MALLOC));
    put_u32(b, IMPORTS_OFF + 4, encode_import(2, false, NAME_FREE));
    // 0xFE is BIND_SPECIAL_DYLIB_FLAT_LOOKUP (-2) once sign-extended.
    put_u32(b, IMPORTS_OFF + 8, encode_import(0xFE, true, NAME_WEAK));
    memcpy(b + SYMBOLS_OFF, "\0_malloc\0_free\0_weak", 21);
}

static void verify_pool_offsets(void) {
    static const char pool[] = "\0_malloc\0_free\0_weak";
    assert(sizeof(pool) == 21);
    assert(strcmp(pool + NAME_MALLOC, "_malloc") == 0);
    assert(strcmp(pool + NAME_FREE, "_free") == 0);
    assert(strcmp(pool + NAME_WEAK, "_weak") == 0);
    assert(SYMBOLS_OFF + sizeof(pool) == BLOB_SIZE);
}

// ---- tests --------------------------------------------------------------

static void test_bitfield_layout_agrees_with_apple(void) {
    // The load-bearing cross-check: build through Apple's bitfields, decode
    // with the parser. If the shift/mask transcription were wrong, the parsed
    // ordinal/weak/name would not round-trip.
    _Alignas(8) uint8_t blob[BLOB_SIZE];
    build_blob(blob);

    hk_chained_fixups_t fixups;
    assert(hk_chained_fixups_parse(blob, sizeof(blob), &fixups) == HK_CHAINED_OK);

    hk_chained_import_t imp;
    assert(hk_chained_import_at(&fixups, 0, &imp) == HK_CHAINED_OK);
    assert(imp.lib_ordinal == 1 && !imp.weak_import);
    assert(strcmp(imp.symbol_name, "_malloc") == 0);
    printf("  bitfield-layout-agrees-with-apple: PASS\n");
}

static void test_parses_header_and_imports(void) {
    _Alignas(8) uint8_t blob[BLOB_SIZE];
    build_blob(blob);

    hk_chained_fixups_t fixups;
    assert(hk_chained_fixups_parse(blob, sizeof(blob), &fixups) == HK_CHAINED_OK);
    assert(fixups.imports_count == 3);
    assert(fixups.imports_format == HK_CHAINED_IMPORT);
    assert(fixups.import_entry_size == HK_CHAINED_IMPORT_SIZE);
    assert(fixups.symbols_size == BLOB_SIZE - SYMBOLS_OFF);

    hk_chained_import_t imp;
    assert(hk_chained_import_at(&fixups, 1, &imp) == HK_CHAINED_OK);
    assert(imp.index == 1 && imp.lib_ordinal == 2);
    assert(strcmp(imp.symbol_name, "_free") == 0);
    assert(imp.addend == 0);  // the plain format carries none

    assert(hk_chained_import_at(&fixups, 3, &imp) == HK_CHAINED_NOT_FOUND);
    printf("  parses-header-and-imports: PASS\n");
}

static void test_lib_ordinal_is_sign_extended(void) {
    // dyld documents lib_ordinal as "-15 .. 240": the top of the unsigned
    // range encodes the special BIND_SPECIAL_DYLIB_* ordinals as negatives.
    // Reading it unsigned would report 254 instead of -2.
    _Alignas(8) uint8_t blob[BLOB_SIZE];
    build_blob(blob);
    hk_chained_fixups_t fixups;
    assert(hk_chained_fixups_parse(blob, sizeof(blob), &fixups) == HK_CHAINED_OK);

    hk_chained_import_t imp;
    assert(hk_chained_import_at(&fixups, 2, &imp) == HK_CHAINED_OK);
    assert(imp.lib_ordinal == -2);
    assert(imp.lib_ordinal != 254);
    assert(imp.weak_import);
    assert(strcmp(imp.symbol_name, "_weak") == 0);
    printf("  lib-ordinal-is-sign-extended: PASS\n");
}

static void test_addend_formats(void) {
    // DYLD_CHAINED_IMPORT_ADDEND: 4-byte word + signed 32-bit addend.
    _Alignas(8) uint8_t b8[64];
    memset(b8, 0, sizeof(b8));
    write_header(b8, 1, HK_CHAINED_IMPORT_ADDEND, 36, 0, 0);
    put_u32(b8, IMPORTS_OFF, encode_import(3, false, 1));
    int32_t addend32 = -4096;
    memcpy(b8 + IMPORTS_OFF + 4, &addend32, sizeof(addend32));
    memcpy(b8 + 36, "\0_sym", 6);

    hk_chained_fixups_t fixups;
    assert(hk_chained_fixups_parse(b8, 42, &fixups) == HK_CHAINED_OK);
    assert(fixups.import_entry_size == HK_CHAINED_IMPORT_ADDEND_SIZE);
    hk_chained_import_t imp;
    assert(hk_chained_import_at(&fixups, 0, &imp) == HK_CHAINED_OK);
    assert(imp.lib_ordinal == 3 && imp.addend == -4096);
    assert(strcmp(imp.symbol_name, "_sym") == 0);

    // DYLD_CHAINED_IMPORT_ADDEND64: 16-bit ordinal, 32-bit name offset,
    // 64-bit addend. Built through Apple's struct again.
    _Alignas(8) uint8_t b64[80];
    memset(b64, 0, sizeof(b64));
    write_header(b64, 1, HK_CHAINED_IMPORT_ADDEND64, 44, 0, 0);
    struct dyld_chained_import_addend64 e64;
    memset(&e64, 0, sizeof(e64));
    e64.lib_ordinal = 0xFFF1;  // -15 once sign-extended
    e64.weak_import = 1;
    e64.name_offset = 1;
    e64.addend = 0x1122334455667788ull;
    memcpy(b64 + IMPORTS_OFF, &e64, sizeof(e64));
    memcpy(b64 + 44, "\0_big", 6);

    assert(hk_chained_fixups_parse(b64, 50, &fixups) == HK_CHAINED_OK);
    assert(fixups.import_entry_size == HK_CHAINED_IMPORT_ADDEND64_SIZE);
    assert(hk_chained_import_at(&fixups, 0, &imp) == HK_CHAINED_OK);
    assert(imp.lib_ordinal == -15);
    assert(imp.weak_import);
    assert((uint64_t)imp.addend == 0x1122334455667788ull);
    assert(strcmp(imp.symbol_name, "_big") == 0);
    printf("  addend-formats: PASS\n");
}

static void test_find_uses_shared_normalization(void) {
    _Alignas(8) uint8_t blob[BLOB_SIZE];
    build_blob(blob);
    hk_chained_fixups_t fixups;
    assert(hk_chained_fixups_parse(blob, sizeof(blob), &fixups) == HK_CHAINED_OK);

    hk_chained_import_t imp;
    // Bare C name finds the stored linker form -- same rule as every other
    // resolver, from hk_symbol_build_candidates.
    assert(hk_chained_imports_find(&fixups, "malloc", HK_SYMBOL_NAME_C, &imp) == HK_CHAINED_OK);
    assert(imp.index == 0);
    assert(hk_chained_imports_find(&fixups, "_free", HK_SYMBOL_NAME_C, &imp) == HK_CHAINED_OK);
    assert(imp.index == 1);
    // Exact mode does not add the underscore.
    assert(hk_chained_imports_find(&fixups, "malloc", HK_SYMBOL_NAME_MACHO_EXACT, &imp)
           == HK_CHAINED_NOT_FOUND);
    assert(hk_chained_imports_find(&fixups, "_nosuch", HK_SYMBOL_NAME_C, &imp)
           == HK_CHAINED_NOT_FOUND);
    printf("  find-uses-shared-normalization: PASS\n");
}

static void test_unsupported_variants_are_distinct(void) {
    // Each unsupported thing gets its own status rather than a generic
    // failure, so a caller can tell "I cannot read this" from "this is broken".
    _Alignas(8) uint8_t blob[BLOB_SIZE];
    hk_chained_fixups_t fixups;

    build_blob(blob);
    put_u32(blob, 0, 1);  // fixups_version 1
    assert(hk_chained_fixups_parse(blob, sizeof(blob), &fixups) == HK_CHAINED_UNSUPPORTED_VERSION);

    build_blob(blob);
    put_u32(blob, 24, 1);  // zlib-compressed symbol pool
    assert(hk_chained_fixups_parse(blob, sizeof(blob), &fixups) == HK_CHAINED_UNSUPPORTED_FORMAT);

    build_blob(blob);
    put_u32(blob, 20, 99);  // unknown imports_format
    assert(hk_chained_fixups_parse(blob, sizeof(blob), &fixups) == HK_CHAINED_UNSUPPORTED_FORMAT);

    // An unknown format with ZERO imports is harmless: there is nothing to
    // decode, so it parses rather than failing on a field nobody will read.
    build_blob(blob);
    put_u32(blob, 20, 99);
    put_u32(blob, 16, 0);
    assert(hk_chained_fixups_parse(blob, sizeof(blob), &fixups) == HK_CHAINED_OK);
    printf("  unsupported-variants-are-distinct: PASS\n");
}

static void test_malformed_payloads(void) {
    _Alignas(8) uint8_t blob[BLOB_SIZE];
    hk_chained_fixups_t fixups;

    // Shorter than the header.
    build_blob(blob);
    assert(hk_chained_fixups_parse(blob, HK_CHAINED_HEADER_SIZE - 1, &fixups) == HK_CHAINED_MALFORMED);

    // Offsets past the blob.
    build_blob(blob);
    put_u32(blob, 8, BLOB_SIZE + 4);   // imports_offset
    assert(hk_chained_fixups_parse(blob, sizeof(blob), &fixups) == HK_CHAINED_MALFORMED);
    build_blob(blob);
    put_u32(blob, 12, BLOB_SIZE + 4);  // symbols_offset
    assert(hk_chained_fixups_parse(blob, sizeof(blob), &fixups) == HK_CHAINED_MALFORMED);
    build_blob(blob);
    put_u32(blob, 4, BLOB_SIZE + 4);   // starts_offset
    assert(hk_chained_fixups_parse(blob, sizeof(blob), &fixups) == HK_CHAINED_MALFORMED);

    // An imports table running past the blob (count computed in 64 bits so a
    // large count cannot wrap).
    build_blob(blob);
    put_u32(blob, 16, 0xFFFFFFFFu);
    assert(hk_chained_fixups_parse(blob, sizeof(blob), &fixups) == HK_CHAINED_MALFORMED);
    printf("  malformed-payloads: PASS\n");
}

static void test_name_offsets_are_bounded_and_terminated(void) {
    // Exact-length heap buffers so a missing bound is an ASan-visible read.
    hk_chained_fixups_t fixups;
    hk_chained_import_t imp;

    uint8_t *blob = (uint8_t *)malloc(BLOB_SIZE);
    assert(blob != NULL);

    // name_offset past the end of the pool.
    build_blob(blob);
    put_u32(blob, IMPORTS_OFF, encode_import(1, false, 9999));
    assert(hk_chained_fixups_parse(blob, BLOB_SIZE, &fixups) == HK_CHAINED_OK);
    assert(hk_chained_import_at(&fixups, 0, &imp) == HK_CHAINED_MALFORMED);

    // A pool with no terminator anywhere: no name can be read from it.
    build_blob(blob);
    memset(blob + SYMBOLS_OFF, 'x', BLOB_SIZE - SYMBOLS_OFF);
    assert(hk_chained_fixups_parse(blob, BLOB_SIZE, &fixups) == HK_CHAINED_OK);
    assert(hk_chained_import_at(&fixups, 0, &imp) == HK_CHAINED_MALFORMED);

    // find() reports the malformed entry rather than skipping past it.
    assert(hk_chained_imports_find(&fixups, "malloc", HK_SYMBOL_NAME_C, &imp)
           == HK_CHAINED_MALFORMED);

    free(blob);
    printf("  name-offsets-are-bounded-and-terminated: PASS\n");
}

static void test_null_arguments(void) {
    _Alignas(8) uint8_t blob[BLOB_SIZE];
    build_blob(blob);
    hk_chained_fixups_t fixups;
    hk_chained_import_t imp;

    assert(hk_chained_fixups_parse(NULL, BLOB_SIZE, &fixups) == HK_CHAINED_INVALID_ARGUMENT);
    assert(hk_chained_fixups_parse(blob, BLOB_SIZE, NULL) == HK_CHAINED_INVALID_ARGUMENT);
    assert(hk_chained_fixups_parse(blob, BLOB_SIZE, &fixups) == HK_CHAINED_OK);
    assert(hk_chained_import_at(NULL, 0, &imp) == HK_CHAINED_INVALID_ARGUMENT);
    assert(hk_chained_import_at(&fixups, 0, NULL) == HK_CHAINED_INVALID_ARGUMENT);
    assert(hk_chained_imports_find(NULL, "x", HK_SYMBOL_NAME_C, &imp) == HK_CHAINED_INVALID_ARGUMENT);
    assert(hk_chained_imports_find(&fixups, NULL, HK_SYMBOL_NAME_C, &imp) == HK_CHAINED_INVALID_ARGUMENT);
    assert(hk_chained_imports_find(&fixups, "x", HK_SYMBOL_NAME_C, NULL) == HK_CHAINED_INVALID_ARGUMENT);
    printf("  null-arguments: PASS\n");
}

static void test_locates_payload_in_an_image(void) {
    // End to end: find LC_DYLD_CHAINED_FIXUPS in an image, then parse it.
    _Alignas(8) uint8_t img[256];
    memset(img, 0, sizeof(img));
    put_u32(img, 0, HK_MH_MAGIC_64);
    put_u32(img, 16, 1);
    put_u32(img, 20, HK_LINKEDIT_DATA_CMD_SIZE);
    put_u32(img, 32, HK_LC_DYLD_CHAINED_FIXUPS);
    put_u32(img, 36, HK_LINKEDIT_DATA_CMD_SIZE);
    put_u32(img, 40, 64);            // dataoff
    put_u32(img, 44, BLOB_SIZE);     // datasize
    build_blob(img + 64);

    size_t off = 0, len = 0;
    assert(hk_macho_find_chained_fixups(img, sizeof(img), &off, &len) == HK_MACHO_OK);
    assert(off == 64 && len == BLOB_SIZE);

    hk_chained_fixups_t fixups;
    assert(hk_chained_fixups_parse(img + off, len, &fixups) == HK_CHAINED_OK);
    hk_chained_import_t imp;
    assert(hk_chained_imports_find(&fixups, "malloc", HK_SYMBOL_NAME_C, &imp) == HK_CHAINED_OK);
    assert(strcmp(imp.symbol_name, "_malloc") == 0);

    // An image without the command reports absence, not failure -- that is
    // simply an older image using the LC_DYSYMTAB mechanism instead.
    _Alignas(8) uint8_t bare[64];
    memset(bare, 0, sizeof(bare));
    put_u32(bare, 0, HK_MH_MAGIC_64);
    assert(hk_macho_find_chained_fixups(bare, sizeof(bare), &off, &len) == HK_MACHO_NOT_FOUND);

    // A payload range running past the image is malformed.
    put_u32(img, 44, 9999);
    assert(hk_macho_find_chained_fixups(img, sizeof(img), &off, &len) == HK_MACHO_MALFORMED);
    printf("  locates-payload-in-an-image: PASS\n");
}

int main(void) {
    verify_pool_offsets();
    test_bitfield_layout_agrees_with_apple();
    test_parses_header_and_imports();
    test_lib_ordinal_is_sign_extended();
    test_addend_formats();
    test_find_uses_shared_normalization();
    test_unsupported_variants_are_distinct();
    test_malformed_payloads();
    test_name_offsets_are_bounded_and_terminated();
    test_null_arguments();
    test_locates_payload_in_an_image();
    printf("all chained fixups tests passed\n");
    return 0;
}
