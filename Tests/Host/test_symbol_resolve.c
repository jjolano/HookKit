// Host test for Sources/Resolvers/HKSymbolResolve.c -- the resolver-selection
// layer. Pure logic over caller-supplied sources, so all of it runs here.
//
// The centerpiece is test_visibility_selects_source: the same name is present
// in BOTH sources with DIFFERENT addresses, so the resolved address reveals
// which source actually answered. Without that, a test could not tell a real
// preference order from two sources that happen to agree.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../Sources/Resolvers/HKSymbolResolve.h"

#define HDR_ADDR ((uintptr_t)0x10000)
#define SLIDE    ((uintptr_t)0x20000)

// ---- symbol table fixture ----------------------------------------------

// "\0_in_both\0_only_local\0_malloc\0__hidden\0_only_symtab\0malloc\0"
static const char k_strings[] =
    "\0_in_both\0_only_local\0_malloc\0__hidden\0_only_symtab\0malloc";
#define OFF_IN_BOTH     1u
#define OFF_ONLY_LOCAL  10u
#define OFF_MALLOC_U    22u   // "_malloc"
#define OFF_HIDDEN_UU   30u   // "__hidden"
#define OFF_ONLY_SYMTAB 39u
#define OFF_MALLOC_BARE 52u   // "malloc", no underscore

static hk_macho_nlist64_t k_nlist[] = {
    {OFF_IN_BOTH,     HK_N_SECT | HK_N_EXT, 1, 0, 0x5000},
    {OFF_ONLY_LOCAL,  HK_N_SECT,            1, 0, 0x6000},  // local: no N_EXT
    {OFF_MALLOC_U,    HK_N_SECT | HK_N_EXT, 1, 0, 0x7000},
    {OFF_HIDDEN_UU,   HK_N_SECT | HK_N_EXT, 1, 0, 0x8000},
    {OFF_ONLY_SYMTAB, HK_N_SECT | HK_N_EXT, 1, 0, 0x9000},
    {OFF_MALLOC_BARE, HK_N_SECT | HK_N_EXT, 1, 0, 0xA000},
};

static void verify_string_table_offsets(void) {
    // Guards every other test here: if the literal is edited, the OFF_*
    // constants must move with it.
    assert(strcmp(k_strings + OFF_IN_BOTH, "_in_both") == 0);
    assert(strcmp(k_strings + OFF_ONLY_LOCAL, "_only_local") == 0);
    assert(strcmp(k_strings + OFF_MALLOC_U, "_malloc") == 0);
    assert(strcmp(k_strings + OFF_HIDDEN_UU, "__hidden") == 0);
    assert(strcmp(k_strings + OFF_ONLY_SYMTAB, "_only_symtab") == 0);
    assert(strcmp(k_strings + OFF_MALLOC_BARE, "malloc") == 0);
}

// ---- export trie fixture ------------------------------------------------

// Builds a one-symbol trie: root --edge(name)--> terminal carrying `address`.
// Keeps offsets under 128 so every ULEB128 is a single byte.
static size_t build_trie(uint8_t *b, const char *name, uint64_t address, uint64_t flags) {
    uint8_t term[8];
    size_t t = 0;
    term[t++] = (uint8_t)flags;
    // ULEB128 of the address (values used here need at most two bytes).
    if (address < 0x80) {
        term[t++] = (uint8_t)address;
    } else {
        term[t++] = (uint8_t)((address & 0x7f) | 0x80);
        term[t++] = (uint8_t)(address >> 7);
        assert((address >> 14) == 0);
    }

    size_t name_len = strlen(name);
    size_t node1 = name_len + 4;
    assert(node1 < 128);

    size_t p = 0;
    b[p++] = 0x00;                   // root terminal_size
    b[p++] = 0x01;                   // one child
    memcpy(b + p, name, name_len); p += name_len;
    b[p++] = 0x00;                   // edge terminator
    b[p++] = (uint8_t)node1;         // child offset
    assert(p == node1);
    b[p++] = (uint8_t)t;             // terminal_size
    memcpy(b + p, term, t); p += t;
    b[p++] = 0x00;                   // no children
    return p;
}

