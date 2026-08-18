// Host test for Sources/Resolvers/HKMachO.c -- Mach-O container parsing
// against synthetic images. All of this is pure buffer arithmetic, so it all
// runs here; only obtaining a real image is device-only.
//
// The payoff test is test_end_to_end_symtab_to_symbol_search: build an image,
// locate LC_SYMTAB, and feed the resulting view straight into the symbol
// search from the previous commit -- proving the two resolvers compose.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../Sources/Resolvers/HKMachO.h"
#include "../../Sources/Resolvers/HKSymbolTable.h"

// The symtab span validation in find_symtab_view multiplies nsyms by this
// size; if the struct ever changed, that bounds check would be wrong.
_Static_assert(sizeof(hk_macho_nlist64_t) == 16, "nlist span math depends on 16-byte entries");
_Static_assert(HK_MACHO_HEADER_64_SIZE == 32, "mach_header_64 is 32 bytes (Mach-O ABI)");
_Static_assert(HK_LOAD_COMMAND_SIZE == 8, "load_command is 8 bytes (Mach-O ABI)");
_Static_assert(HK_SYMTAB_COMMAND_SIZE == 24, "symtab_command is 24 bytes (Mach-O ABI)");

// ---- synthetic image building ------------------------------------------

static void put_u32(uint8_t *b, size_t off, uint32_t v) {
    memcpy(b + off, &v, sizeof(v));
}

// Writes a 64-bit Mach-O header. `ncmds`/`sizeofcmds` are written verbatim so
// tests can deliberately make them inconsistent with the actual commands.
static void write_header(uint8_t *b, uint32_t magic, uint32_t ncmds, uint32_t sizeofcmds) {
    memset(b, 0, HK_MACHO_HEADER_64_SIZE);
    put_u32(b, 0, magic);
    put_u32(b, 4, 0x0100000cu);  // CPU_TYPE_ARM64
    put_u32(b, 8, 0);
    put_u32(b, 12, 6);           // MH_DYLIB
    put_u32(b, 16, ncmds);
    put_u32(b, 20, sizeofcmds);
    put_u32(b, 24, 0);
}

// A complete, valid image: header + LC_SYMTAB + one nlist + a string table.
//   [0,32)   header
//   [32,56)  LC_SYMTAB
//   [56,72)  one nlist_64  (56 is 8-aligned, as a real linker would emit)
//   [72,80)  strings "\0_alpha"
#define IMG_SYMOFF  56u
#define IMG_STROFF  72u
#define IMG_STRSIZE 8u
#define IMG_SIZE    80u

static void build_symtab_image(uint8_t *b) {
    memset(b, 0, IMG_SIZE);
    write_header(b, HK_MH_MAGIC_64, 1, HK_SYMTAB_COMMAND_SIZE);

    put_u32(b, 32, HK_LC_SYMTAB);
    put_u32(b, 36, HK_SYMTAB_COMMAND_SIZE);
    put_u32(b, 40, IMG_SYMOFF);
    put_u32(b, 44, 1);            // nsyms
    put_u32(b, 48, IMG_STROFF);
    put_u32(b, 52, IMG_STRSIZE);

    // one nlist_64: n_strx=1 ("_alpha"), N_SECT|N_EXT, n_sect=1, value 0x4000
    put_u32(b, IMG_SYMOFF + 0, 1);
    b[IMG_SYMOFF + 4] = (uint8_t)(HK_N_SECT | HK_N_EXT);
    b[IMG_SYMOFF + 5] = 1;
    uint64_t value = 0x4000;
    memcpy(b + IMG_SYMOFF + 8, &value, sizeof(value));

    memcpy(b + IMG_STROFF, "\0_alpha", IMG_STRSIZE);
}

// ---- header tests -------------------------------------------------------

static void test_valid_header(void) {
    _Alignas(8) uint8_t img[IMG_SIZE];
    build_symtab_image(img);
    hk_macho_header_t h;
    assert(hk_macho_read_header(img, sizeof(img), &h) == HK_MACHO_OK);
    assert(h.magic == HK_MH_MAGIC_64);
    assert(h.ncmds == 1);
    assert(h.sizeofcmds == HK_SYMTAB_COMMAND_SIZE);
    assert(h.filetype == 6);
    printf("  valid-header: PASS\n");
}

