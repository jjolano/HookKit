// Host test for src/resolvers/HKExportTrie.c -- ULEB128 decoding and
// export trie walking against synthetic tries. Pure buffer logic, so all of
// it runs here.
//
// The main trie below is laid out and traced by hand rather than produced by
// a builder, so the bytes under test are auditable:
//
//   off 0:  00 01 5F 00 05        node0: term=0, 1 child, edge "_"   -> 5
//   off 5:  00 02                 node1: term=0, 2 children
//   off 7:  61 6C 70 68 61 00     edge "alpha"
//   off 13: 14                                                       -> 20
//   off 14: 62 65 74 61 00        edge "beta"
//   off 19: 19                                                       -> 25
//   off 20: 03 00 80 20 00        node2: term={flags 0, addr 0x1000}, 0 kids
//   off 25: 03 00 80 40 00        node3: term={flags 0, addr 0x2000}, 0 kids

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/resolvers/HKExportTrie.h"
#include "../../src/resolvers/HKMachO.h"

static const uint8_t k_trie[] = {
    0x00, 0x01, 0x5F, 0x00, 0x05,               // node0
    0x00, 0x02,                                 // node1 header
    'a', 'l', 'p', 'h', 'a', 0x00, 0x14,        // -> node2 (20)
    'b', 'e', 't', 'a', 0x00, 0x19,             // -> node3 (25)
    0x03, 0x00, 0x80, 0x20, 0x00,               // node2: addr 0x1000
    0x03, 0x00, 0x80, 0x40, 0x00,               // node3: addr 0x2000
};

// ---- ULEB128 ------------------------------------------------------------

static void test_uleb128_values(void) {
    struct { const uint8_t bytes[12]; size_t len; uint64_t expect; } cases[] = {
        { {0x00}, 1, 0 },
        { {0x7F}, 1, 127 },
        { {0x80, 0x01}, 2, 128 },
        { {0xE5, 0x8E, 0x26}, 3, 624485 },  // the canonical LEB128 example
        // Largest 64-bit value: nine 0xFF bytes then 0x01 at shift 63.
        { {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x01}, 10, UINT64_MAX },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        size_t off = 0;
        uint64_t v = 0;
        assert(hk_export_read_uleb128(cases[i].bytes, cases[i].len, &off, &v));
        assert(v == cases[i].expect);
        assert(off == cases[i].len);  // consumed exactly the encoding
    }
    printf("  uleb128-values: PASS\n");
}

static void test_uleb128_rejects_bad_encodings(void) {
    size_t off;
    uint64_t v;

    // Truncated: a continuation bit with nothing after it.
    const uint8_t truncated[] = {0x80};
    off = 0;
    assert(!hk_export_read_uleb128(truncated, sizeof(truncated), &off, &v));

    // Overflow: 10 bytes, but the last contributes more than the single bit
    // that still fits at shift 63.
    const uint8_t overflow[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x02};
    off = 0;
    assert(!hk_export_read_uleb128(overflow, sizeof(overflow), &off, &v));

    // Overlong: an 11-byte encoding is malformed even if the value would fit.
    const uint8_t overlong[] = {0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x00};
    off = 0;
    assert(!hk_export_read_uleb128(overlong, sizeof(overlong), &off, &v));

    // Reading at the very end of a buffer.
    const uint8_t one[] = {0x01};
    off = 1;
    assert(!hk_export_read_uleb128(one, sizeof(one), &off, &v));

    assert(!hk_export_read_uleb128(NULL, 4, &off, &v));
    printf("  uleb128-rejects-bad-encodings: PASS\n");
}

// ---- trie walking -------------------------------------------------------

static void test_finds_exported_symbols(void) {
    hk_export_symbol_t sym;
    assert(hk_export_trie_find(k_trie, sizeof(k_trie), "_alpha", &sym) == HK_EXPORT_OK);
    assert(sym.address == 0x1000);
    assert(!sym.is_weak && !sym.is_reexport && !sym.is_stub_and_resolver);

    assert(hk_export_trie_find(k_trie, sizeof(k_trie), "_beta", &sym) == HK_EXPORT_OK);
    assert(sym.address == 0x2000);
    printf("  finds-exported-symbols: PASS\n");
}

static void test_absent_and_partial_names(void) {
    hk_export_symbol_t sym;
    // A name with no edge at all.
    assert(hk_export_trie_find(k_trie, sizeof(k_trie), "_zzz", &sym) == HK_EXPORT_NOT_FOUND);
    // A proper prefix of a real symbol: reaches no terminal node.
    assert(hk_export_trie_find(k_trie, sizeof(k_trie), "_alph", &sym) == HK_EXPORT_NOT_FOUND);
    // A real symbol plus extra characters: descends, then runs out of children.
    assert(hk_export_trie_find(k_trie, sizeof(k_trie), "_alphaX", &sym) == HK_EXPORT_NOT_FOUND);
    // The bare C form is deliberately NOT normalized here (see HKExportTrie.h).
    assert(hk_export_trie_find(k_trie, sizeof(k_trie), "alpha", &sym) == HK_EXPORT_NOT_FOUND);
    printf("  absent-and-partial-names: PASS\n");
}

