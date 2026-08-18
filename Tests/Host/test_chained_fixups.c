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
_Static_assert(offsetof(struct dyld_chained_starts_in_segment, size) == 0, "");
_Static_assert(offsetof(struct dyld_chained_starts_in_segment, page_size) == 4, "");
_Static_assert(offsetof(struct dyld_chained_starts_in_segment, pointer_format) == 6, "");
_Static_assert(offsetof(struct dyld_chained_starts_in_segment, segment_offset) == 8, "");
_Static_assert(offsetof(struct dyld_chained_starts_in_segment, max_valid_pointer) == 16, "");
_Static_assert(offsetof(struct dyld_chained_starts_in_segment, page_count) == 20, "");
_Static_assert(offsetof(struct dyld_chained_starts_in_segment, page_start) ==
               HK_CHAINED_STARTS_IN_SEGMENT_HEADER, "page_start[] follows the 22-byte header");
_Static_assert(DYLD_CHAINED_PTR_ARM64E == HK_CHAINED_PTR_ARM64E, "");
_Static_assert(DYLD_CHAINED_PTR_64 == HK_CHAINED_PTR_64, "");
_Static_assert(DYLD_CHAINED_PTR_64_OFFSET == HK_CHAINED_PTR_64_OFFSET, "");
_Static_assert(DYLD_CHAINED_PTR_ARM64E_USERLAND == HK_CHAINED_PTR_ARM64E_USERLAND, "");
_Static_assert(DYLD_CHAINED_PTR_ARM64E_USERLAND24 == HK_CHAINED_PTR_ARM64E_USERLAND24, "");
_Static_assert(DYLD_CHAINED_PTR_START_NONE == HK_CHAINED_PTR_START_NONE, "");
_Static_assert(DYLD_CHAINED_PTR_START_MULTI == HK_CHAINED_PTR_START_MULTI, "");
_Static_assert(DYLD_CHAINED_PTR_START_LAST == HK_CHAINED_PTR_START_LAST, "");
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


// ---- traversal fixtures -------------------------------------------------

static void put_u16(uint8_t *b, size_t off, uint16_t v) { memcpy(b + off, &v, sizeof(v)); }
static void put_u64f(uint8_t *b, size_t off, uint64_t v) { memcpy(b + off, &v, sizeof(v)); }

// Encoded through Apple's own bitfield struct, so the parser decodes bits it
// did not lay out -- the same cross-check the imports table gets.
static uint64_t arm64e_bind24(uint32_t ordinal, uint32_t next, bool auth) {
    if (auth) {
        struct dyld_chained_ptr_arm64e_auth_bind24 e;
        memset(&e, 0, sizeof(e));
        e.ordinal = ordinal; e.next = next; e.bind = 1; e.auth = 1;
        uint64_t raw; memcpy(&raw, &e, sizeof(raw)); return raw;
    }
    struct dyld_chained_ptr_arm64e_bind24 e;
    memset(&e, 0, sizeof(e));
    e.ordinal = ordinal; e.next = next; e.bind = 1; e.auth = 0;
    uint64_t raw; memcpy(&raw, &e, sizeof(raw)); return raw;
}

static uint64_t arm64e_rebase24(uint32_t next) {
    struct dyld_chained_ptr_arm64e_rebase e;
    memset(&e, 0, sizeof(e));
    e.target = 0x1234; e.next = next; e.bind = 0; e.auth = 0;
    uint64_t raw; memcpy(&raw, &e, sizeof(raw)); return raw;
}

static uint64_t ptr64_bind(uint32_t ordinal, uint32_t next) {
    struct dyld_chained_ptr_64_bind e;
    memset(&e, 0, sizeof(e));
    e.ordinal = ordinal; e.next = next; e.bind = 1;
    uint64_t raw; memcpy(&raw, &e, sizeof(raw)); return raw;
}

