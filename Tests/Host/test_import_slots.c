// Host test for Sources/Resolvers/HKImportSlots.c -- mapping import slots to
// the symbols they bind to, against synthetic images. Pure buffer logic.
//
// Three of these tests exist specifically because fishhook's equivalent walk
// omits the check they cover (sound there, since dyld pre-validated the
// image): the section's window into the indirect symbol table, the symbol
// table index, and the string table offset.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../Sources/Resolvers/HKImportSlots.h"

// ---- image layout -------------------------------------------------------
//   [0,32)     header (ncmds=3, sizeofcmds=336)
//   [32,264)   LC_SEGMENT_64 __DATA, 2 sections
//                [104,184)  __got            addr 0x1000, 2 slots, reserved1 0
//                [184,264)  __la_symbol_ptr  addr 0x2000, 2 slots, reserved1 2
//   [264,288)  LC_SYMTAB
//   [288,368)  LC_DYSYMTAB
//   [368,416)  3 nlist entries
//   [416,438)  strings
//   [440,456)  4 indirect symbol entries
#define SEG_OFF     32u
#define SEG_CMDSIZE (HK_SEGMENT_COMMAND_64_SIZE + 2u * HK_SECTION_64_SIZE)
#define SECT_GOT    (SEG_OFF + HK_SEGMENT_COMMAND_64_SIZE)
#define SECT_LA     (SECT_GOT + HK_SECTION_64_SIZE)
#define SYMTAB_OFF  (SEG_OFF + SEG_CMDSIZE)
#define DYSYM_OFF   (SYMTAB_OFF + HK_SYMTAB_COMMAND_SIZE)
#define SIZEOFCMDS  (SEG_CMDSIZE + HK_SYMTAB_COMMAND_SIZE + HK_DYSYMTAB_COMMAND_SIZE)
#define NLIST_OFF   368u
#define STR_OFF     416u
#define STR_SIZE    22u
#define INDIRECT_OFF 440u
#define INDIRECT_N  4u
#define IMG_SIZE    512u

// strings: "\0_malloc\0_free\0_local"
#define STR_MALLOC 1u
#define STR_FREE   9u
#define STR_LOCAL  15u

static void put_u32(uint8_t *b, size_t off, uint32_t v) { memcpy(b + off, &v, sizeof(v)); }
static void put_u64(uint8_t *b, size_t off, uint64_t v) { memcpy(b + off, &v, sizeof(v)); }

static void write_section(uint8_t *b, size_t off, const char *name, uint64_t addr,
                          uint64_t size, uint32_t flags, uint32_t reserved1) {
    memset(b + off, 0, HK_SECTION_64_SIZE);
    memcpy(b + off, name, strlen(name));
    memcpy(b + off + 16, "__DATA", 6);
    put_u64(b, off + 32, addr);
    put_u64(b, off + 40, size);
    put_u32(b, off + 64, flags);
    put_u32(b, off + 68, reserved1);
}

static void write_nlist(uint8_t *b, size_t off, uint32_t strx) {
    put_u32(b, off, strx);
    b[off + 4] = 0x01;  // N_UNDF | N_EXT: an import
    b[off + 5] = 0;
    put_u64(b, off + 8, 0);
}