static hk_symbol_sources_t make_sources(const uint8_t *trie, size_t trie_size,
                                        bool with_symbol_table) {
    hk_symbol_sources_t s;
    memset(&s, 0, sizeof(s));
    s.header_address = HDR_ADDR;
    s.slide = SLIDE;
    if (with_symbol_table) {
        s.symbol_table.nlist = k_nlist;
        s.symbol_table.nlist_count = sizeof(k_nlist) / sizeof(k_nlist[0]);
        s.symbol_table.strings = k_strings;
        s.symbol_table.strings_size = sizeof(k_strings);
    }
    s.export_trie = trie;
    s.export_trie_size = trie_size;
    return s;
}

// ---- the centerpiece ----------------------------------------------------

static void test_visibility_selects_source(void) {
    // "_in_both" is in the trie at offset 0x100 and in the symbol table at
    // unslid 0x5000, so the two sources yield DIFFERENT runtime addresses:
    //   trie   -> HDR_ADDR + 0x100 = 0x10100
    //   symtab -> 0x5000 + SLIDE   = 0x25000
    // The resolved address therefore names which source won.
    uint8_t trie[64];
    size_t trie_size = build_trie(trie, "_in_both", 0x100, 0);
    hk_symbol_sources_t sources = make_sources(trie, trie_size, true);

    hk_symbol_resolution_t r;

    // ANY: trie first.
    assert(hk_resolve_symbol(&sources, "_in_both", HK_SYMBOL_NAME_C,
                             HK_SYMBOL_VISIBILITY_ANY, &r) == HK_RESOLVE_OK);
    assert(r.source == HK_RESOLVE_SOURCE_EXPORT_TRIE);
    assert(r.address == HDR_ADDR + 0x100);
    assert(r.raw_value == 0x100);

    // PRIVATE_ALLOWED: symbol table first -- a genuinely different answer.
    assert(hk_resolve_symbol(&sources, "_in_both", HK_SYMBOL_NAME_C,
                             HK_SYMBOL_VISIBILITY_PRIVATE_ALLOWED, &r) == HK_RESOLVE_OK);
    assert(r.source == HK_RESOLVE_SOURCE_SYMBOL_TABLE);
    assert(r.address == 0x5000 + SLIDE);
    assert(r.raw_value == 0x5000);
    assert(r.n_sect == 1);

    // EXPORTED_ONLY: trie, which is dyld's authority on exports.
    assert(hk_resolve_symbol(&sources, "_in_both", HK_SYMBOL_NAME_C,
                             HK_SYMBOL_VISIBILITY_EXPORTED_ONLY, &r) == HK_RESOLVE_OK);
    assert(r.source == HK_RESOLVE_SOURCE_EXPORT_TRIE);
    assert(r.address == HDR_ADDR + 0x100);
    printf("  visibility-selects-source: PASS\n");
}

static void test_exported_only_does_not_fall_back_when_a_trie_exists(void) {
    // "_only_symtab" is in the symbol table (and marked N_EXT) but absent from
    // the trie. With a trie present, absence from it means "not exported" --
    // falling back to the symbol table would contradict dyld's own authority.
    uint8_t trie[64];
    size_t trie_size = build_trie(trie, "_something_else", 0x100, 0);
    hk_symbol_sources_t sources = make_sources(trie, trie_size, true);

    hk_symbol_resolution_t r;
    assert(hk_resolve_symbol(&sources, "_only_symtab", HK_SYMBOL_NAME_C,
                             HK_SYMBOL_VISIBILITY_EXPORTED_ONLY, &r) == HK_RESOLVE_NOT_FOUND);
    // The other visibilities do find it, via the symbol table.
    assert(hk_resolve_symbol(&sources, "_only_symtab", HK_SYMBOL_NAME_C,
                             HK_SYMBOL_VISIBILITY_ANY, &r) == HK_RESOLVE_OK);
    assert(r.source == HK_RESOLVE_SOURCE_SYMBOL_TABLE);
    printf("  exported-only-does-not-fall-back-when-a-trie-exists: PASS\n");
}