// Blob layout for traversal tests:
//   [0,28)   header (starts_offset 28)
//   [28,40)  starts_in_image: seg_count 2, seg_info_offset[0]=0, [1]=12
//   [40,70)  starts_in_segment (22-byte header + up to 4 uint16 starts)
//   [72,80)  2 imports
//   [80,94)  symbols "\0_alpha\0_beta"
#define T_STARTS_OFF    28u
#define T_SEG_OFF       40u
#define T_PAGESTART_OFF (T_SEG_OFF + HK_CHAINED_STARTS_IN_SEGMENT_HEADER)
// page_start[] gets room for 4 uint16 (one real page plus overflow entries),
// so the imports and symbols start after it rather than on top of it.
#define T_IMPORTS_OFF   72u
#define T_SYMBOLS_OFF   80u
#define T_BLOB_SIZE     94u

// The image the chains live in: one "segment" at 0x100, one 0x100-byte page.
#define T_IMG_SIZE   0x400u
#define T_SEGMENT    0x100u
#define T_PAGE_SIZE  0x100u

static void build_traversal_blob(uint8_t *b, uint16_t pointer_format,
                                 uint16_t page_start_value, uint16_t page_count) {
    memset(b, 0, T_BLOB_SIZE);
    put_u32(b, 0, 0);                 // fixups_version
    put_u32(b, 4, T_STARTS_OFF);
    put_u32(b, 8, T_IMPORTS_OFF);
    put_u32(b, 12, T_SYMBOLS_OFF);
    put_u32(b, 16, 2);                // imports_count
    put_u32(b, 20, HK_CHAINED_IMPORT);
    put_u32(b, 24, 0);                // symbols_format

    put_u32(b, T_STARTS_OFF + 0, 2);  // seg_count
    put_u32(b, T_STARTS_OFF + 4, 0);  // segment 0: no fixups
    put_u32(b, T_STARTS_OFF + 8, T_SEG_OFF - T_STARTS_OFF);  // segment 1

    put_u32(b, T_SEG_OFF + 0, HK_CHAINED_STARTS_IN_SEGMENT_HEADER + 2u * page_count);
    put_u16(b, T_SEG_OFF + 4, T_PAGE_SIZE);
    put_u16(b, T_SEG_OFF + 6, pointer_format);
    put_u64f(b, T_SEG_OFF + 8, T_SEGMENT);
    put_u16(b, T_SEG_OFF + 20, page_count);
    put_u16(b, T_PAGESTART_OFF, page_start_value);

    put_u32(b, T_IMPORTS_OFF + 0, encode_import(1, false, 1));  // "_alpha"
    put_u32(b, T_IMPORTS_OFF + 4, encode_import(1, false, 8));  // "_beta"
    memcpy(b + T_SYMBOLS_OFF, "\0_alpha\0_beta", 14);
}

// Chain: bind(ordinal 0) -> rebase -> bind(ordinal 1, auth). Offsets 0x110,
// 0x118, 0x120 with stride 8, so `next` is 1 between each.
static void build_traversal_image(uint8_t *img) {
    memset(img, 0, T_IMG_SIZE);
    put_u64f(img, 0x110, arm64e_bind24(0, 1, false));
    put_u64f(img, 0x118, arm64e_rebase24(1));
    put_u64f(img, 0x120, arm64e_bind24(1, 0, true));
}

typedef struct {
    hk_chained_bind_t binds[8];
    size_t n;
    size_t stop_after;
} bind_collect_t;

static bool collect_bind(void *ctx, const hk_chained_bind_t *bind) {
    bind_collect_t *c = (bind_collect_t *)ctx;
    if (c->n < 8) { c->binds[c->n] = *bind; }
    c->n++;
    return !(c->stop_after && c->n >= c->stop_after);
}

// ---- traversal tests ----------------------------------------------------