static void test_magic_dispatch(void) {
    _Alignas(8) uint8_t img[HK_MACHO_HEADER_64_SIZE];
    hk_macho_header_t h;

    write_header(img, HK_MH_CIGAM_64, 0, 0);
    assert(hk_macho_read_header(img, sizeof(img), &h) == HK_MACHO_BYTE_ORDER_UNSUPPORTED);

    write_header(img, HK_MH_MAGIC, 0, 0);
    assert(hk_macho_read_header(img, sizeof(img), &h) == HK_MACHO_NOT_64_BIT);
    write_header(img, HK_MH_CIGAM, 0, 0);
    assert(hk_macho_read_header(img, sizeof(img), &h) == HK_MACHO_NOT_64_BIT);

    write_header(img, HK_FAT_MAGIC, 0, 0);
    assert(hk_macho_read_header(img, sizeof(img), &h) == HK_MACHO_FAT_UNSUPPORTED);
    write_header(img, HK_FAT_CIGAM, 0, 0);
    assert(hk_macho_read_header(img, sizeof(img), &h) == HK_MACHO_FAT_UNSUPPORTED);

    write_header(img, 0xdeadbeefu, 0, 0);
    assert(hk_macho_read_header(img, sizeof(img), &h) == HK_MACHO_NOT_MACHO);
    printf("  magic-dispatch: PASS\n");
}

static void test_too_small_buffers(void) {
    _Alignas(8) uint8_t img[HK_MACHO_HEADER_64_SIZE];
    write_header(img, HK_MH_MAGIC_64, 0, 0);
    hk_macho_header_t h;

    // Fewer than 4 bytes: cannot even read the magic.
    assert(hk_macho_read_header(img, 2, &h) == HK_MACHO_TOO_SMALL);
    // Magic readable, but the rest of the header is not.
    assert(hk_macho_read_header(img, 8, &h) == HK_MACHO_TOO_SMALL);
    assert(hk_macho_read_header(img, HK_MACHO_HEADER_64_SIZE - 1, &h) == HK_MACHO_TOO_SMALL);
    // Exactly a header with no commands is valid.
    assert(hk_macho_read_header(img, HK_MACHO_HEADER_64_SIZE, &h) == HK_MACHO_OK);
    printf("  too-small-buffers: PASS\n");
}

static void test_sizeofcmds_overrunning_buffer(void) {
    _Alignas(8) uint8_t img[40];
    write_header(img, HK_MH_MAGIC_64, 1, 100);  // claims 100 bytes of commands in 8
    hk_macho_header_t h;
    assert(hk_macho_read_header(img, sizeof(img), &h) == HK_MACHO_MALFORMED);
    printf("  sizeofcmds-overrunning-buffer: PASS\n");
}

// ---- load command iteration --------------------------------------------

typedef struct {
    uint32_t cmds[8];
    size_t offsets[8];
    size_t n;
    size_t stop_after;
} visit_ctx_t;

static bool record(void *c, uint32_t cmd, size_t offset, uint32_t cmdsize) {
    (void)cmdsize;
    visit_ctx_t *v = (visit_ctx_t *)c;
    if (v->n < 8) {
        v->cmds[v->n] = cmd;
        v->offsets[v->n] = offset;
    }
    v->n++;
    return !(v->stop_after && v->n >= v->stop_after);
}