static void test_exported_only_falls_back_without_a_trie(void) {
    // No trie at all: the symbol table filtered to N_EXT is the best answer
    // available, so it IS consulted -- but a local symbol is still refused.
    hk_symbol_sources_t sources = make_sources(NULL, 0, true);

    hk_symbol_resolution_t r;
    assert(hk_resolve_symbol(&sources, "_only_symtab", HK_SYMBOL_NAME_C,
                             HK_SYMBOL_VISIBILITY_EXPORTED_ONLY, &r) == HK_RESOLVE_OK);
    assert(r.source == HK_RESOLVE_SOURCE_SYMBOL_TABLE && r.raw_value == 0x9000);

    assert(hk_resolve_symbol(&sources, "_only_local", HK_SYMBOL_NAME_C,
                             HK_SYMBOL_VISIBILITY_EXPORTED_ONLY, &r) == HK_RESOLVE_NOT_FOUND);
    // ANY and PRIVATE_ALLOWED accept the local symbol.
    assert(hk_resolve_symbol(&sources, "_only_local", HK_SYMBOL_NAME_C,
                             HK_SYMBOL_VISIBILITY_PRIVATE_ALLOWED, &r) == HK_RESOLVE_OK);
    assert(r.raw_value == 0x6000);
    printf("  exported-only-falls-back-without-a-trie: PASS\n");
}

static void test_private_allowed_still_reaches_the_trie(void) {
    // A symbol exported but stripped from the symbol table: PRIVATE_ALLOWED
    // permits private symbols, it does not require them, so the trie is still
    // consulted as a fallback.
    uint8_t trie[64];
    size_t trie_size = build_trie(trie, "_trie_only", 0x300, 0);
    hk_symbol_sources_t sources = make_sources(trie, trie_size, true);

    hk_symbol_resolution_t r;
    assert(hk_resolve_symbol(&sources, "_trie_only", HK_SYMBOL_NAME_C,
                             HK_SYMBOL_VISIBILITY_PRIVATE_ALLOWED, &r) == HK_RESOLVE_OK);
    assert(r.source == HK_RESOLVE_SOURCE_EXPORT_TRIE);
    assert(r.address == HDR_ADDR + 0x300);
    printf("  private-allowed-still-reaches-the-trie: PASS\n");
}

// ---- unified name normalization ----------------------------------------

static void test_normalization_applies_to_both_sources(void) {
    // The bare C name "malloc" must find the linker-form "_malloc" in EITHER
    // source -- one rule, applied once, covering both.
    uint8_t trie[64];
    size_t trie_size = build_trie(trie, "_malloc", 0x400, 0);

    // Trie only.
    hk_symbol_sources_t trie_only = make_sources(trie, trie_size, false);
    hk_symbol_resolution_t r;
    assert(hk_resolve_symbol(&trie_only, "malloc", HK_SYMBOL_NAME_C,
                             HK_SYMBOL_VISIBILITY_ANY, &r) == HK_RESOLVE_OK);
    assert(r.source == HK_RESOLVE_SOURCE_EXPORT_TRIE && r.address == HDR_ADDR + 0x400);

    // Symbol table only. (Queried internally with MACHO_EXACT, so this proves
    // the layer's own candidate expansion is doing the work.)
    hk_symbol_sources_t symtab_only = make_sources(NULL, 0, true);
    assert(hk_resolve_symbol(&symtab_only, "malloc", HK_SYMBOL_NAME_C,
                             HK_SYMBOL_VISIBILITY_PRIVATE_ALLOWED, &r) == HK_RESOLVE_OK);
    assert(r.source == HK_RESOLVE_SOURCE_SYMBOL_TABLE);
    printf("  normalization-applies-to-both-sources: PASS\n");
}