// Builds a one-symbol trie: root --edge(name)--> terminal node carrying
// `term`. Keeps every offset and size below 128 so each ULEB128 is one byte.
static size_t build_single(uint8_t *b, const char *name,
                           const uint8_t *term, size_t term_len) {
    size_t name_len = strlen(name);
    size_t node1 = name_len + 4;
    assert(node1 < 128 && term_len < 128);

    size_t p = 0;
    b[p++] = 0x00;                 // root terminal_size
    b[p++] = 0x01;                 // one child
    memcpy(b + p, name, name_len); p += name_len;
    b[p++] = 0x00;                 // edge string terminator
    b[p++] = (uint8_t)node1;       // child offset
    assert(p == node1);

    b[p++] = (uint8_t)term_len;    // terminal_size
    memcpy(b + p, term, term_len); p += term_len;
    b[p++] = 0x00;                 // no children
    return p;
}

static void test_export_flag_kinds(void) {
    uint8_t buf[64];
    hk_export_symbol_t sym;

    // Weak definition, address 0x10.
    const uint8_t weak[] = {HK_EXPORT_FLAGS_WEAK_DEFINITION, 0x10};
    size_t n = build_single(buf, "_w", weak, sizeof(weak));
    assert(hk_export_trie_find(buf, n, "_w", &sym) == HK_EXPORT_OK);
    assert(sym.is_weak && sym.address == 0x10);

    // Thread-local and absolute are encoded in the kind bits, not separate flags.
    const uint8_t tls[] = {HK_EXPORT_FLAGS_KIND_THREAD_LOCAL, 0x20};
    n = build_single(buf, "_t", tls, sizeof(tls));
    assert(hk_export_trie_find(buf, n, "_t", &sym) == HK_EXPORT_OK);
    assert(sym.is_thread_local && !sym.is_absolute && sym.address == 0x20);

    const uint8_t abs_kind[] = {HK_EXPORT_FLAGS_KIND_ABSOLUTE, 0x30};
    n = build_single(buf, "_a", abs_kind, sizeof(abs_kind));
    assert(hk_export_trie_find(buf, n, "_a", &sym) == HK_EXPORT_OK);
    assert(sym.is_absolute && !sym.is_thread_local && sym.address == 0x30);

    // Stub and resolver: two offsets, both reported.
    const uint8_t stub[] = {HK_EXPORT_FLAGS_STUB_AND_RESOLVER, 0x40, 0x50};
    n = build_single(buf, "_s", stub, sizeof(stub));
    assert(hk_export_trie_find(buf, n, "_s", &sym) == HK_EXPORT_OK);
    assert(sym.is_stub_and_resolver && sym.address == 0x40 && sym.other == 0x50);
    printf("  export-flag-kinds: PASS\n");
}

static void test_reexport_reported_not_faked(void) {
    // A re-export names another dylib instead of carrying an address, so it
    // must be reported as unsupported rather than given a bogus address.
    uint8_t buf[64];
    const uint8_t reexport[] = {HK_EXPORT_FLAGS_REEXPORT, 0x01};
    size_t n = build_single(buf, "_r", reexport, sizeof(reexport));

    hk_export_symbol_t sym;
    assert(hk_export_trie_find(buf, n, "_r", &sym) == HK_EXPORT_UNSUPPORTED_KIND);
    assert(sym.is_reexport);
    assert(sym.other == 1);       // the library ordinal is still surfaced
    assert(sym.address == 0);     // and no address is invented
    printf("  reexport-reported-not-faked: PASS\n");
}

// ---- malformed input ----------------------------------------------------

static void test_cycle_is_broken(void) {
    // A node whose only child has a ZERO-LENGTH edge pointing back at itself.
    // The empty edge matches every name and consumes no characters, so name
    // length does not bound the walk -- only the depth cap does. Without it
    // this loops forever rather than crashing, which no sanitizer would catch.
    const uint8_t cyclic[] = {
        0x00,  // terminal_size 0
        0x01,  // one child
        0x00,  // edge string "" (immediately terminated)
        0x00,  // child offset 0 -- itself
    };
    hk_export_symbol_t sym;
    assert(hk_export_trie_find(cyclic, sizeof(cyclic), "anything", &sym) == HK_EXPORT_MALFORMED);
    printf("  cycle-is-broken: PASS\n");
}

