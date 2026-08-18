// Host test for Sources/Resolvers/HKSymbolTable.c -- Mach-O symbol table
// search against synthetic tables. Pure logic, so all of it runs here; the
// device side only ever supplies the table view (see HKSymbolTable.h).
//
// The nlist layout static_asserts below are load-bearing: this file declares
// its own hk_macho_nlist64_t so it can build on a non-Apple host, and that is
// only sound if the layout is identical to Apple's struct nlist_64.

#include <assert.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "../../Sources/Resolvers/HKSymbolTable.h"

_Static_assert(sizeof(hk_macho_nlist64_t) == 16, "nlist_64 is 16 bytes");
_Static_assert(offsetof(hk_macho_nlist64_t, n_strx) == 0, "n_strx first (Apple wraps it in union n_un)");
_Static_assert(offsetof(hk_macho_nlist64_t, n_type) == 4, "n_type at 4");
_Static_assert(offsetof(hk_macho_nlist64_t, n_sect) == 5, "n_sect at 5");
_Static_assert(offsetof(hk_macho_nlist64_t, n_desc) == 6, "n_desc at 6");
_Static_assert(offsetof(hk_macho_nlist64_t, n_value) == 8, "n_value at 8");

// ---- helpers ------------------------------------------------------------

static hk_macho_nlist64_t sym(uint32_t strx, uint8_t type, uint8_t sect, uint64_t value) {
    hk_macho_nlist64_t s;
    memset(&s, 0, sizeof(s));
    s.n_strx = strx;
    s.n_type = type;
    s.n_sect = sect;
    s.n_value = value;
    return s;
}

static hk_symbol_query_t query(const char *name, hk_symbol_name_convention_t conv,
                               hk_symbol_visibility_t vis) {
    hk_symbol_query_t q;
    q.name = name;
    q.convention = conv;
    q.visibility = vis;
    return q;
}

// A string table laid out as: [0]="" then the given names, NUL-separated.
// Offsets are returned so tests can reference them precisely.
//   offsets[0] -> "_alpha", offsets[1] -> "_beta", offsets[2] -> "_gamma"
static const char k_strings[] = "\0_alpha\0_beta\0_gamma";
#define OFF_ALPHA 1u
#define OFF_BETA  8u
#define OFF_GAMMA 14u

static hk_symbol_table_view_t view_of(const hk_macho_nlist64_t *nl, uint32_t count) {
    hk_symbol_table_view_t v;
    v.nlist = nl;
    v.nlist_count = count;
    v.strings = k_strings;
    v.strings_size = sizeof(k_strings);  // includes the final NUL
    return v;
}

// ---- tests --------------------------------------------------------------

static void test_string_table_offsets_are_what_the_tests_assume(void) {
    // Guards every other test in this file: if the literal above is edited,
    // the OFF_* constants must move with it.
    assert(strcmp(k_strings + OFF_ALPHA, "_alpha") == 0);
    assert(strcmp(k_strings + OFF_BETA, "_beta") == 0);
    assert(strcmp(k_strings + OFF_GAMMA, "_gamma") == 0);
    printf("  string-table-offsets-are-what-the-tests-assume: PASS\n");
}

static void test_finds_c_symbol_by_bare_name(void) {
    hk_macho_nlist64_t nl[] = {
        sym(OFF_ALPHA, HK_N_SECT | HK_N_EXT, 1, 0x4000),
    };
    hk_symbol_table_view_t v = view_of(nl, 1);
    hk_symbol_query_t q = query("alpha", HK_SYMBOL_NAME_C, HK_SYMBOL_VISIBILITY_ANY);
    hk_symbol_match_t m;
    assert(hk_symbol_table_find(&v, &q, &m));
    assert(m.n_value == 0x4000);  // UNSLID -- the caller adds the slide
    assert(m.n_sect == 1);
    assert(m.index == 0);

    // The underscored form must resolve to the same entry.
    hk_symbol_query_t q2 = query("_alpha", HK_SYMBOL_NAME_C, HK_SYMBOL_VISIBILITY_ANY);
    hk_symbol_match_t m2;
    assert(hk_symbol_table_find(&v, &q2, &m2));
    assert(m2.n_value == 0x4000);
    printf("  finds-c-symbol-by-bare-name: PASS\n");
}