static void test_iterates_commands_in_order(void) {
    // Three commands of sizes 8, 16, 8.
    _Alignas(8) uint8_t img[HK_MACHO_HEADER_64_SIZE + 32];
    write_header(img, HK_MH_MAGIC_64, 3, 32);
    put_u32(img, 32, HK_LC_UUID);      put_u32(img, 36, 8);
    put_u32(img, 40, HK_LC_SYMTAB);    put_u32(img, 44, 16);
    put_u32(img, 56, HK_LC_DYSYMTAB);  put_u32(img, 60, 8);

    visit_ctx_t v = {{0}, {0}, 0, 0};
    assert(hk_macho_iterate_load_commands(img, sizeof(img), record, &v) == HK_MACHO_OK);
    assert(v.n == 3);
    assert(v.cmds[0] == HK_LC_UUID && v.offsets[0] == 32);
    assert(v.cmds[1] == HK_LC_SYMTAB && v.offsets[1] == 40);
    assert(v.cmds[2] == HK_LC_DYSYMTAB && v.offsets[2] == 56);
    printf("  iterates-commands-in-order: PASS\n");
}

static void test_visitor_early_stop(void) {
    _Alignas(8) uint8_t img[HK_MACHO_HEADER_64_SIZE + 16];
    write_header(img, HK_MH_MAGIC_64, 2, 16);
    put_u32(img, 32, HK_LC_UUID);   put_u32(img, 36, 8);
    put_u32(img, 40, HK_LC_SYMTAB); put_u32(img, 44, 8);

    visit_ctx_t v = {{0}, {0}, 0, 1};  // stop after the first
    assert(hk_macho_iterate_load_commands(img, sizeof(img), record, &v) == HK_MACHO_OK);
    assert(v.n == 1 && v.cmds[0] == HK_LC_UUID);
    printf("  visitor-early-stop: PASS\n");
}

static void test_degenerate_cmdsize_rejected(void) {
    visit_ctx_t v = {{0}, {0}, 0, 0};

    // cmdsize 0 would leave the cursor parked forever -- the infinite-loop
    // guard. Rejected, and (proven by this test terminating) not looped on.
    _Alignas(8) uint8_t zero[HK_MACHO_HEADER_64_SIZE + 8];
    write_header(zero, HK_MH_MAGIC_64, 1, 8);
    put_u32(zero, 32, HK_LC_UUID); put_u32(zero, 36, 0);
    assert(hk_macho_iterate_load_commands(zero, sizeof(zero), record, &v) == HK_MACHO_MALFORMED);

    // cmdsize below the 8-byte minimum.
    _Alignas(8) uint8_t small[HK_MACHO_HEADER_64_SIZE + 8];
    write_header(small, HK_MH_MAGIC_64, 1, 8);
    put_u32(small, 32, HK_LC_UUID); put_u32(small, 36, 4);
    assert(hk_macho_iterate_load_commands(small, sizeof(small), record, &v) == HK_MACHO_MALFORMED);

    // cmdsize not a multiple of 8: invalid for 64-bit Mach-O.
    _Alignas(8) uint8_t unaligned[HK_MACHO_HEADER_64_SIZE + 16];
    write_header(unaligned, HK_MH_MAGIC_64, 1, 16);
    put_u32(unaligned, 32, HK_LC_UUID); put_u32(unaligned, 36, 12);
    assert(hk_macho_iterate_load_commands(unaligned, sizeof(unaligned), record, &v) == HK_MACHO_MALFORMED);
    printf("  degenerate-cmdsize-rejected: PASS\n");
}

static void test_command_overrunning_region_rejected(void) {
    _Alignas(8) uint8_t img[HK_MACHO_HEADER_64_SIZE + 8];
    write_header(img, HK_MH_MAGIC_64, 1, 8);
    put_u32(img, 32, HK_LC_UUID);
    put_u32(img, 36, 16);  // claims 16 bytes inside an 8-byte region
    visit_ctx_t v = {{0}, {0}, 0, 0};
    assert(hk_macho_iterate_load_commands(img, sizeof(img), record, &v) == HK_MACHO_MALFORMED);
    printf("  command-overrunning-region-rejected: PASS\n");
}