static void test_candidate_order_prefers_the_exact_name(void) {
    // The table holds BOTH "malloc" (0xA000) and "_malloc" (0x7000). A query
    // for "malloc" must take the exact match first.
    hk_symbol_sources_t sources = make_sources(NULL, 0, true);
    hk_symbol_resolution_t r;
    assert(hk_resolve_symbol(&sources, "malloc", HK_SYMBOL_NAME_C,
                             HK_SYMBOL_VISIBILITY_ANY, &r) == HK_RESOLVE_OK);
    assert(r.raw_value == 0xA000);  // the bare entry, not the underscored one
    printf("  candidate-order-prefers-the-exact-name: PASS\n");
}

static void test_underscored_name_still_gets_a_prefixed_candidate(void) {
    // A C symbol literally named "_hidden" appears as "__hidden". Only the
    // prefixed candidate finds it, so the prefix must be tried even when the
    // query already begins with an underscore.
    hk_symbol_sources_t sources = make_sources(NULL, 0, true);
    hk_symbol_resolution_t r;
    assert(hk_resolve_symbol(&sources, "_hidden", HK_SYMBOL_NAME_C,
                             HK_SYMBOL_VISIBILITY_ANY, &r) == HK_RESOLVE_OK);
    assert(r.raw_value == 0x8000);  // "__hidden"
    printf("  underscored-name-still-gets-a-prefixed-candidate: PASS\n");
}

static void test_macho_exact_disables_normalization(void) {
    hk_symbol_sources_t sources = make_sources(NULL, 0, true);
    hk_symbol_resolution_t r;
    // Exact mode: "_malloc" is in the table, so it resolves...
    assert(hk_resolve_symbol(&sources, "_malloc", HK_SYMBOL_NAME_MACHO_EXACT,
                             HK_SYMBOL_VISIBILITY_ANY, &r) == HK_RESOLVE_OK);
    assert(r.raw_value == 0x7000);
    // ...but "_only_symtabX" is not, and no prefixed candidate is invented.
    assert(hk_resolve_symbol(&sources, "only_symtab", HK_SYMBOL_NAME_MACHO_EXACT,
                             HK_SYMBOL_VISIBILITY_ANY, &r) == HK_RESOLVE_NOT_FOUND);
    printf("  macho-exact-disables-normalization: PASS\n");
}

// ---- edges --------------------------------------------------------------

static void test_reexport_reported_without_an_address(void) {
    uint8_t trie[64];
    size_t trie_size = build_trie(trie, "_re", 0x01, HK_EXPORT_FLAGS_REEXPORT);
    hk_symbol_sources_t sources = make_sources(trie, trie_size, true);

    hk_symbol_resolution_t r;
    assert(hk_resolve_symbol(&sources, "_re", HK_SYMBOL_NAME_C,
                             HK_SYMBOL_VISIBILITY_ANY, &r) == HK_RESOLVE_UNSUPPORTED);
    assert(r.source == HK_RESOLVE_SOURCE_EXPORT_TRIE);
    assert(r.address == 0);  // no address invented for a re-export
    assert((r.export_flags & HK_EXPORT_FLAGS_REEXPORT) != 0);
    printf("  reexport-reported-without-an-address: PASS\n");
}