static void test_child_offset_out_of_range(void) {
    uint8_t bad[sizeof(k_trie)];
    memcpy(bad, k_trie, sizeof(bad));
    bad[4] = 0x7F;  // root's child offset -> 127, past the 30-byte trie

    hk_export_symbol_t sym;
    assert(hk_export_trie_find(bad, sizeof(bad), "_alpha", &sym) == HK_EXPORT_MALFORMED);
    printf("  child-offset-out-of-range: PASS\n");
}

static void test_truncated_trie(void) {
    hk_export_symbol_t sym;
    // Every truncation must either fail cleanly or return the RIGHT answer --
    // never a wrong address, and never an over-read. (Heap-allocated at the
    // exact length so an over-read is ASan-visible.)
    //
    // Succeeding is legitimate for some cuts: resolving "_alpha" reads up to
    // offset 23 (the last byte of node2's terminal), and never touches
    // "_beta"'s edge or node at all, since the first matching edge wins. So
    // any length >= 24 still holds its complete path.
    unsigned succeeded = 0;
    for (size_t len = 1; len < sizeof(k_trie); len++) {
        uint8_t *cut = (uint8_t *)malloc(len);
        assert(cut != NULL);
        memcpy(cut, k_trie, len);
        hk_export_status_t st = hk_export_trie_find(cut, len, "_alpha", &sym);
        if (st == HK_EXPORT_OK) {
            assert(sym.address == 0x1000);  // correct, or nothing
            succeeded++;
        } else {
            assert(st == HK_EXPORT_MALFORMED || st == HK_EXPORT_NOT_FOUND);
        }
        free(cut);
    }
    // Lengths 24..29 keep _alpha's full path; anything shorter must not
    // resolve. Asserting the exact count keeps this from silently degrading
    // into "everything failed", which would pass vacuously.
    assert(succeeded == 6);
    printf("  truncated-trie: PASS\n");
}

static void test_unterminated_edge_string(void) {
    // An edge string with no NUL before the end of the trie. Exact-length
    // heap buffer so a missing bound would be an ASan-visible over-read.
    const uint8_t src[] = { 0x00, 0x01, 'a', 'b', 'c' };  // edge never terminates
    uint8_t *buf = (uint8_t *)malloc(sizeof(src));
    assert(buf != NULL);
    memcpy(buf, src, sizeof(src));

    hk_export_symbol_t sym;
    assert(hk_export_trie_find(buf, sizeof(src), "abc", &sym) == HK_EXPORT_MALFORMED);
    free(buf);
    printf("  unterminated-edge-string: PASS\n");
}

static void test_terminal_overrunning_trie(void) {
    uint8_t bad[sizeof(k_trie)];
    memcpy(bad, k_trie, sizeof(bad));
    bad[20] = 0x7F;  // node2 claims a 127-byte terminal in a 30-byte trie

    hk_export_symbol_t sym;
    assert(hk_export_trie_find(bad, sizeof(bad), "_alpha", &sym) == HK_EXPORT_MALFORMED);
    printf("  terminal-overrunning-trie: PASS\n");
}

static void test_invalid_arguments(void) {
    hk_export_symbol_t sym;
    assert(hk_export_trie_find(NULL, 4, "_x", &sym) == HK_EXPORT_INVALID_ARGUMENT);
    assert(hk_export_trie_find(k_trie, 0, "_x", &sym) == HK_EXPORT_INVALID_ARGUMENT);
    assert(hk_export_trie_find(k_trie, sizeof(k_trie), NULL, &sym) == HK_EXPORT_INVALID_ARGUMENT);
    assert(hk_export_trie_find(k_trie, sizeof(k_trie), "_x", NULL) == HK_EXPORT_INVALID_ARGUMENT);
    printf("  invalid-arguments: PASS\n");
}

// ---- locating the trie in a Mach-O image --------------------------------

static void put_u32(uint8_t *b, size_t off, uint32_t v) {
    memcpy(b + off, &v, sizeof(v));
}

static void write_header(uint8_t *b, uint32_t ncmds, uint32_t sizeofcmds) {
    memset(b, 0, HK_MACHO_HEADER_64_SIZE);
    put_u32(b, 0, HK_MH_MAGIC_64);
    put_u32(b, 16, ncmds);
    put_u32(b, 20, sizeofcmds);
}