static void test_ncmds_exceeding_region_rejected(void) {
    _Alignas(8) uint8_t img[HK_MACHO_HEADER_64_SIZE + 8];
    write_header(img, HK_MH_MAGIC_64, 2, 8);  // 2 commands claimed, room for 1
    put_u32(img, 32, HK_LC_UUID); put_u32(img, 36, 8);
    visit_ctx_t v = {{0}, {0}, 0, 0};
    assert(hk_macho_iterate_load_commands(img, sizeof(img), record, &v) == HK_MACHO_MALFORMED);
    assert(v.n == 1);  // the first was visited before the inconsistency showed
    printf("  ncmds-exceeding-region-rejected: PASS\n");
}

static void test_find_load_command(void) {
    _Alignas(8) uint8_t img[IMG_SIZE];
    build_symtab_image(img);
    size_t off = 0;
    uint32_t cmdsize = 0;
    assert(hk_macho_find_load_command(img, sizeof(img), HK_LC_SYMTAB, &off, &cmdsize) == HK_MACHO_OK);
    assert(off == 32 && cmdsize == HK_SYMTAB_COMMAND_SIZE);

    assert(hk_macho_find_load_command(img, sizeof(img), HK_LC_DYLD_EXPORTS_TRIE,
                                      &off, &cmdsize) == HK_MACHO_NOT_FOUND);
    printf("  find-load-command: PASS\n");
}

// ---- symtab view --------------------------------------------------------

static void test_symtab_view_ranges_validated(void) {
    _Alignas(8) uint8_t img[IMG_SIZE];
    hk_symbol_table_view_t view;

    // symoff past the end.
    build_symtab_image(img);
    put_u32(img, 40, IMG_SIZE + 8);
    assert(hk_macho_find_symtab_view(img, sizeof(img), &view) == HK_MACHO_MALFORMED);

    // nsyms so large the nlist span leaves the image (and would overflow a
    // 32-bit multiply -- the span is computed in 64 bits for exactly this).
    build_symtab_image(img);
    put_u32(img, 44, 0xFFFFFFFFu);
    assert(hk_macho_find_symtab_view(img, sizeof(img), &view) == HK_MACHO_MALFORMED);

    // stroff/strsize leaving the image.
    build_symtab_image(img);
    put_u32(img, 52, IMG_SIZE);
    assert(hk_macho_find_symtab_view(img, sizeof(img), &view) == HK_MACHO_MALFORMED);

    // A truncated LC_SYMTAB (cmdsize below the 24-byte structure).
    build_symtab_image(img);
    put_u32(img, 36, 8);
    put_u32(img, 20, 8);  // keep sizeofcmds consistent with the shrunken command
    put_u32(img, 16, 1);
    assert(hk_macho_find_symtab_view(img, sizeof(img), &view) == HK_MACHO_MALFORMED);

    // No LC_SYMTAB at all.
    _Alignas(8) uint8_t bare[HK_MACHO_HEADER_64_SIZE];
    write_header(bare, HK_MH_MAGIC_64, 0, 0);
    assert(hk_macho_find_symtab_view(bare, sizeof(bare), &view) == HK_MACHO_NOT_FOUND);
    printf("  symtab-view-ranges-validated: PASS\n");
}

static void test_end_to_end_symtab_to_symbol_search(void) {
    // The composition that matters: parse an image, get the symbol table
    // view, and resolve a symbol through it with the previous commit's
    // search. Neither half is mocked.
    _Alignas(8) uint8_t img[IMG_SIZE];
    build_symtab_image(img);

    hk_symbol_table_view_t view;
    assert(hk_macho_find_symtab_view(img, sizeof(img), &view) == HK_MACHO_OK);
    assert(view.nlist_count == 1);
    assert(view.strings_size == IMG_STRSIZE);

    hk_symbol_query_t q;
    q.name = "alpha";  // bare C name; the table holds "_alpha"
    q.convention = HK_SYMBOL_NAME_C;
    q.visibility = HK_SYMBOL_VISIBILITY_EXPORTED_ONLY;

    hk_symbol_match_t m;
    assert(hk_symbol_table_find(&view, &q, &m));
    assert(m.n_value == 0x4000);
    assert(m.n_sect == 1);

    // And a symbol that isn't there resolves to nothing.
    q.name = "absent";
    assert(!hk_symbol_table_find(&view, &q, &m));
    printf("  end-to-end-symtab-to-symbol-search: PASS\n");
}