static void build_image(uint8_t *b) {
    memset(b, 0, IMG_SIZE);
    put_u32(b, 0, HK_MH_MAGIC_64);
    put_u32(b, 16, 3);            // ncmds
    put_u32(b, 20, SIZEOFCMDS);

    put_u32(b, SEG_OFF, HK_LC_SEGMENT_64);
    put_u32(b, SEG_OFF + 4, SEG_CMDSIZE);
    memcpy(b + SEG_OFF + 8, "__DATA", 6);
    put_u32(b, SEG_OFF + 64, 2);  // nsects

    write_section(b, SECT_GOT, "__got", 0x1000, 16, HK_S_NON_LAZY_SYMBOL_POINTERS, 0);
    write_section(b, SECT_LA, "__la_symbol_ptr", 0x2000, 16, HK_S_LAZY_SYMBOL_POINTERS, 2);

    put_u32(b, SYMTAB_OFF, HK_LC_SYMTAB);
    put_u32(b, SYMTAB_OFF + 4, HK_SYMTAB_COMMAND_SIZE);
    put_u32(b, SYMTAB_OFF + 8, NLIST_OFF);
    put_u32(b, SYMTAB_OFF + 12, 3);          // nsyms
    put_u32(b, SYMTAB_OFF + 16, STR_OFF);
    put_u32(b, SYMTAB_OFF + 20, STR_SIZE);

    put_u32(b, DYSYM_OFF, HK_LC_DYSYMTAB);
    put_u32(b, DYSYM_OFF + 4, HK_DYSYMTAB_COMMAND_SIZE);
    put_u32(b, DYSYM_OFF + 56, INDIRECT_OFF);
    put_u32(b, DYSYM_OFF + 60, INDIRECT_N);

    write_nlist(b, NLIST_OFF + 0, STR_MALLOC);
    write_nlist(b, NLIST_OFF + 16, STR_FREE);
    write_nlist(b, NLIST_OFF + 32, STR_LOCAL);
    memcpy(b + STR_OFF, "\0_malloc\0_free\0_local", STR_SIZE);

    // __got window: slot 0 -> symbol 0, slot 1 -> LOCAL (no symbol).
    put_u32(b, INDIRECT_OFF + 0, 0);
    put_u32(b, INDIRECT_OFF + 4, HK_INDIRECT_SYMBOL_LOCAL);
    // __la_symbol_ptr window: slot 0 -> symbol 1, slot 1 -> symbol 2.
    put_u32(b, INDIRECT_OFF + 8, 1);
    put_u32(b, INDIRECT_OFF + 12, 2);
}

static void verify_string_offsets(void) {
    static const char s[] = "\0_malloc\0_free\0_local";
    // 1 + 7 + 1 + 5 + 1 + 6 explicit bytes, plus the literal's trailing NUL,
    // which is the terminator for "_local" and the last byte of the table.
    assert(sizeof(s) == STR_SIZE);
    assert(strcmp(s + STR_MALLOC, "_malloc") == 0);
    assert(strcmp(s + STR_FREE, "_free") == 0);
    assert(strcmp(s + STR_LOCAL, "_local") == 0);
}

// ---- collection ---------------------------------------------------------

typedef struct {
    hk_import_slot_t slots[8];
    size_t n;
    size_t stop_after;
} collect_t;

static bool collect(void *ctx, const hk_import_slot_t *slot) {
    collect_t *c = (collect_t *)ctx;
    if (c->n < 8) {
        c->slots[c->n] = *slot;
    }
    c->n++;
    return !(c->stop_after && c->n >= c->stop_after);
}

// ---- tests --------------------------------------------------------------

static void test_iterates_slots_with_names(void) {
    _Alignas(8) uint8_t img[IMG_SIZE];
    build_image(img);

    hk_import_tables_t tables;
    assert(hk_import_tables_from_file_image(img, sizeof(img), &tables) == HK_IMPORT_OK);
    assert(tables.indirect_count == INDIRECT_N);
    assert(tables.symbols.nlist_count == 3);

    collect_t c = {{{0}}, 0, 0};
    assert(hk_import_slots_iterate(img, sizeof(img), &tables, collect, &c) == HK_IMPORT_OK);

    // Four slots exist, but one is INDIRECT_SYMBOL_LOCAL and names no symbol.
    assert(c.n == 3);

    assert(c.slots[0].n_sect == 1);
    assert(strcmp(c.slots[0].sectname, "__got") == 0);
    assert(c.slots[0].slot_index == 0);
    assert(c.slots[0].slot_vmaddr == 0x1000);
    assert(strcmp(c.slots[0].symbol_name, "_malloc") == 0);
    assert(!c.slots[0].is_lazy);

    assert(c.slots[1].n_sect == 2);
    assert(strcmp(c.slots[1].sectname, "__la_symbol_ptr") == 0);
    assert(c.slots[1].slot_vmaddr == 0x2000);
    assert(strcmp(c.slots[1].symbol_name, "_free") == 0);
    assert(c.slots[1].is_lazy);

    // Slot stride is one pointer, so the second slot sits 8 bytes on.
    assert(c.slots[2].slot_index == 1);
    assert(c.slots[2].slot_vmaddr == 0x2008);
    assert(strcmp(c.slots[2].symbol_name, "_local") == 0);
    printf("  iterates-slots-with-names: PASS\n");
}