static void test_macho_exact_does_not_normalize_underscore(void) {
    hk_macho_nlist64_t nl[] = {
        sym(OFF_ALPHA, HK_N_SECT | HK_N_EXT, 1, 0x4000),
    };
    hk_symbol_table_view_t v = view_of(nl, 1);
    hk_symbol_match_t m;

    // Exact mode: the table holds "_alpha", so "alpha" must NOT match...
    hk_symbol_query_t bare = query("alpha", HK_SYMBOL_NAME_MACHO_EXACT, HK_SYMBOL_VISIBILITY_ANY);
    assert(!hk_symbol_table_find(&v, &bare, &m));
    // ...but the exact table string must.
    hk_symbol_query_t exact = query("_alpha", HK_SYMBOL_NAME_MACHO_EXACT, HK_SYMBOL_VISIBILITY_ANY);
    assert(hk_symbol_table_find(&v, &exact, &m));
    assert(m.n_value == 0x4000);
    printf("  macho-exact-does-not-normalize-underscore: PASS\n");
}

static void test_mangled_conventions_normalize_underscore(void) {
    // C++ and Swift mangled names take Mach-O's leading underscore too, so
    // both conventions must behave like C here, not like MACHO_EXACT.
    hk_macho_nlist64_t nl[] = {
        sym(OFF_BETA, HK_N_SECT | HK_N_EXT, 1, 0x5000),
    };
    hk_symbol_table_view_t v = view_of(nl, 1);
    hk_symbol_match_t m;

    hk_symbol_query_t cxx = query("beta", HK_SYMBOL_NAME_CXX_MANGLED, HK_SYMBOL_VISIBILITY_ANY);
    assert(hk_symbol_table_find(&v, &cxx, &m) && m.n_value == 0x5000);

    hk_symbol_query_t swift = query("beta", HK_SYMBOL_NAME_SWIFT_MANGLED, HK_SYMBOL_VISIBILITY_ANY);
    assert(hk_symbol_table_find(&v, &swift, &m) && m.n_value == 0x5000);
    printf("  mangled-conventions-normalize-underscore: PASS\n");
}

static void test_exported_only_rejects_local_symbol(void) {
    // Same name, no N_EXT bit: a local (static) symbol.
    hk_macho_nlist64_t local[] = {
        sym(OFF_ALPHA, HK_N_SECT, 1, 0x4000),
    };
    hk_symbol_table_view_t v = view_of(local, 1);
    hk_symbol_query_t q = query("alpha", HK_SYMBOL_NAME_C, HK_SYMBOL_VISIBILITY_EXPORTED_ONLY);
    hk_symbol_match_t m;
    assert(!hk_symbol_table_find(&v, &q, &m));

    // ANY and PRIVATE_ALLOWED both accept it.
    hk_symbol_query_t any = query("alpha", HK_SYMBOL_NAME_C, HK_SYMBOL_VISIBILITY_ANY);
    assert(hk_symbol_table_find(&v, &any, &m) && m.n_value == 0x4000);
    hk_symbol_query_t priv = query("alpha", HK_SYMBOL_NAME_C, HK_SYMBOL_VISIBILITY_PRIVATE_ALLOWED);
    assert(hk_symbol_table_find(&v, &priv, &m) && m.n_value == 0x4000);

    // An external symbol satisfies EXPORTED_ONLY.
    hk_macho_nlist64_t ext[] = {
        sym(OFF_ALPHA, HK_N_SECT | HK_N_EXT, 1, 0x4000),
    };
    hk_symbol_table_view_t ve = view_of(ext, 1);
    assert(hk_symbol_table_find(&ve, &q, &m) && m.n_value == 0x4000);
    printf("  exported-only-rejects-local-symbol: PASS\n");
}

static void test_stab_debug_entries_rejected(void) {
    // N_BNSYM (0x2e) and N_ENSYM (0x4e) are STAB entries that ALSO satisfy
    // (n_type & N_TYPE) == N_SECT by coincidence -- the exact reason the STAB
    // check has to come first and stand on its own.
    assert((0x2eu & HK_N_TYPE) == HK_N_SECT);  // the coincidence, asserted
    assert((0x4eu & HK_N_TYPE) == HK_N_SECT);

    hk_macho_nlist64_t nl[] = {
        sym(OFF_ALPHA, 0x2e, 1, 0x4000),  // N_BNSYM, named, plausible address
        sym(OFF_ALPHA, 0x4e, 1, 0x4000),  // N_ENSYM
    };
    hk_symbol_table_view_t v = view_of(nl, 2);
    hk_symbol_query_t q = query("alpha", HK_SYMBOL_NAME_C, HK_SYMBOL_VISIBILITY_ANY);
    hk_symbol_match_t m;
    assert(!hk_symbol_table_find(&v, &q, &m));
    printf("  stab-debug-entries-rejected: PASS\n");
}

