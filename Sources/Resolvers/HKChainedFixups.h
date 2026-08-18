// Chained fixups -- Milestone 5. The MODERN import mechanism (iOS 15+),
// which the LC_DYSYMTAB indirect-symbol path in HKImportSlots.h does not
// cover: on a current image that path finds nothing, so without this,
// import resolution silently misses.
//
// This file is the METADATA half: the LC_DYLD_CHAINED_FIXUPS header, the
// imports table, and the symbol string pool -- i.e. "what does this image
// import, by name". Walking the fixup chains to find *which slot* holds each
// bind is the traversal half, deliberately a separate piece: it needs
// per-pointer-format decoding and cycle guards of its own, and splitting
// keeps each testable. Nothing here is device-specific.
//
// Reuse survey: vendor/litehook/fixup-chains.h vendors Apple's own
// definitions and includes only <stdint.h>, so it builds on this host. It is
// used as the authoritative CROSS-CHECK -- Tests/Host/test_chained_fixups.c
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