static void test_walks_chain_and_skips_rebases(void) {
    _Alignas(8) uint8_t blob[T_BLOB_SIZE];
    _Alignas(8) uint8_t img[T_IMG_SIZE];
    build_traversal_blob(blob, HK_CHAINED_PTR_ARM64E_USERLAND24, 0x10, 1);
    build_traversal_image(img);

    hk_chained_fixups_t fixups;
    assert(hk_chained_fixups_parse(blob, sizeof(blob), &fixups) == HK_CHAINED_OK);

    bind_collect_t c = {{{0}}, 0, 0};
    assert(hk_chained_fixups_iterate_binds(&fixups, img, sizeof(img), collect_bind, &c)
           == HK_CHAINED_OK);

    // Three chain entries, but the middle one is a rebase and carries no symbol.
    assert(c.n == 2);
    assert(c.binds[0].slot_image_offset == 0x110);
    assert(c.binds[0].import_ordinal == 0);
    assert(!c.binds[0].is_auth);
    assert(c.binds[0].segment_index == 1);  // segment 0 had no fixups
    assert(c.binds[1].slot_image_offset == 0x120);
    assert(c.binds[1].import_ordinal == 1);
    assert(c.binds[1].is_auth);
    printf("  walks-chain-and-skips-rebases: PASS\n");
}

static void test_binds_join_to_the_imports_table(void) {
    // The point of the whole feature: a bind's ordinal names an import, which
    // names a symbol. This is the join the older LC_DYSYMTAB path does via the
    // indirect symbol table.
    _Alignas(8) uint8_t blob[T_BLOB_SIZE];
    _Alignas(8) uint8_t img[T_IMG_SIZE];
    build_traversal_blob(blob, HK_CHAINED_PTR_ARM64E_USERLAND24, 0x10, 1);
    build_traversal_image(img);

    hk_chained_fixups_t fixups;
    assert(hk_chained_fixups_parse(blob, sizeof(blob), &fixups) == HK_CHAINED_OK);
    bind_collect_t c = {{{0}}, 0, 0};
    assert(hk_chained_fixups_iterate_binds(&fixups, img, sizeof(img), collect_bind, &c)
           == HK_CHAINED_OK);
    assert(c.n == 2);

    hk_chained_import_t imp;
    assert(hk_chained_import_at(&fixups, c.binds[0].import_ordinal, &imp) == HK_CHAINED_OK);
    assert(strcmp(imp.symbol_name, "_alpha") == 0);
    assert(hk_chained_import_at(&fixups, c.binds[1].import_ordinal, &imp) == HK_CHAINED_OK);
    assert(strcmp(imp.symbol_name, "_beta") == 0);
    printf("  binds-join-to-the-imports-table: PASS\n");
}

static void test_ptr64_format_has_its_own_bit_layout(void) {
    // DYLD_CHAINED_PTR_64 puts bind at bit 63 and next at 51-62 (12 bits),
    // with stride 4 -- decoding it with the arm64e layout would misread both.
    _Alignas(8) uint8_t blob[T_BLOB_SIZE];
    _Alignas(8) uint8_t img[T_IMG_SIZE];
    build_traversal_blob(blob, HK_CHAINED_PTR_64, 0x10, 1);
    memset(img, 0, sizeof(img));
    put_u64f(img, 0x110, ptr64_bind(1, 4));   // stride 4 -> next 4 == +16 bytes
    put_u64f(img, 0x120, ptr64_bind(0, 0));

    hk_chained_fixups_t fixups;
    assert(hk_chained_fixups_parse(blob, sizeof(blob), &fixups) == HK_CHAINED_OK);
    bind_collect_t c = {{{0}}, 0, 0};
    assert(hk_chained_fixups_iterate_binds(&fixups, img, sizeof(img), collect_bind, &c)
           == HK_CHAINED_OK);
    assert(c.n == 2);
    assert(c.binds[0].slot_image_offset == 0x110 && c.binds[0].import_ordinal == 1);
    assert(c.binds[1].slot_image_offset == 0x120 && c.binds[1].import_ordinal == 0);
    assert(!c.binds[0].is_auth);  // PTR_64 has no auth bit
    printf("  ptr64-format-has-its-own-bit-layout: PASS\n");
}