static void test_name_too_long_and_empty(void) {
    hk_symbol_sources_t sources = make_sources(NULL, 0, true);
    hk_symbol_resolution_t r;

    char *huge = (char *)malloc(HK_RESOLVE_MAX_NAME + 8);
    assert(huge != NULL);
    memset(huge, 'a', HK_RESOLVE_MAX_NAME + 6);
    huge[HK_RESOLVE_MAX_NAME + 6] = '\0';
    assert(hk_resolve_symbol(&sources, huge, HK_SYMBOL_NAME_C,
                             HK_SYMBOL_VISIBILITY_ANY, &r) == HK_RESOLVE_NAME_TOO_LONG);

    // The exact boundary must be ACCEPTED, not rejected: a name of exactly
    // HK_RESOLVE_MAX_NAME still fits the prefixed candidate ('_' + name + NUL
    // == MAX_NAME + 2). An off-by-one either way shows up here -- too strict
    // and this returns TOO_LONG, too loose and the copy overflows.
    huge[HK_RESOLVE_MAX_NAME] = '\0';
    assert(strlen(huge) == HK_RESOLVE_MAX_NAME);
    assert(hk_resolve_symbol(&sources, huge, HK_SYMBOL_NAME_C,
                             HK_SYMBOL_VISIBILITY_ANY, &r) == HK_RESOLVE_NOT_FOUND);
    free(huge);

    assert(hk_resolve_symbol(&sources, "", HK_SYMBOL_NAME_C,
                             HK_SYMBOL_VISIBILITY_ANY, &r) == HK_RESOLVE_INVALID_ARGUMENT);
    printf("  name-too-long-and-empty: PASS\n");
}

static void test_empty_sources_and_null_arguments(void) {
    hk_symbol_sources_t empty;
    memset(&empty, 0, sizeof(empty));
    hk_symbol_resolution_t r;

    // Neither source present: every visibility reports NOT_FOUND, not a crash.
    assert(hk_resolve_symbol(&empty, "_x", HK_SYMBOL_NAME_C,
                             HK_SYMBOL_VISIBILITY_ANY, &r) == HK_RESOLVE_NOT_FOUND);
    assert(hk_resolve_symbol(&empty, "_x", HK_SYMBOL_NAME_C,
                             HK_SYMBOL_VISIBILITY_EXPORTED_ONLY, &r) == HK_RESOLVE_NOT_FOUND);
    assert(hk_resolve_symbol(&empty, "_x", HK_SYMBOL_NAME_C,
                             HK_SYMBOL_VISIBILITY_PRIVATE_ALLOWED, &r) == HK_RESOLVE_NOT_FOUND);

    assert(hk_resolve_symbol(NULL, "_x", HK_SYMBOL_NAME_C,
                             HK_SYMBOL_VISIBILITY_ANY, &r) == HK_RESOLVE_INVALID_ARGUMENT);
    assert(hk_resolve_symbol(&empty, NULL, HK_SYMBOL_NAME_C,
                             HK_SYMBOL_VISIBILITY_ANY, &r) == HK_RESOLVE_INVALID_ARGUMENT);
    assert(hk_resolve_symbol(&empty, "_x", HK_SYMBOL_NAME_C,
                             HK_SYMBOL_VISIBILITY_ANY, NULL) == HK_RESOLVE_INVALID_ARGUMENT);

    hk_symbol_sources_t s;
    assert(hk_symbol_sources_from_file_image(NULL, 16, &s) == HK_RESOLVE_INVALID_ARGUMENT);
    assert(hk_symbol_sources_from_loaded_image(NULL, 16, 0, &s) == HK_RESOLVE_INVALID_ARGUMENT);
    printf("  empty-sources-and-null-arguments: PASS\n");
}

// ---- source collection from real images ---------------------------------

static void put_u32(uint8_t *b, size_t off, uint32_t v) { memcpy(b + off, &v, sizeof(v)); }
static void put_u64(uint8_t *b, size_t off, uint64_t v) { memcpy(b + off, &v, sizeof(v)); }