static void test_finds_trie_via_exports_trie_command(void) {
    _Alignas(8) uint8_t img[128];
    memset(img, 0, sizeof(img));
    write_header(img, 1, HK_LINKEDIT_DATA_CMD_SIZE);
    put_u32(img, 32, HK_LC_DYLD_EXPORTS_TRIE);
    put_u32(img, 36, HK_LINKEDIT_DATA_CMD_SIZE);
    put_u32(img, 40, 48);                  // dataoff
    put_u32(img, 44, sizeof(k_trie));      // datasize
    memcpy(img + 48, k_trie, sizeof(k_trie));

    size_t off = 0, len = 0;
    assert(hk_macho_find_export_trie(img, sizeof(img), &off, &len) == HK_MACHO_OK);
    assert(off == 48 && len == sizeof(k_trie));

    // End to end: locate the trie in the image, then resolve through it.
    hk_export_symbol_t sym;
    assert(hk_export_trie_find(img + off, len, "_beta", &sym) == HK_EXPORT_OK);
    assert(sym.address == 0x2000);
    printf("  finds-trie-via-exports-trie-command: PASS\n");
}

static void test_finds_trie_via_dyld_info_command(void) {
    // The older form: export_off/export_size live inside LC_DYLD_INFO_ONLY.
    _Alignas(8) uint8_t img[160];
    memset(img, 0, sizeof(img));
    write_header(img, 1, HK_DYLD_INFO_COMMAND_SIZE);
    put_u32(img, 32, HK_LC_DYLD_INFO_ONLY);
    put_u32(img, 36, HK_DYLD_INFO_COMMAND_SIZE);
    put_u32(img, 32 + 40, 80);                 // export_off
    put_u32(img, 32 + 44, sizeof(k_trie));     // export_size
    memcpy(img + 80, k_trie, sizeof(k_trie));

    size_t off = 0, len = 0;
    assert(hk_macho_find_export_trie(img, sizeof(img), &off, &len) == HK_MACHO_OK);
    assert(off == 80 && len == sizeof(k_trie));

    hk_export_symbol_t sym;
    assert(hk_export_trie_find(img + off, len, "_alpha", &sym) == HK_EXPORT_OK);
    assert(sym.address == 0x1000);
    printf("  finds-trie-via-dyld-info-command: PASS\n");
}

static void test_trie_range_validated_and_absent(void) {
    size_t off = 0, len = 0;

    // No export command at all.
    _Alignas(8) uint8_t bare[64];
    memset(bare, 0, sizeof(bare));
    write_header(bare, 0, 0);
    assert(hk_macho_find_export_trie(bare, sizeof(bare), &off, &len) == HK_MACHO_NOT_FOUND);

    // An empty trie is "no exports", not an error.
    _Alignas(8) uint8_t empty[64];
    memset(empty, 0, sizeof(empty));
    write_header(empty, 1, HK_LINKEDIT_DATA_CMD_SIZE);
    put_u32(empty, 32, HK_LC_DYLD_EXPORTS_TRIE);
    put_u32(empty, 36, HK_LINKEDIT_DATA_CMD_SIZE);
    put_u32(empty, 40, 48);
    put_u32(empty, 44, 0);
    assert(hk_macho_find_export_trie(empty, sizeof(empty), &off, &len) == HK_MACHO_NOT_FOUND);

    // A range running past the image is malformed.
    _Alignas(8) uint8_t bad[64];
    memset(bad, 0, sizeof(bad));
    write_header(bad, 1, HK_LINKEDIT_DATA_CMD_SIZE);
    put_u32(bad, 32, HK_LC_DYLD_EXPORTS_TRIE);
    put_u32(bad, 36, HK_LINKEDIT_DATA_CMD_SIZE);
    put_u32(bad, 40, 48);
    put_u32(bad, 44, 999);
    assert(hk_macho_find_export_trie(bad, sizeof(bad), &off, &len) == HK_MACHO_MALFORMED);

    // A truncated LC_DYLD_EXPORTS_TRIE command.
    _Alignas(8) uint8_t trunc[64];
    memset(trunc, 0, sizeof(trunc));
    write_header(trunc, 1, 8);
    put_u32(trunc, 32, HK_LC_DYLD_EXPORTS_TRIE);
    put_u32(trunc, 36, 8);
    assert(hk_macho_find_export_trie(trunc, sizeof(trunc), &off, &len) == HK_MACHO_MALFORMED);
    printf("  trie-range-validated-and-absent: PASS\n");
}

int main(void) {
    test_uleb128_values();
    test_uleb128_rejects_bad_encodings();
    test_finds_exported_symbols();
    test_absent_and_partial_names();
    test_export_flag_kinds();
    test_reexport_reported_not_faked();
    test_cycle_is_broken();
    test_child_offset_out_of_range();
    test_truncated_trie();
    test_unterminated_edge_string();
    test_terminal_overrunning_trie();
    test_invalid_arguments();
    test_finds_trie_via_exports_trie_command();
    test_finds_trie_via_dyld_info_command();
    test_trie_range_validated_and_absent();
    printf("all export trie tests passed\n");
    return 0;
}