static void test_page_start_none_and_early_stop(void) {
    _Alignas(8) uint8_t blob[T_BLOB_SIZE];
    _Alignas(8) uint8_t img[T_IMG_SIZE];
    build_traversal_image(img);
    hk_chained_fixups_t fixups;

    // A page marked NONE has no fixups and must be skipped entirely.
    build_traversal_blob(blob, HK_CHAINED_PTR_ARM64E_USERLAND24,
                         HK_CHAINED_PTR_START_NONE, 1);
    assert(hk_chained_fixups_parse(blob, sizeof(blob), &fixups) == HK_CHAINED_OK);
    bind_collect_t c = {{{0}}, 0, 0};
    assert(hk_chained_fixups_iterate_binds(&fixups, img, sizeof(img), collect_bind, &c)
           == HK_CHAINED_OK);
    assert(c.n == 0);

    // The visitor can stop the walk early.
    build_traversal_blob(blob, HK_CHAINED_PTR_ARM64E_USERLAND24, 0x10, 1);
    assert(hk_chained_fixups_parse(blob, sizeof(blob), &fixups) == HK_CHAINED_OK);
    bind_collect_t s = {{{0}}, 0, 1};
    assert(hk_chained_fixups_iterate_binds(&fixups, img, sizeof(img), collect_bind, &s)
           == HK_CHAINED_OK);
    assert(s.n == 1);
    printf("  page-start-none-and-early-stop: PASS\n");
}

static void test_start_multi_overflow_list(void) {
    // A page with several chain starts: page_start holds MULTI|index, and the
    // entries from there on are chain starts, the last flagged with LAST.
    // page_count 3 gives three uint16 slots: [0] the MULTI marker, [1] and [2]
    // the overflow list itself.
    _Alignas(8) uint8_t blob[T_BLOB_SIZE];
    _Alignas(8) uint8_t img[T_IMG_SIZE];
    build_traversal_blob(blob, HK_CHAINED_PTR_ARM64E_USERLAND24,
                         (uint16_t)(HK_CHAINED_PTR_START_MULTI | 1u), 1);
    // ONE real page, with the overflow entries at indices 1 and 2 -- past
    // page_count, inside the segment struct's declared size. Bounding the
    // list by page_count instead of `size` would reject this, which is the
    // bug this test caught.
    put_u32(blob, T_SEG_OFF, HK_CHAINED_STARTS_IN_SEGMENT_HEADER + 3u * 2u);
    put_u16(blob, T_PAGESTART_OFF + 2, 0x10);                                 // chain at 0x110
    put_u16(blob, T_PAGESTART_OFF + 4, (uint16_t)(0x20 | HK_CHAINED_PTR_START_LAST));
    memset(img, 0, sizeof(img));
    put_u64f(img, 0x110, arm64e_bind24(0, 0, false));  // first start
    put_u64f(img, 0x120, arm64e_bind24(1, 0, false));  // second start

    hk_chained_fixups_t fixups;
    assert(hk_chained_fixups_parse(blob, sizeof(blob), &fixups) == HK_CHAINED_OK);
    bind_collect_t c = {{{0}}, 0, 0};
    assert(hk_chained_fixups_iterate_binds(&fixups, img, sizeof(img), collect_bind, &c)
           == HK_CHAINED_OK);
    assert(c.n == 2);
    assert(c.binds[0].slot_image_offset == 0x110 && c.binds[0].import_ordinal == 0);
    assert(c.binds[1].slot_image_offset == 0x120 && c.binds[1].import_ordinal == 1);
    printf("  start-multi-overflow-list: PASS\n");
}