static void test_visitor_early_stop(void) {
    _Alignas(8) uint8_t img[IMG_SIZE];
    build_image(img);
    hk_import_tables_t tables;
    assert(hk_import_tables_from_file_image(img, sizeof(img), &tables) == HK_IMPORT_OK);

    collect_t c = {{{0}}, 0, 1};
    assert(hk_import_slots_iterate(img, sizeof(img), &tables, collect, &c) == HK_IMPORT_OK);
    assert(c.n == 1);
    printf("  visitor-early-stop: PASS\n");
}

static void test_find_by_name_uses_shared_normalization(void) {
    _Alignas(8) uint8_t img[IMG_SIZE];
    build_image(img);
    hk_import_tables_t tables;
    assert(hk_import_tables_from_file_image(img, sizeof(img), &tables) == HK_IMPORT_OK);

    hk_import_slot_t slot;
    // Bare C name finds the stored linker form -- the same rule hk_resolve_symbol
    // applies, from the same place.
    assert(hk_import_slots_find(img, sizeof(img), &tables, "malloc",
                                HK_SYMBOL_NAME_C, &slot) == HK_IMPORT_OK);
    assert(slot.slot_vmaddr == 0x1000 && !slot.is_lazy);

    assert(hk_import_slots_find(img, sizeof(img), &tables, "_free",
                                HK_SYMBOL_NAME_C, &slot) == HK_IMPORT_OK);
    assert(slot.slot_vmaddr == 0x2000 && slot.is_lazy);

    // MACHO_EXACT does not add the underscore, so the bare name misses.
    assert(hk_import_slots_find(img, sizeof(img), &tables, "malloc",
                                HK_SYMBOL_NAME_MACHO_EXACT, &slot) == HK_IMPORT_NOT_FOUND);
    assert(hk_import_slots_find(img, sizeof(img), &tables, "_nosuch",
                                HK_SYMBOL_NAME_C, &slot) == HK_IMPORT_NOT_FOUND);
    printf("  find-by-name-uses-shared-normalization: PASS\n");
}

// --- the three checks fishhook omits ---

static void test_reserved1_window_must_fit_indirect_table(void) {
    // CHECK 1. A section claiming a window past the end of the indirect symbol
    // table would read beyond it. fishhook computes
    // `indirect_symtab + section->reserved1` with no such check.
    uint8_t *img = (uint8_t *)aligned_alloc(8, IMG_SIZE);
    assert(img != NULL);
    build_image(img);
    put_u32(img, SECT_GOT + 68, 3);  // reserved1 3 + 2 slots > 4 entries

    hk_import_tables_t tables;
    assert(hk_import_tables_from_file_image(img, IMG_SIZE, &tables) == HK_IMPORT_OK);
    collect_t c = {{{0}}, 0, 0};
    assert(hk_import_slots_iterate(img, IMG_SIZE, &tables, collect, &c) == HK_IMPORT_MALFORMED);
    free(img);
    printf("  reserved1-window-must-fit-indirect-table: PASS\n");
}

