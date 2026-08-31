// Resolver selection -- Milestone 5. The single place that decides HOW a
// symbol is looked up, sitting above the two mechanisms:
//   HKExportTrie.h   -- dyld's authoritative list of EXPORTED symbols
//   HKSymbolTable.h  -- the nlist table, where PRIVATE symbols live
//
// Two commits deferred to this layer by name; this is it. It owns exactly the
// two decisions neither mechanism can make alone:
//
// 1. NAME NORMALIZATION, in one place. Mach-O stores linker-form names, so C
//    `malloc` appears as `_malloc`. The trie walker is exact-name-only by
//    design, and HKSymbolTable has an equivalent rule built in. Rather than
//    have two implementations of one rule, this layer expands a query into an
//    ordered candidate list -- the name as given, then the underscore-prefixed
//    form -- and queries BOTH sources with HK_SYMBOL_NAME_MACHO_EXACT, which
//    switches off HKSymbolTable's internal normalization. The result is
//    identical to what HKSymbolTable did alone (an entry matches if it equals
//    the query, or equals "_" + query), but the rule now exists once.
//    HKSymbolTable keeps its internal handling for standalone use.
//
// 2. SOURCE PREFERENCE, which is what finally makes hk_symbol_visibility_t
//    mean something. Until now ANY and PRIVATE_ALLOWED behaved identically
//    because a symbol-table search alone has nothing to differ about. With two
//    sources they genuinely differ:
//
//      EXPORTED_ONLY     the export trie is dyld's authority on what is
//                        exported, so if a trie exists it is the ONLY source
//                        consulted -- a symbol absent from it is not exported,
//                        and falling back would contradict that. Images with
//                        no trie fall back to the symbol table filtered to
//                        N_EXT, which is the best available answer.
//      ANY               trie first (authoritative, and exports are the common
//                        case), then the symbol table, which also covers
//                        private symbols.
//      PRIVATE_ALLOWED   symbol table first, since that is the only place
//                        private symbols exist; then the trie, so an exported
//                        symbol stripped from the symbol table is still found.
//                        "Private allowed" permits private symbols, it does
//                        not require them.
//
// Everything here is pure logic over caller-supplied sources, so it is fully
// host-testable; only obtaining a real image is device-only.

#ifndef HK_RESOLVERS_SYMBOL_RESOLVE_H
#define HK_RESOLVERS_SYMBOL_RESOLVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "HKExportTrie.h"
#include "HKMachO.h"
#include "HKSymbolTable.h"

#ifdef __cplusplus
extern "C" {
#endif

// Longest symbol name accepted. Only the underscore-prefixed candidate needs
// buffering, so this bounds that copy; a longer name is refused with a
// distinct status rather than silently truncated.
#define HK_RESOLVE_MAX_NAME 1024u

typedef enum {
    HK_RESOLVE_OK = 0,
    HK_RESOLVE_NOT_FOUND,
    HK_RESOLVE_INVALID_ARGUMENT,
    HK_RESOLVE_NAME_TOO_LONG,
    HK_RESOLVE_MALFORMED_IMAGE,
    HK_RESOLVE_UNSUPPORTED,   // found, but carries no address (a re-export)
} hk_resolve_status_t;

typedef enum {
    HK_RESOLVE_SOURCE_NONE = 0,
    HK_RESOLVE_SOURCE_EXPORT_TRIE,
    HK_RESOLVE_SOURCE_SYMBOL_TABLE,
} hk_resolve_source_t;

// The linker-form candidate expansion described above, exposed so other
// resolvers (HKImportSlots.h) apply the SAME rule rather than reimplementing
// it. Ordered: the name as given, then the underscore-prefixed form.
typedef struct {
    const char *names[2];
    unsigned count;
    char storage[HK_RESOLVE_MAX_NAME + 2];  // '_' + name + NUL
} hk_symbol_candidates_t;

hk_resolve_status_t hk_symbol_build_candidates(const char *name,
                                               hk_symbol_name_convention_t convention,
                                               hk_symbol_candidates_t *out_candidates);

// Where one image's symbols can be found. Either source may be absent
// (`export_trie == NULL`, or `symbol_table.nlist == NULL`); a resolve simply
// skips what is not there.
//
// The two mechanisms report addresses against different bases -- a trie gives
// an offset from the mach header, an nlist gives an unslid VM address -- so
// both anchors are carried here and the resolver returns a single comparable
// runtime address:
//     trie   -> header_address + offset
//     nlist  -> n_value + slide
// For an image mapped as a plain file buffer, setting header_address to the
// buffer and slide to (buffer - __TEXT.vmaddr) makes both agree; the
// constructors below do that.
typedef struct {
    hk_symbol_table_view_t symbol_table;
    const uint8_t *export_trie;
    size_t export_trie_size;
    uintptr_t header_address;
    uintptr_t slide;
} hk_symbol_sources_t;

typedef struct {
    uintptr_t address;            // runtime address, comparable across sources
    hk_resolve_source_t source;   // which source actually answered
    uint64_t raw_value;           // trie offset or unslid n_value, as found
    uint8_t n_sect;               // symbol table only; 0 otherwise
    uint64_t export_flags;        // export trie only; 0 otherwise
    bool is_weak;
} hk_symbol_resolution_t;

// Collects both sources from an image laid out as on disk. A missing source
// is not an error; an image with neither yields sources that resolve nothing.
hk_resolve_status_t hk_symbol_sources_from_file_image(const void *image, size_t size,
                                                      hk_symbol_sources_t *out_sources);

// Same for a loaded image, translating both tables through __LINKEDIT.
// `header_region_size` bounds only the load-command parsing.
hk_resolve_status_t hk_symbol_sources_from_loaded_image(const void *header,
                                                        size_t header_region_size,
                                                        uintptr_t slide,
                                                        hk_symbol_sources_t *out_sources);

// Resolves from a loaded image's normal Mach-O sources and, on Apple devices,
// its exact dyld shared-cache local-symbol range. The latter is needed because
// cache private symbols live outside the mapped image's LC_SYMTAB payload.
hk_resolve_status_t hk_resolve_loaded_image_symbol(
    const void *header, size_t header_region_size, uintptr_t slide,
    const char *name, hk_symbol_name_convention_t convention,
    hk_symbol_visibility_t visibility, hk_symbol_resolution_t *out_resolution);

// Resolves `name` under `convention` and `visibility`, per the preference
// rules documented at the top of this file. Returns HK_RESOLVE_UNSUPPORTED if
// the match is a re-export (it names another dylib rather than carrying an
// address); `out_resolution` still records what was found, with address 0.
hk_resolve_status_t hk_resolve_symbol(const hk_symbol_sources_t *sources,
                                      const char *name,
                                      hk_symbol_name_convention_t convention,
                                      hk_symbol_visibility_t visibility,
                                      hk_symbol_resolution_t *out_resolution);

#ifdef __cplusplus
}
#endif

#endif // HK_RESOLVERS_SYMBOL_RESOLVE_H