static void test_sources_from_file_image(void) {
    // header + LC_SYMTAB + LC_DYLD_EXPORTS_TRIE, laid out as on disk.
    //   [32,56)  LC_SYMTAB      [56,72)  LC_DYLD_EXPORTS_TRIE
    //   [72,88)  one nlist      [88,104) strings      [104,..) trie
    _Alignas(8) uint8_t img[256];
    memset(img, 0, sizeof(img));
    put_u32(img, 0, HK_MH_MAGIC_64);
    put_u32(img, 16, 2);                                   // ncmds
    put_u32(img, 20, HK_SYMTAB_COMMAND_SIZE + HK_LINKEDIT_DATA_CMD_SIZE);

    put_u32(img, 32, HK_LC_SYMTAB);
    put_u32(img, 36, HK_SYMTAB_COMMAND_SIZE);
    put_u32(img, 40, 72);    // symoff
    put_u32(img, 44, 1);     // nsyms
    put_u32(img, 48, 88);    // stroff
    put_u32(img, 52, 16);    // strsize

    uint8_t trie[64];
    size_t trie_size = build_trie(trie, "_exported", 0x11, 0);
    put_u32(img, 56, HK_LC_DYLD_EXPORTS_TRIE);
    put_u32(img, 60, HK_LINKEDIT_DATA_CMD_SIZE);
    put_u32(img, 64, 104);              // dataoff
    put_u32(img, 68, (uint32_t)trie_size);

    put_u32(img, 72, 1);                                   // n_strx -> "_priv"
    img[76] = (uint8_t)(HK_N_SECT);                        // local symbol
    img[77] = 1;
    put_u64(img, 80, 0x3000);
    memcpy(img + 88, "\0_priv", 7);
    memcpy(img + 104, trie, trie_size);

    hk_symbol_sources_t sources;
    assert(hk_symbol_sources_from_file_image(img, sizeof(img), &sources) == HK_RESOLVE_OK);
    assert(sources.symbol_table.nlist != NULL && sources.symbol_table.nlist_count == 1);
    assert(sources.export_trie == img + 104 && sources.export_trie_size == trie_size);

    // Both sources resolve through the collected view.
    hk_symbol_resolution_t r;
    assert(hk_resolve_symbol(&sources, "exported", HK_SYMBOL_NAME_C,
                             HK_SYMBOL_VISIBILITY_ANY, &r) == HK_RESOLVE_OK);
    assert(r.source == HK_RESOLVE_SOURCE_EXPORT_TRIE);
    assert(r.address == (uintptr_t)img + 0x11);

    assert(hk_resolve_symbol(&sources, "priv", HK_SYMBOL_NAME_C,
                             HK_SYMBOL_VISIBILITY_PRIVATE_ALLOWED, &r) == HK_RESOLVE_OK);
    assert(r.source == HK_RESOLVE_SOURCE_SYMBOL_TABLE && r.raw_value == 0x3000);

    // An image with neither table is not an error -- it just resolves nothing.
    _Alignas(8) uint8_t bare[64];
    memset(bare, 0, sizeof(bare));
    put_u32(bare, 0, HK_MH_MAGIC_64);
    hk_symbol_sources_t empty_sources;
    assert(hk_symbol_sources_from_file_image(bare, sizeof(bare), &empty_sources) == HK_RESOLVE_OK);
    assert(empty_sources.symbol_table.nlist == NULL && empty_sources.export_trie == NULL);
    assert(hk_resolve_symbol(&empty_sources, "_x", HK_SYMBOL_NAME_C,
                             HK_SYMBOL_VISIBILITY_ANY, &r) == HK_RESOLVE_NOT_FOUND);
    printf("  sources-from-file-image: PASS\n");
}