static void test_symtab_index_must_be_in_range(void) {
    // CHECK 2. An indirect entry naming symbol 99 of a 3-entry symbol table.
    uint8_t *img = (uint8_t *)aligned_alloc(8, IMG_SIZE);
    assert(img != NULL);
    build_image(img);
    put_u32(img, INDIRECT_OFF + 0, 99);

    hk_import_tables_t tables;
    assert(hk_import_tables_from_file_image(img, IMG_SIZE, &tables) == HK_IMPORT_OK);
    collect_t c = {{{0}}, 0, 0};
    assert(hk_import_slots_iterate(img, IMG_SIZE, &tables, collect, &c) == HK_IMPORT_MALFORMED);
    free(img);
    printf("  symtab-index-must-be-in-range: PASS\n");
}

static void test_string_offset_must_be_bounded_and_terminated(void) {
    // CHECK 3, both halves. An n_strx past the string table, and a name that
    // runs to the table's end without a terminator.
    uint8_t *img = (uint8_t *)aligned_alloc(8, IMG_SIZE);
    assert(img != NULL);
    hk_import_tables_t tables;
    collect_t c;

    build_image(img);
    put_u32(img, NLIST_OFF + 0, STR_SIZE + 10);  // offset past the table
    assert(hk_import_tables_from_file_image(img, IMG_SIZE, &tables) == HK_IMPORT_OK);
    c = (collect_t){{{0}}, 0, 0};
    assert(hk_import_slots_iterate(img, IMG_SIZE, &tables, collect, &c) == HK_IMPORT_MALFORMED);

    build_image(img);
    // Fill the table with non-NUL bytes so no name terminates inside it.
    memset(img + STR_OFF, 'x', STR_SIZE);
    assert(hk_import_tables_from_file_image(img, IMG_SIZE, &tables) == HK_IMPORT_OK);
    c = (collect_t){{{0}}, 0, 0};
    assert(hk_import_slots_iterate(img, IMG_SIZE, &tables, collect, &c) == HK_IMPORT_MALFORMED);

    free(img);
    printf("  string-offset-must-be-bounded-and-terminated: PASS\n");
}

static void test_malformed_sections_and_tables(void) {
    _Alignas(8) uint8_t img[IMG_SIZE];
    hk_import_tables_t tables;
    collect_t c;

    // A pointer section whose size is not a whole number of pointers.
    build_image(img);
    put_u64(img, SECT_GOT + 40, 17);
    assert(hk_import_tables_from_file_image(img, sizeof(img), &tables) == HK_IMPORT_OK);
    c = (collect_t){{{0}}, 0, 0};
    assert(hk_import_slots_iterate(img, sizeof(img), &tables, collect, &c) == HK_IMPORT_MALFORMED);

    // An indirect table declared past the end of the image.
    build_image(img);
    put_u32(img, DYSYM_OFF + 56, IMG_SIZE + 8);
    assert(hk_import_tables_from_file_image(img, sizeof(img), &tables) == HK_IMPORT_MALFORMED);

    // A truncated LC_DYSYMTAB command.
    build_image(img);
    put_u32(img, DYSYM_OFF + 4, 8);
    put_u32(img, 20, SEG_CMDSIZE + HK_SYMTAB_COMMAND_SIZE + 8);
    assert(hk_import_tables_from_file_image(img, sizeof(img), &tables) == HK_IMPORT_MALFORMED);
    printf("  malformed-sections-and-tables: PASS\n");
}

static void test_absent_tables_are_not_errors(void) {
    // An image with no LC_DYSYMTAB has no import slots -- that is a legitimate
    // steady state, not a failure.
    _Alignas(8) uint8_t img[IMG_SIZE];
    build_image(img);
    put_u32(img, DYSYM_OFF, HK_LC_UUID);  // turn LC_DYSYMTAB into something else

    hk_import_tables_t tables;
    assert(hk_import_tables_from_file_image(img, sizeof(img), &tables) == HK_IMPORT_OK);
    assert(tables.indirect_symbols == NULL && tables.indirect_count == 0);

    collect_t c = {{{0}}, 0, 0};
    assert(hk_import_slots_iterate(img, sizeof(img), &tables, collect, &c) == HK_IMPORT_OK);
    assert(c.n == 0);

    hk_import_slot_t slot;
    assert(hk_import_slots_find(img, sizeof(img), &tables, "malloc",
                                HK_SYMBOL_NAME_C, &slot) == HK_IMPORT_NOT_FOUND);
    printf("  absent-tables-are-not-errors: PASS\n");
}