static void test_unsupported_pointer_format_is_rejected(void) {
    // Rejected BEFORE walking: decoding with the wrong bit layout would
    // silently produce nonsense rather than fail.
    _Alignas(8) uint8_t blob[T_BLOB_SIZE];
    _Alignas(8) uint8_t img[T_IMG_SIZE];
    build_traversal_image(img);
    hk_chained_fixups_t fixups;
    bind_collect_t c = {{{0}}, 0, 0};

    build_traversal_blob(blob, 3 /* DYLD_CHAINED_PTR_32 */, 0x10, 1);
    assert(hk_chained_fixups_parse(blob, sizeof(blob), &fixups) == HK_CHAINED_OK);
    assert(hk_chained_fixups_iterate_binds(&fixups, img, sizeof(img), collect_bind, &c)
           == HK_CHAINED_UNSUPPORTED_FORMAT);

    build_traversal_blob(blob, 13 /* ARM64E_SHARED_CACHE */, 0x10, 1);
    assert(hk_chained_fixups_parse(blob, sizeof(blob), &fixups) == HK_CHAINED_OK);
    assert(hk_chained_fixups_iterate_binds(&fixups, img, sizeof(img), collect_bind, &c)
           == HK_CHAINED_UNSUPPORTED_FORMAT);
    printf("  unsupported-pointer-format-is-rejected: PASS\n");
}

static void test_chain_bounds_are_enforced(void) {
    hk_chained_fixups_t fixups;
    bind_collect_t c;

    // A `next` that walks the chain past the segment's declared span.
    _Alignas(8) uint8_t blob[T_BLOB_SIZE];
    _Alignas(8) uint8_t img[T_IMG_SIZE];
    build_traversal_blob(blob, HK_CHAINED_PTR_ARM64E_USERLAND24, 0x10, 1);
    memset(img, 0, sizeof(img));
    // Segment is [0x100,0x200); next=0x7FF*8 lands far beyond it.
    put_u64f(img, 0x110, arm64e_bind24(0, 0x7FF, false));
    assert(hk_chained_fixups_parse(blob, sizeof(blob), &fixups) == HK_CHAINED_OK);
    c = (bind_collect_t){{{0}}, 0, 0};
    assert(hk_chained_fixups_iterate_binds(&fixups, img, sizeof(img), collect_bind, &c)
           == HK_CHAINED_MALFORMED);
    assert(c.n == 1);  // the first bind was reported before the overrun showed

    // A start offset already outside the segment.
    build_traversal_blob(blob, HK_CHAINED_PTR_ARM64E_USERLAND24, 0xF0, 1);
    put_u16(blob, T_SEG_OFF + 4, 0x40);  // page_size 0x40 -> segment [0x100,0x140)
    put_u64f(img, 0x1F0, arm64e_bind24(0, 0, false));
    assert(hk_chained_fixups_parse(blob, sizeof(blob), &fixups) == HK_CHAINED_OK);
    c = (bind_collect_t){{{0}}, 0, 0};
    assert(hk_chained_fixups_iterate_binds(&fixups, img, sizeof(img), collect_bind, &c)
           == HK_CHAINED_MALFORMED);

    // An image too small to hold the chain, even though the segment allows it.
    build_traversal_blob(blob, HK_CHAINED_PTR_ARM64E_USERLAND24, 0x10, 1);
    assert(hk_chained_fixups_parse(blob, sizeof(blob), &fixups) == HK_CHAINED_OK);
    c = (bind_collect_t){{{0}}, 0, 0};
    assert(hk_chained_fixups_iterate_binds(&fixups, img, 0x118, collect_bind, &c)
           == HK_CHAINED_MALFORMED);
    printf("  chain-bounds-are-enforced: PASS\n");
}

