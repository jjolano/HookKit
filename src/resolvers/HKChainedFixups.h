// Chained fixups -- Milestone 5. The MODERN import mechanism (iOS 15+),
// which the LC_DYSYMTAB indirect-symbol path in HKImportSlots.h does not
// cover: on a current image that path finds nothing, so without this,
// import resolution silently misses.
//
// Two halves, built in two commits and both here now:
//   METADATA   the payload header, imports table and symbol pool -- "what
//              does this image import, by name and ordinal".
//   TRAVERSAL  walking starts_in_image -> starts_in_segment -> page_start[]
//              -> the fixup chains, to find WHICH SLOT holds each bind.
// Joined, they answer the same question HKImportSlots.h answers for the older
// LC_DYSYMTAB mechanism. Nothing here is device-specific.
//
// Reuse survey: vendor/litehook/fixup-chains.h vendors Apple's own
// definitions and includes only <stdint.h>, so it builds on this host. It is
// used as the authoritative CROSS-CHECK -- tests/host/test_chained_fixups.c
// includes it and static_asserts this file's constants and offsets against
// Apple's enums and structs. The decoding here uses explicit shifts and masks
// rather than those bitfield structs, because bitfield layout is an ABI
// detail and a parser should not depend on the host compiler agreeing with
// the target's; the cross-check is what keeps the two honest.

#ifndef HK_RESOLVERS_CHAINED_FIXUPS_H
#define HK_RESOLVERS_CHAINED_FIXUPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "HKSymbolResolve.h"  // hk_symbol_candidates_t: one normalization rule
#include "HKSymbolTable.h"    // hk_symbol_name_convention_t