static void test_null_arguments(void) {
    _Alignas(8) uint8_t img[IMG_SIZE];
    build_image(img);
    hk_import_tables_t tables;
    assert(hk_import_tables_from_file_image(img, sizeof(img), &tables) == HK_IMPORT_OK);
    hk_import_slot_t slot;
    collect_t c = {{{0}}, 0, 0};

    assert(hk_import_tables_from_file_image(NULL, 16, &tables) == HK_IMPORT_INVALID_ARGUMENT);
    assert(hk_import_tables_from_file_image(img, 16, NULL) == HK_IMPORT_INVALID_ARGUMENT);
    assert(hk_import_tables_from_loaded_image(NULL, 16, 0, &tables) == HK_IMPORT_INVALID_ARGUMENT);
    assert(hk_import_slots_iterate(NULL, 16, &tables, collect, &c) == HK_IMPORT_INVALID_ARGUMENT);
    assert(hk_import_slots_iterate(img, 16, NULL, collect, &c) == HK_IMPORT_INVALID_ARGUMENT);
    assert(hk_import_slots_iterate(img, 16, &tables, NULL, &c) == HK_IMPORT_INVALID_ARGUMENT);
    assert(hk_import_slots_find(img, 16, &tables, NULL, HK_SYMBOL_NAME_C, &slot)
           == HK_IMPORT_INVALID_ARGUMENT);
    assert(hk_import_slots_find(img, 16, &tables, "x", HK_SYMBOL_NAME_C, NULL)
           == HK_IMPORT_INVALID_ARGUMENT);
    printf("  null-arguments: PASS\n");
}

