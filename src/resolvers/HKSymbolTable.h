// Mach-O symbol table (LC_SYMTAB / nlist) search -- Milestone 5's resolver
// work. Underpins the private-symbol resolver: the symbol table is how
// non-exported symbols are found. (Exported symbols also appear here, but
// the export trie is the proper path for those -- a separate resolver, not
// yet built.)
//
// This is a PURE function over a caller-supplied table view. It deliberately
// knows nothing about dyld, mmap, files, the shared cache, PAC, or the slide:
//   - the caller supplies the nlist array + string table (on device, from a
//     mapped image's LC_SYMTAB; in host tests, from a synthetic buffer),
//   - the result is the symbol's UNSLID n_value plus its n_sect,
//   - applying the slide and any PAC signing is the caller's job (n_sect is
//     returned precisely so the device side can look up section flags to
//     decide whether the address is code).
// That split makes the search host-testable. The device-side reader supplies
// a view here instead of coupling table walking to mmap/dlsym/PAC/slide.
//
// Robustness is a requirement, not a nicety: a symbol table can be malformed
// (a truncated string table, an unterminated final string, an out-of-range
// n_strx). Every read below is bounds-checked against the caller-declared
// sizes; the search never reads past them, even for a hostile table.

#ifndef HK_RESOLVERS_SYMBOL_TABLE_H
#define HK_RESOLVERS_SYMBOL_TABLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../../include/HookKit/HookKitTargets.h"  // conventions, visibility

#ifdef __cplusplus
extern "C" {
#endif

// Mach-O's nlist_64, declared here rather than included from
// <mach-o/nlist.h> so this compiles on a non-Apple host. The layout is fixed
// Mach-O ABI; tests/host/test_symbol_table.c static_asserts size and every
// offset so a mismatch with Apple's struct can never go unnoticed. (Apple
// wraps n_strx in a one-member union `n_un`; that has no layout effect.)
typedef struct {
    uint32_t n_strx;
    uint8_t  n_type;
    uint8_t  n_sect;
    uint16_t n_desc;
    uint64_t n_value;
} hk_macho_nlist64_t;

// n_type field masks/values (Mach-O <mach-o/nlist.h>).
#define HK_N_STAB 0xe0u  // any bit set => a debug (STAB) entry, not a symbol
#define HK_N_PEXT 0x10u  // private external
#define HK_N_TYPE 0x0eu  // mask for the type bits below
#define HK_N_EXT  0x01u  // external (exported) symbol

#define HK_N_UNDF 0x0u   // undefined
#define HK_N_ABS  0x2u   // absolute
#define HK_N_SECT 0xeu   // defined in the section given by n_sect
#define HK_N_PBUD 0xcu   // prebound undefined
#define HK_N_INDR 0xau   // indirect

// A borrowed view of one image's symbol table. Nothing here is owned.
typedef struct {
    const hk_macho_nlist64_t *nlist;
    uint32_t nlist_count;
    const char *strings;
    size_t strings_size;
} hk_symbol_table_view_t;

typedef struct {
    const char *name;                        // never NULL for a valid query
    hk_symbol_name_convention_t convention;
    hk_symbol_visibility_t visibility;
} hk_symbol_query_t;

typedef struct {
    uint64_t n_value;   // UNSLID address; caller adds the image slide
    uint8_t n_sect;     // 1-based section index, for the caller's code/data check
    uint8_t n_type;     // raw type byte, so the caller can inspect N_EXT etc.
    uint32_t index;     // index of the matching entry, for diagnostics
} hk_symbol_match_t;

// Finds the first entry (in table order) whose name matches `query` under its
// convention and passes the visibility filter. Returns false if there is no
// match, or on any invalid argument.
//
// Documented, tested rules:
//   - Entries with any HK_N_STAB bit set are debug records and are always
//     rejected (their fields do not mean what a symbol's do).
//   - Only HK_N_SECT entries with a non-zero n_value are candidates: a symbol
//     that is undefined, absolute, or has no address cannot be a hook target.
//   - HK_SYMBOL_VISIBILITY_EXPORTED_ONLY requires the HK_N_EXT bit; ANY and
//     PRIVATE_ALLOWED accept both external and local symbols.
//
//     ANY and PRIVATE_ALLOWED still behave identically *here*, and that is
//     correct rather than a gap. An export-trie resolver now exists
//     (HKExportTrie.h), but the difference between those two visibilities is
//     about WHICH SOURCE to consult and in what order -- prefer the export
//     trie, fall back to the symbol table, or go straight to the symbol table
//     because private symbols are explicitly acceptable. That is a
//     resolver-selection decision belonging to the layer above both
//     resolvers. Within a symbol-table search on
//     its own there is genuinely nothing for the two to differ about: the
//     symbol table is a private-symbol source by nature.
//   - Name matching: HK_SYMBOL_NAME_MACHO_EXACT compares the table string
//     byte-for-byte. Every other convention (C, C++ mangled, Swift mangled)
//     also accepts the table's leading-underscore form, honoring the ABI's
//     "leading underscore normalized internally -- pass either form".
//   - "First in table order" is the rule when several entries match; it is
//     deterministic and tested, not incidental.
//
// NOT implemented, and deliberately not guessed at: hk_symbol_alias_policy_t
// (HK_SYMBOL_ALIAS_FOLLOW would mean resolving through to other names at the
// same address, which needs a second pass and a defined preference order) and
// hk_symbol_target_t.interior_address_permitted. Both are absent rather than
// approximated; a caller's alias policy is simply not consulted yet.
bool hk_symbol_table_find(const hk_symbol_table_view_t *view,
                          const hk_symbol_query_t *query,
                          hk_symbol_match_t *out_match);

#ifdef __cplusplus
}
#endif

#endif // HK_RESOLVERS_SYMBOL_TABLE_H