static void test_undefined_absolute_and_zero_value_rejected(void) {
    hk_macho_nlist64_t nl[] = {
        sym(OFF_ALPHA, HK_N_UNDF | HK_N_EXT, 0, 0),        // undefined
        sym(OFF_ALPHA, HK_N_ABS  | HK_N_EXT, 0, 0x1234),   // absolute, not in a section
        sym(OFF_ALPHA, HK_N_INDR | HK_N_EXT, 0, 0x1234),   // indirect
        sym(OFF_ALPHA, HK_N_SECT | HK_N_EXT, 1, 0),        // defined but no address
    };
    hk_symbol_table_view_t v = view_of(nl, 4);
    hk_symbol_query_t q = query("alpha", HK_SYMBOL_NAME_C, HK_SYMBOL_VISIBILITY_ANY);
    hk_symbol_match_t m;
    assert(!hk_symbol_table_find(&v, &q, &m));
    printf("  undefined-absolute-and-zero-value-rejected: PASS\n");
}

static void test_first_match_in_table_order_wins(void) {
    // Three entries named "_alpha"; the first valid one must win, and the
    // rule must hold when an earlier entry is skipped for being invalid.
    hk_macho_nlist64_t nl[] = {
        sym(OFF_ALPHA, HK_N_SECT | HK_N_EXT, 1, 0),        // skipped: no address
        sym(OFF_ALPHA, HK_N_SECT | HK_N_EXT, 2, 0x7000),   // <- expected
        sym(OFF_ALPHA, HK_N_SECT | HK_N_EXT, 3, 0x8000),
    };
    hk_symbol_table_view_t v = view_of(nl, 3);
    hk_symbol_query_t q = query("alpha", HK_SYMBOL_NAME_C, HK_SYMBOL_VISIBILITY_ANY);
    hk_symbol_match_t m;
    assert(hk_symbol_table_find(&v, &q, &m));
    assert(m.n_value == 0x7000 && m.index == 1 && m.n_sect == 2);
    printf("  first-match-in-table-order-wins: PASS\n");
}

static void test_out_of_range_and_zero_strx_rejected(void) {
    hk_macho_nlist64_t nl[] = {
        sym(0, HK_N_SECT | HK_N_EXT, 1, 0x4000),            // n_strx 0 == unnamed
        sym(99999, HK_N_SECT | HK_N_EXT, 1, 0x4000),        // n_strx past the table
    };
    hk_symbol_table_view_t v = view_of(nl, 2);
    hk_symbol_query_t q = query("alpha", HK_SYMBOL_NAME_C, HK_SYMBOL_VISIBILITY_ANY);
    hk_symbol_match_t m;
    assert(!hk_symbol_table_find(&v, &q, &m));
    printf("  out-of-range-and-zero-strx-rejected: PASS\n");
}

static void test_unterminated_string_table_does_not_over_read(void) {
    // The bounds-safety test that matters. The string table is heap-allocated
    // at EXACTLY 6 bytes: "\0_abcd" with NO terminator after the 'd'. Any read
    // past it is a real heap-buffer-overflow that ASan traps. A plain strcmp()
    // would run off the end looking for a NUL. The name sits at offset 1, not
    // 0, because n_strx == 0 means "unnamed" and is rejected before matching.
    size_t n = 6;
    char *strings = (char *)malloc(n);
    assert(strings != NULL);
    memcpy(strings, "\0_abcd", n);  // deliberately unterminated

    hk_macho_nlist64_t nl[] = { sym(1, HK_N_SECT | HK_N_EXT, 1, 0x4000) };
    hk_symbol_table_view_t v;
    v.nlist = nl;
    v.nlist_count = 1;
    v.strings = strings;
    v.strings_size = n;
    hk_symbol_match_t m;

    // Exact path: compares "_abcd" and runs out of table before a NUL.
    hk_symbol_query_t exact = query("_abcd", HK_SYMBOL_NAME_MACHO_EXACT, HK_SYMBOL_VISIBILITY_ANY);
    assert(!hk_symbol_table_find(&v, &exact, &m));

    // Underscore-normalizing path: skips the '_' and compares "abcd", which
    // also runs out of table. Both paths must stay in bounds.
    hk_symbol_query_t bare = query("abcd", HK_SYMBOL_NAME_C, HK_SYMBOL_VISIBILITY_ANY);
    assert(!hk_symbol_table_find(&v, &bare, &m));

    free(strings);
    printf("  unterminated-string-table-does-not-over-read: PASS\n");
}