static void test_loaded_image_translates_the_indirect_table(void) {
    // The loaded layout: the indirect table lives in __LINKEDIT, so its file
    // offset must be translated. A DECOY table sits where file-offset logic
    // would read, naming a different symbol -- so a missing translation
    // returns a visibly wrong answer instead of merely failing.
    const size_t IMG = 0x600;
    uint8_t *img = (uint8_t *)aligned_alloc(8, IMG);
    assert(img != NULL);
    memset(img, 0, IMG);

    const uint64_t V = 0x100000000ull;
    const uint32_t seg_data = 32;
    const uint32_t seg_le = seg_data + SEG_CMDSIZE;
    const uint32_t sym_off = seg_le + HK_SEGMENT_COMMAND_64_SIZE;
    const uint32_t dys_off = sym_off + HK_SYMTAB_COMMAND_SIZE;

    put_u32(img, 0, HK_MH_MAGIC_64);
    put_u32(img, 16, 4);
    put_u32(img, 20, SEG_CMDSIZE + HK_SEGMENT_COMMAND_64_SIZE +
                     HK_SYMTAB_COMMAND_SIZE + HK_DYSYMTAB_COMMAND_SIZE);

    put_u32(img, seg_data, HK_LC_SEGMENT_64);
    put_u32(img, seg_data + 4, SEG_CMDSIZE);
    memcpy(img + seg_data + 8, "__DATA", 6);
    put_u64(img, seg_data + 24, V);
    put_u32(img, seg_data + 64, 2);
    write_section(img, seg_data + HK_SEGMENT_COMMAND_64_SIZE, "__got",
                  0x1000, 8, HK_S_NON_LAZY_SYMBOL_POINTERS, 0);
    write_section(img, seg_data + HK_SEGMENT_COMMAND_64_SIZE + HK_SECTION_64_SIZE,
                  "__la_symbol_ptr", 0x2000, 0, HK_S_LAZY_SYMBOL_POINTERS, 1);

    // Non-degenerate __LINKEDIT: vmaddr delta 0x140 != fileoff 0x40.
    put_u32(img, seg_le, HK_LC_SEGMENT_64);
    put_u32(img, seg_le + 4, HK_SEGMENT_COMMAND_64_SIZE);
    memcpy(img + seg_le + 8, "__LINKEDIT", 10);
    put_u64(img, seg_le + 24, V + 0x140);
    put_u64(img, seg_le + 32, 0x400);
    put_u64(img, seg_le + 40, 0x40);
    put_u64(img, seg_le + 48, 0x400);

    // File offsets are chosen so BOTH the translated location (file + 0x100)
    // and the raw one land past the load commands, which end at 440 -- an
    // earlier draft put the string table at 0x1A0, i.e. byte 416, which sits
    // inside LC_DYSYMTAB and silently overwrote indirectsymoff.
    put_u32(img, sym_off, HK_LC_SYMTAB);
    put_u32(img, sym_off + 4, HK_SYMTAB_COMMAND_SIZE);
    put_u32(img, sym_off + 8, 0x210);   // symoff  -> img + 0x310, 2 entries
    put_u32(img, sym_off + 12, 2);
    put_u32(img, sym_off + 16, 0x230);  // stroff  -> img + 0x330
    put_u32(img, sym_off + 20, 24);

    put_u32(img, dys_off, HK_LC_DYSYMTAB);
    put_u32(img, dys_off + 4, HK_DYSYMTAB_COMMAND_SIZE);
    put_u32(img, dys_off + 56, 0x200);  // indirectsymoff -> img + 0x300
    put_u32(img, dys_off + 60, 2);

    // Real tables, where translation lands (linkedit_base = img + 0x100):
    //   indirect [0x300,0x308)   nlist [0x310,0x330)   strings [0x330,0x348)
    write_nlist(img, 0x310, 1);         // symbol 0 -> "_real"
    write_nlist(img, 0x320, 7);         // symbol 1 -> "_decoy"
    memcpy(img + 0x330, "\0_real\0_decoy", 14);
    put_u32(img, 0x300, 0);             // slot 0 -> symbol 0 ("_real")
    put_u32(img, 0x304, 1);

    // Decoy indirect table exactly where the raw file offset would point.
    put_u32(img, 0x200, 1);             // would name "_decoy" instead
    put_u32(img, 0x204, 1);

    uintptr_t slide = (uintptr_t)img - (uintptr_t)V;
    hk_import_tables_t tables;
    assert(hk_import_tables_from_loaded_image(img, IMG, slide, &tables) == HK_IMPORT_OK);
    assert(tables.indirect_symbols == img + 0x300);  // translated, not raw

    collect_t c = {{{0}}, 0, 0};
    assert(hk_import_slots_iterate(img, IMG, &tables, collect, &c) == HK_IMPORT_OK);
    assert(c.n == 1);
    assert(strcmp(c.slots[0].symbol_name, "_real") == 0);   // translated
    assert(strcmp(c.slots[0].symbol_name, "_decoy") != 0);  // not the decoy
    assert(c.slots[0].slot_vmaddr == 0x1000);

    free(img);
    printf("  loaded-image-translates-the-indirect-table: PASS\n");
}

int main(void) {
    verify_string_offsets();
    test_iterates_slots_with_names();
    test_visitor_early_stop();
    test_find_by_name_uses_shared_normalization();
    test_reserved1_window_must_fit_indirect_table();
    test_symtab_index_must_be_in_range();
    test_string_offset_must_be_bounded_and_terminated();
    test_malformed_sections_and_tables();
    test_absent_tables_are_not_errors();
    test_null_arguments();
    test_loaded_image_translates_the_indirect_table();
    printf("all import slot tests passed\n");
    return 0;
}