#ifdef __cplusplus
extern "C" {
#endif

// dyld_chained_fixups_header: seven uint32 fields.
#define HK_CHAINED_HEADER_SIZE 28u

// imports_format values.
#define HK_CHAINED_IMPORT           1u
#define HK_CHAINED_IMPORT_ADDEND    2u
#define HK_CHAINED_IMPORT_ADDEND64  3u

// Per-entry sizes for the three formats above.
#define HK_CHAINED_IMPORT_SIZE           4u
#define HK_CHAINED_IMPORT_ADDEND_SIZE    8u
#define HK_CHAINED_IMPORT_ADDEND64_SIZE 16u

// symbols_format: 0 uncompressed, 1 zlib. Only uncompressed is supported.
#define HK_CHAINED_SYMBOLS_UNCOMPRESSED 0u

// pointer_format values. Only the arm64 userland formats are decoded; every
// other value gets HK_CHAINED_UNSUPPORTED_FORMAT rather than being misread
// with the wrong bit layout.
#define HK_CHAINED_PTR_ARM64E             1u
#define HK_CHAINED_PTR_64                 2u
#define HK_CHAINED_PTR_64_OFFSET          6u
#define HK_CHAINED_PTR_ARM64E_USERLAND    9u
#define HK_CHAINED_PTR_ARM64E_USERLAND24 12u

// page_start[] sentinels.
#define HK_CHAINED_PTR_START_NONE  0xFFFFu  // page has no fixups
#define HK_CHAINED_PTR_START_MULTI 0x8000u  // page_start is an index into the overflow list
#define HK_CHAINED_PTR_START_LAST  0x8000u  // marks the last entry of an overflow list

// On-disk sizes, asserted against Apple's structs in the test.
#define HK_CHAINED_STARTS_IN_SEGMENT_HEADER 22u  // through page_count, before page_start[]

typedef enum {
    HK_CHAINED_OK = 0,
    HK_CHAINED_NOT_FOUND,
    HK_CHAINED_INVALID_ARGUMENT,
    HK_CHAINED_MALFORMED,
    HK_CHAINED_UNSUPPORTED_VERSION,  // fixups_version this parser does not know
    HK_CHAINED_UNSUPPORTED_FORMAT,   // unknown imports_format, or zlib-compressed symbols
    HK_CHAINED_NAME_TOO_LONG,
} hk_chained_status_t;

// A parsed and range-validated LC_DYLD_CHAINED_FIXUPS payload. Borrows the
// blob; nothing is owned.
typedef struct {
    const uint8_t *blob;
    size_t size;

    uint32_t fixups_version;
    uint32_t starts_offset;    // where the traversal half will begin
    uint32_t imports_offset;
    uint32_t symbols_offset;
    uint32_t imports_count;
    uint32_t imports_format;
    uint32_t symbols_format;

    uint32_t import_entry_size;  // derived from imports_format
    size_t symbols_size;         // pool runs from symbols_offset to the blob's end
} hk_chained_fixups_t;

typedef struct {
    uint32_t index;
    // Signed on purpose: dyld documents lib_ordinal as "-15 .. 240", i.e. the
    // top of the unsigned range encodes the special BIND_SPECIAL_DYLIB_*
    // ordinals as negatives. Reading it unsigned would turn "this image
    // itself" into 254.
    int32_t lib_ordinal;
    bool weak_import;
    const char *symbol_name;  // borrowed from the pool, NUL-terminated within it
    int64_t addend;           // 0 for the plain HK_CHAINED_IMPORT format
} hk_chained_import_t;

// Parses and validates the header, checking every declared offset and the
// imports table's extent against `size`.
hk_chained_status_t hk_chained_fixups_parse(const void *blob, size_t size,
                                            hk_chained_fixups_t *out_fixups);

// Reads one import. The name is bounds-checked and must be NUL-terminated
// inside the symbol pool.
hk_chained_status_t hk_chained_import_at(const hk_chained_fixups_t *fixups,
                                         uint32_t index,
                                         hk_chained_import_t *out_import);

// One bind site found by walking the chains.
typedef struct {
    // Offset of the pointer slot FROM THE IMAGE BASE. Deliberately not called
    // a vmaddr: hk_import_slot_t.slot_vmaddr is an unslid VM address from a
    // section header, which is a different coordinate system. Conflating the
    // two would be a real bug, so they are named apart.
    uint64_t slot_image_offset;
    uint32_t import_ordinal;   // index into the imports table
    uint32_t segment_index;
    uint16_t pointer_format;
    bool is_auth;              // arm64e authenticated bind
    uint8_t key;               // 0=IA, 1=IB, 2=DA, 3=DB
    uint16_t diversity;
    bool address_diversity;
    int64_t addend;            // addend encoded in the pointer word
} hk_chained_bind_t;

// Translation for one LC_SEGMENT_64 when chain words are read from the
// original file rather than dyld's already-fixed live mapping.
typedef struct {
    uint64_t image_offset;
    uint64_t file_offset;
    uint64_t file_size;
} hk_chained_segment_mapping_t;

typedef bool (*hk_chained_bind_visit_fn)(void *ctx, const hk_chained_bind_t *bind);

// Walks starts_in_image -> starts_in_segment -> page_start[] -> the chains,
// visiting every BIND site (rebases are skipped: they carry no symbol).
//
// `image_base`/`image_size` are where the chain data lives: a segment's
// declared `segment_offset` is an offset from the image base, so this is the
// LOADED layout. A file-on-disk image would need its VM offsets translated to
// file offsets first; that is not done here rather than guessed at.
//
// Termination is structural, not merely guarded: `next` counts stride units
// and `next == 0` ends the chain, so any nonzero `next` advances by at least
// one stride. The offset therefore strictly increases and is bounded, so the
// walk cannot cycle the way an export trie can. The explicit progress check in
// the implementation is defense in depth against a zero stride -- which is the
// one way this could loop, and is why unknown formats are rejected before any
// walking begins.
hk_chained_status_t hk_chained_fixups_iterate_binds(const hk_chained_fixups_t *fixups,
                                                    const void *image_base,
                                                    size_t image_size,
                                                    hk_chained_bind_visit_fn visit,
                                                    void *ctx);

hk_chained_status_t hk_chained_fixups_iterate_file_binds(
    const hk_chained_fixups_t *fixups, const void *file_base, size_t file_size,
    const hk_chained_segment_mapping_t *segments, uint32_t segment_count,
    hk_chained_bind_visit_fn visit, void *ctx);

// Finds the first import whose name matches, using the SAME linker-form
// candidate expansion as hk_resolve_symbol and hk_import_slots_find
// (hk_symbol_build_candidates), so the normalization rule still lives in one
// place.
hk_chained_status_t hk_chained_imports_find(const hk_chained_fixups_t *fixups,
                                            const char *name,
                                            hk_symbol_name_convention_t convention,
                                            hk_chained_import_t *out_import);

#ifdef __cplusplus
}
#endif

#endif // HK_RESOLVERS_CHAINED_FIXUPS_H