static void test_malformed_starts_structures(void) {
    _Alignas(8) uint8_t blob[T_BLOB_SIZE];
    _Alignas(8) uint8_t img[T_IMG_SIZE];
    build_traversal_image(img);
    hk_chained_fixups_t fixups;
    bind_collect_t c = {{{0}}, 0, 0};

    // page_size 0 would make every page start resolve to the same address.
    build_traversal_blob(blob, HK_CHAINED_PTR_ARM64E_USERLAND24, 0x10, 1);
    put_u16(blob, T_SEG_OFF + 4, 0);
    assert(hk_chained_fixups_parse(blob, sizeof(blob), &fixups) == HK_CHAINED_OK);
    assert(hk_chained_fixups_iterate_binds(&fixups, img, sizeof(img), collect_bind, &c)
           == HK_CHAINED_MALFORMED);

    // A page_start[] array larger than the blob can hold.
    build_traversal_blob(blob, HK_CHAINED_PTR_ARM64E_USERLAND24, 0x10, 1);
    put_u16(blob, T_SEG_OFF + 20, 0xFFFF);
    assert(hk_chained_fixups_parse(blob, sizeof(blob), &fixups) == HK_CHAINED_OK);
    assert(hk_chained_fixups_iterate_binds(&fixups, img, sizeof(img), collect_bind, &c)
           == HK_CHAINED_MALFORMED);

    // A seg_count claiming more entries than the blob holds.
    build_traversal_blob(blob, HK_CHAINED_PTR_ARM64E_USERLAND24, 0x10, 1);
    put_u32(blob, T_STARTS_OFF, 0xFFFFFFFFu);
    assert(hk_chained_fixups_parse(blob, sizeof(blob), &fixups) == HK_CHAINED_OK);
    assert(hk_chained_fixups_iterate_binds(&fixups, img, sizeof(img), collect_bind, &c)
           == HK_CHAINED_MALFORMED);

    // starts_offset past the blob.
    build_traversal_blob(blob, HK_CHAINED_PTR_ARM64E_USERLAND24, 0x10, 1);
    put_u32(blob, 4, T_BLOB_SIZE);
    assert(hk_chained_fixups_parse(blob, sizeof(blob), &fixups) == HK_CHAINED_OK);
    assert(hk_chained_fixups_iterate_binds(&fixups, img, sizeof(img), collect_bind, &c)
           == HK_CHAINED_MALFORMED);

    // A seg_info_offset pointing past the blob.
    build_traversal_blob(blob, HK_CHAINED_PTR_ARM64E_USERLAND24, 0x10, 1);
    put_u32(blob, T_STARTS_OFF + 8, T_BLOB_SIZE);
    assert(hk_chained_fixups_parse(blob, sizeof(blob), &fixups) == HK_CHAINED_OK);
    assert(hk_chained_fixups_iterate_binds(&fixups, img, sizeof(img), collect_bind, &c)
           == HK_CHAINED_MALFORMED);
    printf("  malformed-starts-structures: PASS\n");
}

static void test_traversal_null_arguments(void) {
    _Alignas(8) uint8_t blob[T_BLOB_SIZE];
    _Alignas(8) uint8_t img[T_IMG_SIZE];
    build_traversal_blob(blob, HK_CHAINED_PTR_ARM64E_USERLAND24, 0x10, 1);
    build_traversal_image(img);
    hk_chained_fixups_t fixups;
    assert(hk_chained_fixups_parse(blob, sizeof(blob), &fixups) == HK_CHAINED_OK);
    bind_collect_t c = {{{0}}, 0, 0};

    assert(hk_chained_fixups_iterate_binds(NULL, img, sizeof(img), collect_bind, &c)
           == HK_CHAINED_INVALID_ARGUMENT);
    assert(hk_chained_fixups_iterate_binds(&fixups, NULL, sizeof(img), collect_bind, &c)
           == HK_CHAINED_INVALID_ARGUMENT);
    assert(hk_chained_fixups_iterate_binds(&fixups, img, sizeof(img), NULL, &c)
           == HK_CHAINED_INVALID_ARGUMENT);
    printf("  traversal-null-arguments: PASS\n");
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
    test_walks_chain_and_skips_rebases();
    test_binds_join_to_the_imports_table();
    test_ptr64_format_has_its_own_bit_layout();
    test_page_start_none_and_early_stop();
    test_start_multi_overflow_list();
    test_unsupported_pointer_format_is_rejected();
    test_chain_bounds_are_enforced();
    test_malformed_starts_structures();
    test_traversal_null_arguments();
    printf("all chained fixups tests passed\n");
    return 0;
}