// ---- alignment ----------------------------------------------------------

static void test_misaligned_image_parses_safely(void) {
    // Parsing must work at any alignment: every read in HKMachO.c goes
    // through memcpy. With struct-pointer casts this would be undefined
    // behaviour, which UBSan reports.
    uint8_t *raw = (uint8_t *)malloc(IMG_SIZE + 1);
    assert(raw != NULL);
    uint8_t *img = raw + 1;  // deliberately odd address
    build_symtab_image(img);

    hk_macho_header_t h;
    assert(hk_macho_read_header(img, IMG_SIZE, &h) == HK_MACHO_OK);
    assert(h.ncmds == 1);

    visit_ctx_t v = {{0}, {0}, 0, 0};
    assert(hk_macho_iterate_load_commands(img, IMG_SIZE, record, &v) == HK_MACHO_OK);
    assert(v.n == 1 && v.cmds[0] == HK_LC_SYMTAB);

    // The typed nlist pointer, though, would be misaligned here -- so the
    // view is refused rather than handed back as an unsound pointer.
    hk_symbol_table_view_t view;
    assert(hk_macho_find_symtab_view(img, IMG_SIZE, &view) == HK_MACHO_MALFORMED);

    free(raw);
    printf("  misaligned-image-parses-safely: PASS\n");
}

static void test_misaligned_symoff_rejected(void) {
    // Even in an aligned image, a symoff that is not 8-aligned yields an
    // unsound nlist pointer and must be rejected.
    _Alignas(8) uint8_t img[IMG_SIZE];
    build_symtab_image(img);
    put_u32(img, 40, IMG_SYMOFF - 4);  // still in range, but 4-aligned only
    hk_symbol_table_view_t view;
    assert(hk_macho_find_symtab_view(img, sizeof(img), &view) == HK_MACHO_MALFORMED);
    printf("  misaligned-symoff-rejected: PASS\n");
}

static void test_null_tolerance(void) {
    _Alignas(8) uint8_t img[IMG_SIZE];
    build_symtab_image(img);
    hk_macho_header_t h;
    size_t off;
    uint32_t cs;
    hk_symbol_table_view_t view;
    visit_ctx_t v = {{0}, {0}, 0, 0};

    assert(hk_macho_read_header(NULL, IMG_SIZE, &h) == HK_MACHO_NOT_MACHO);
    assert(hk_macho_read_header(img, IMG_SIZE, NULL) == HK_MACHO_NOT_MACHO);
    assert(hk_macho_iterate_load_commands(img, IMG_SIZE, NULL, &v) == HK_MACHO_NOT_MACHO);
    assert(hk_macho_find_load_command(img, IMG_SIZE, HK_LC_SYMTAB, NULL, &cs) == HK_MACHO_NOT_MACHO);
    assert(hk_macho_find_load_command(img, IMG_SIZE, HK_LC_SYMTAB, &off, NULL) == HK_MACHO_NOT_MACHO);
    assert(hk_macho_find_symtab_view(img, IMG_SIZE, NULL) == HK_MACHO_NOT_MACHO);
    assert(hk_macho_find_symtab_view(NULL, IMG_SIZE, &view) == HK_MACHO_NOT_MACHO);
    printf("  null-tolerance: PASS\n");
}

int main(void) {
    test_valid_header();
    test_magic_dispatch();
    test_too_small_buffers();
    test_sizeofcmds_overrunning_buffer();
    test_iterates_commands_in_order();
    test_visitor_early_stop();
    test_degenerate_cmdsize_rejected();
    test_command_overrunning_region_rejected();
    test_ncmds_exceeding_region_rejected();
    test_find_load_command();
    test_symtab_view_ranges_validated();
    test_end_to_end_symtab_to_symbol_search();
    test_misaligned_image_parses_safely();
    test_misaligned_symoff_rejected();
    test_null_tolerance();
    printf("all mach-o tests passed\n");
    return 0;
}