static void test_exactly_terminated_string_table_matches(void) {
    // The boundary case on the other side: the final NUL is the last byte of
    // the table, which must still match (an off-by-one in the bounds check
    // would wrongly reject this).
    size_t n = 7;
    char *strings = (char *)malloc(n);
    assert(strings != NULL);
    memcpy(strings, "\0_abcd", n);  // 7 bytes: includes the trailing NUL

    hk_macho_nlist64_t nl[] = { sym(1, HK_N_SECT | HK_N_EXT, 1, 0x4000) };
    hk_symbol_table_view_t v;
    v.nlist = nl;
    v.nlist_count = 1;
    v.strings = strings;
    v.strings_size = n;
    hk_symbol_match_t m;

    hk_symbol_query_t bare = query("abcd", HK_SYMBOL_NAME_C, HK_SYMBOL_VISIBILITY_ANY);
    assert(hk_symbol_table_find(&v, &bare, &m) && m.n_value == 0x4000);
    hk_symbol_query_t exact = query("_abcd", HK_SYMBOL_NAME_MACHO_EXACT, HK_SYMBOL_VISIBILITY_ANY);
    assert(hk_symbol_table_find(&v, &exact, &m) && m.n_value == 0x4000);

    free(strings);
    printf("  exactly-terminated-string-table-matches: PASS\n");
}

static void test_no_match_and_null_tolerance(void) {
    hk_macho_nlist64_t nl[] = { sym(OFF_ALPHA, HK_N_SECT | HK_N_EXT, 1, 0x4000) };
    hk_symbol_table_view_t v = view_of(nl, 1);
    hk_symbol_query_t q = query("nosuch", HK_SYMBOL_NAME_C, HK_SYMBOL_VISIBILITY_ANY);
    hk_symbol_match_t m;
    assert(!hk_symbol_table_find(&v, &q, &m));

    assert(!hk_symbol_table_find(NULL, &q, &m));
    assert(!hk_symbol_table_find(&v, NULL, &m));
    assert(!hk_symbol_table_find(&v, &q, NULL));

    hk_symbol_query_t empty = query("", HK_SYMBOL_NAME_C, HK_SYMBOL_VISIBILITY_ANY);
    assert(!hk_symbol_table_find(&v, &empty, &m));
    hk_symbol_query_t noname = query(NULL, HK_SYMBOL_NAME_C, HK_SYMBOL_VISIBILITY_ANY);
    assert(!hk_symbol_table_find(&v, &noname, &m));

    // A view with no entries, or no string table, finds nothing.
    hk_symbol_table_view_t empty_view = view_of(nl, 0);
    hk_symbol_query_t a = query("alpha", HK_SYMBOL_NAME_C, HK_SYMBOL_VISIBILITY_ANY);
    assert(!hk_symbol_table_find(&empty_view, &a, &m));
    hk_symbol_table_view_t no_strings = view_of(nl, 1);
    no_strings.strings = NULL;
    assert(!hk_symbol_table_find(&no_strings, &a, &m));
    hk_symbol_table_view_t zero_strings = view_of(nl, 1);
    zero_strings.strings_size = 0;
    assert(!hk_symbol_table_find(&zero_strings, &a, &m));
    printf("  no-match-and-null-tolerance: PASS\n");
}

int main(void) {
    test_string_table_offsets_are_what_the_tests_assume();
    test_finds_c_symbol_by_bare_name();
    test_macho_exact_does_not_normalize_underscore();
    test_mangled_conventions_normalize_underscore();
    test_exported_only_rejects_local_symbol();
    test_stab_debug_entries_rejected();
    test_undefined_absolute_and_zero_value_rejected();
    test_first_match_in_table_order_wins();
    test_out_of_range_and_zero_strx_rejected();
    test_unterminated_string_table_does_not_over_read();
    test_exactly_terminated_string_table_matches();
    test_no_match_and_null_tolerance();
    printf("all symbol table tests passed\n");
    return 0;
}