static void test_sources_from_loaded_image(void) {
    // A loaded layout: both tables live in __LINKEDIT and must be reached by
    // translation, not by using their file offsets directly.
    //   [32,104)   __TEXT       [104,176)  __LINKEDIT
    //   [176,200)  LC_SYMTAB    [200,216)  LC_DYLD_EXPORTS_TRIE
    const size_t IMG = 0x400;
    uint8_t *img = (uint8_t *)aligned_alloc(8, IMG);
    assert(img != NULL);
    memset(img, 0, IMG);

    const uint64_t V = 0x100000000ull;
    put_u32(img, 0, HK_MH_MAGIC_64);
    put_u32(img, 16, 4);
    put_u32(img, 20, 72 + 72 + 24 + 16);

    put_u32(img, 32, HK_LC_SEGMENT_64);
    put_u32(img, 36, HK_SEGMENT_COMMAND_64_SIZE);
    memcpy(img + 40, "__TEXT", 6);
    put_u64(img, 56, V);            // vmaddr
    put_u64(img, 72, 0);            // fileoff

    // Non-degenerate: vmaddr delta 0x140 != fileoff 0x40, so translation must
    // actually compute rather than coincidentally match.
    put_u32(img, 104, HK_LC_SEGMENT_64);
    put_u32(img, 108, HK_SEGMENT_COMMAND_64_SIZE);
    memcpy(img + 112, "__LINKEDIT", 10);
    put_u64(img, 128, V + 0x140);   // vmaddr
    put_u64(img, 136, 0x200);       // vmsize
    put_u64(img, 144, 0x40);        // fileoff
    put_u64(img, 152, 0x200);       // filesize

    put_u32(img, 176, HK_LC_SYMTAB);
    put_u32(img, 180, HK_SYMTAB_COMMAND_SIZE);
    put_u32(img, 184, 0x80);        // symoff  -> img + 0x180
    put_u32(img, 188, 1);
    put_u32(img, 192, 0x90);        // stroff  -> img + 0x190
    put_u32(img, 196, 16);

    uint8_t trie[64];
    size_t trie_size = build_trie(trie, "_lexp", 0x22, 0);
    put_u32(img, 200, HK_LC_DYLD_EXPORTS_TRIE);
    put_u32(img, 204, HK_LINKEDIT_DATA_CMD_SIZE);
    put_u32(img, 208, 0xC0);        // dataoff -> img + 0x1C0
    put_u32(img, 212, (uint32_t)trie_size);

    put_u32(img, 0x180, 1);                       // n_strx -> "_lpriv"
    img[0x184] = (uint8_t)HK_N_SECT;
    img[0x185] = 1;
    put_u64(img, 0x188, 0x4000);
    memcpy(img + 0x190, "\0_lpriv", 8);
    memcpy(img + 0x1C0, trie, trie_size);

    uintptr_t slide = (uintptr_t)img - (uintptr_t)V;
    hk_symbol_sources_t sources;
    assert(hk_symbol_sources_from_loaded_image(img, IMG, slide, &sources) == HK_RESOLVE_OK);
    // linkedit_base = slide + 0x140 + V - 0x40 = img + 0x100.
    assert((const uint8_t *)sources.symbol_table.nlist == img + 0x180);
    assert(sources.export_trie == img + 0x1C0);

    hk_symbol_resolution_t r;
    assert(hk_resolve_symbol(&sources, "lexp", HK_SYMBOL_NAME_C,
                             HK_SYMBOL_VISIBILITY_ANY, &r) == HK_RESOLVE_OK);
    assert(r.source == HK_RESOLVE_SOURCE_EXPORT_TRIE);
    assert(r.address == (uintptr_t)img + 0x22);

    assert(hk_resolve_symbol(&sources, "lpriv", HK_SYMBOL_NAME_C,
                             HK_SYMBOL_VISIBILITY_PRIVATE_ALLOWED, &r) == HK_RESOLVE_OK);
    assert(r.source == HK_RESOLVE_SOURCE_SYMBOL_TABLE);
    assert(r.raw_value == 0x4000);
    assert(r.address == 0x4000 + slide);

    free(img);
    printf("  sources-from-loaded-image: PASS\n");
}

int main(void) {
    verify_string_table_offsets();
    test_visibility_selects_source();
    test_exported_only_does_not_fall_back_when_a_trie_exists();
    test_exported_only_falls_back_without_a_trie();
    test_private_allowed_still_reaches_the_trie();
    test_normalization_applies_to_both_sources();
    test_candidate_order_prefers_the_exact_name();
    test_underscored_name_still_gets_a_prefixed_candidate();
    test_macho_exact_disables_normalization();
    test_reexport_reported_without_an_address();
    test_name_too_long_and_empty();
    test_empty_sources_and_null_arguments();
    test_sources_from_file_image();
    test_sources_from_loaded_image();
    printf("all symbol resolve tests passed\n");
    return 0;
}
