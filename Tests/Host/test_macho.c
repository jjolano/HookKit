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

static void put_be32(uint8_t *b, size_t off, uint32_t v) {
    b[off] = (uint8_t)(v >> 24); b[off + 1] = (uint8_t)(v >> 16);
    b[off + 2] = (uint8_t)(v >> 8); b[off + 3] = (uint8_t)v;
}

static void put_be64(uint8_t *b, size_t off, uint64_t v) {
    put_be32(b, off, (uint32_t)(v >> 32));
    put_be32(b, off + 4, (uint32_t)v);
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

static void test_selects_thin_fat32_and_fat64_slices(void) {
    const uint32_t cpu = 0x0100000cu;
    const uint32_t subtype = 2;
    const void *slice = NULL;
    size_t slice_size = 0;

    uint8_t thin[HK_MACHO_HEADER_64_SIZE];
    write_header(thin, HK_MH_MAGIC_64, 0, 0);
    put_u32(thin, 8, subtype);
    assert(hk_macho_select_slice(thin, sizeof(thin), cpu, subtype,
                                 &slice, &slice_size) == HK_MACHO_OK);
    assert(slice == thin && slice_size == sizeof(thin));

    uint8_t fat32[128] = {0};
    put_be32(fat32, 0, HK_FAT_MAGIC);
    put_be32(fat32, 4, 1);
    put_be32(fat32, 8, cpu);
    put_be32(fat32, 12, subtype);
    put_be32(fat32, 16, 64);
    put_be32(fat32, 20, HK_MACHO_HEADER_64_SIZE);
    write_header(fat32 + 64, HK_MH_MAGIC_64, 0, 0);
    put_u32(fat32 + 64, 8, subtype);
    assert(hk_macho_select_slice(fat32, sizeof(fat32), cpu, subtype,
                                 &slice, &slice_size) == HK_MACHO_OK);
    assert(slice == fat32 + 64 && slice_size == HK_MACHO_HEADER_64_SIZE);

    uint8_t fat64[192] = {0};
    put_be32(fat64, 0, HK_FAT_MAGIC_64);
    put_be32(fat64, 4, 1);
    put_be32(fat64, 8, cpu);
    put_be32(fat64, 12, subtype);
    put_be64(fat64, 16, 128);
    put_be64(fat64, 24, HK_MACHO_HEADER_64_SIZE);
    write_header(fat64 + 128, HK_MH_MAGIC_64, 0, 0);
    put_u32(fat64 + 128, 8, subtype);
    assert(hk_macho_select_slice(fat64, sizeof(fat64), cpu, subtype,
                                 &slice, &slice_size) == HK_MACHO_OK);
    assert(slice == fat64 + 128 && slice_size == HK_MACHO_HEADER_64_SIZE);
    assert(hk_macho_select_slice(fat64, sizeof(fat64), cpu, 0,
                                 &slice, &slice_size) == HK_MACHO_NOT_FOUND);

    put_be64(fat64, 24, 1000);
    assert(hk_macho_select_slice(fat64, sizeof(fat64), cpu, subtype,
                                 &slice, &slice_size) == HK_MACHO_MALFORMED);
    printf("  selects-thin-fat32-and-fat64-slices: PASS\n");
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


// ---- segments and sections ----------------------------------------------

// An image with two segments and two sections:
//   [0,32)     header (ncmds=3, sizeofcmds=328)
//   [32,264)   LC_SEGMENT_64 __TEXT, cmdsize 232, nsects 2
//                [104,184)  section __text   (instructions)
//                [184,264)  section __const  (data)
//   [264,336)  LC_SEGMENT_64 __LINKEDIT, cmdsize 72, nsects 0
//   [336,360)  LC_SYMTAB
#define SEG_TEXT_OFF      32u
#define SEG_TEXT_CMDSIZE  (HK_SEGMENT_COMMAND_64_SIZE + 2u * HK_SECTION_64_SIZE)
#define SEG_LE_OFF        (SEG_TEXT_OFF + SEG_TEXT_CMDSIZE)
#define SEG_SYMTAB_OFF    (SEG_LE_OFF + HK_SEGMENT_COMMAND_64_SIZE)
#define SEG_SIZEOFCMDS    (SEG_TEXT_CMDSIZE + HK_SEGMENT_COMMAND_64_SIZE + HK_SYMTAB_COMMAND_SIZE)
#define SEG_IMG_SIZE      0x400u

// Where the loaded-image test places the two symbol tables. The decoy sits
// where FILE-offset logic would look; the real one where __LINKEDIT
// translation lands. See test_loaded_image_symtab_translation.
#define DECOY_NLIST_OFF   0x80u
#define DECOY_STR_OFF     0x90u
#define REAL_NLIST_OFF    0x180u
#define REAL_STR_OFF      0x190u

static void put_u64(uint8_t *b, size_t off, uint64_t v) {
    memcpy(b + off, &v, sizeof(v));
}

static void write_segment(uint8_t *b, size_t off, const char *name, uint32_t cmdsize,
                          uint64_t vmaddr, uint64_t vmsize,
                          uint64_t fileoff, uint64_t filesize, uint32_t nsects) {
    memset(b + off, 0, cmdsize);
    put_u32(b, off + 0, HK_LC_SEGMENT_64);
    put_u32(b, off + 4, cmdsize);
    memcpy(b + off + 8, name, strlen(name));  // fixed 16-byte field, zero-padded
    put_u64(b, off + 24, vmaddr);
    put_u64(b, off + 32, vmsize);
    put_u64(b, off + 40, fileoff);
    put_u64(b, off + 48, filesize);
    put_u32(b, off + 64, nsects);
}

static void write_section(uint8_t *b, size_t off, const char *sectname, uint32_t flags) {
    memset(b + off, 0, HK_SECTION_64_SIZE);
    memcpy(b + off, sectname, strlen(sectname));
    put_u32(b, off + 64, flags);
}

static void write_nlist(uint8_t *b, size_t off, uint32_t strx, uint8_t type,
                        uint8_t sect, uint64_t value) {
    put_u32(b, off + 0, strx);
    b[off + 4] = type;
    b[off + 5] = sect;
    put_u64(b, off + 8, value);
}

// `text_vmaddr` is the image's preferred load address; the loaded-image test
// derives a slide from it. File-image tests can pass anything.
static void build_segment_image(uint8_t *b, uint64_t text_vmaddr) {
    memset(b, 0, SEG_IMG_SIZE);
    write_header(b, HK_MH_MAGIC_64, 3, SEG_SIZEOFCMDS);

    write_segment(b, SEG_TEXT_OFF, "__TEXT", SEG_TEXT_CMDSIZE,
                  text_vmaddr, 0x1000, 0, 0x1000, 2);
    write_section(b, SEG_TEXT_OFF + HK_SEGMENT_COMMAND_64_SIZE,
                  "__text", HK_S_ATTR_PURE_INSTRUCTIONS);
    write_section(b, SEG_TEXT_OFF + HK_SEGMENT_COMMAND_64_SIZE + HK_SECTION_64_SIZE,
                  "__const", 0);

    // Deliberately non-degenerate: the vmaddr delta (0x140) differs from the
    // file offset (0x40), so translation genuinely has to do arithmetic.
    write_segment(b, SEG_LE_OFF, "__LINKEDIT", HK_SEGMENT_COMMAND_64_SIZE,
                  text_vmaddr + 0x140, 0x200, 0x40, 0x200, 0);

    put_u32(b, SEG_SYMTAB_OFF + 0, HK_LC_SYMTAB);
    put_u32(b, SEG_SYMTAB_OFF + 4, HK_SYMTAB_COMMAND_SIZE);
    put_u32(b, SEG_SYMTAB_OFF + 8, 0x80);   // symoff
    put_u32(b, SEG_SYMTAB_OFF + 12, 1);     // nsyms
    put_u32(b, SEG_SYMTAB_OFF + 16, 0x90);  // stroff
    put_u32(b, SEG_SYMTAB_OFF + 20, 8);     // strsize

    // Negative control: a decoy table exactly where file-offset logic would
    // read (symoff/stroff used as image offsets), holding a different value.
    write_nlist(b, DECOY_NLIST_OFF, 1, (uint8_t)(HK_N_SECT | HK_N_EXT), 1, 0xDEAD);
    memcpy(b + DECOY_STR_OFF, "\0_alpha", 8);

    // The real table, where __LINKEDIT translation lands.
    write_nlist(b, REAL_NLIST_OFF, 1, (uint8_t)(HK_N_SECT | HK_N_EXT), 1, 0x4000);
    memcpy(b + REAL_STR_OFF, "\0_alpha", 8);
}

static void test_find_segment(void) {
    _Alignas(8) uint8_t img[SEG_IMG_SIZE];
    build_segment_image(img, 0x100000000ull);

    hk_macho_segment_t seg;
    assert(hk_macho_find_segment(img, sizeof(img), "__TEXT", &seg) == HK_MACHO_OK);
    assert(strcmp(seg.segname, "__TEXT") == 0);
    assert(seg.nsects == 2);
    assert(seg.vmaddr == 0x100000000ull);
    assert(seg.command_offset == SEG_TEXT_OFF);

    assert(hk_macho_find_segment(img, sizeof(img), "__LINKEDIT", &seg) == HK_MACHO_OK);
    assert(seg.fileoff == 0x40 && seg.filesize == 0x200 && seg.nsects == 0);

    assert(hk_macho_find_segment(img, sizeof(img), "__DATA", &seg) == HK_MACHO_NOT_FOUND);
    printf("  find-segment: PASS\n");
}

static void test_segname_is_not_read_as_a_c_string(void) {
    // segname is a fixed 16-byte field with no required terminator. A full
    // 16-character name must compare correctly and must not over-read into
    // the following vmaddr field.
    _Alignas(8) uint8_t img[SEG_IMG_SIZE];
    build_segment_image(img, 0x100000000ull);
    memcpy(img + SEG_TEXT_OFF + 8, "ABCDEFGHIJKLMNOP", 16);  // exactly 16, unterminated

    hk_macho_segment_t seg;
    assert(hk_macho_find_segment(img, sizeof(img), "ABCDEFGHIJKLMNOP", &seg) == HK_MACHO_OK);
    assert(strlen(seg.segname) == 16);
    // A 17-character query cannot match a 16-byte field.
    assert(hk_macho_find_segment(img, sizeof(img), "ABCDEFGHIJKLMNOPQ", &seg) == HK_MACHO_NOT_FOUND);
    printf("  segname-is-not-read-as-a-c-string: PASS\n");
}

static void test_section_flags_and_code_check(void) {
    _Alignas(8) uint8_t img[SEG_IMG_SIZE];
    build_segment_image(img, 0x100000000ull);
    uint32_t flags = 0;

    // n_sect is 1-based and counts across segments in load-command order.
    assert(hk_macho_section_flags(img, sizeof(img), 1, &flags) == HK_MACHO_OK);
    assert(flags == HK_S_ATTR_PURE_INSTRUCTIONS);
    assert(hk_macho_section_is_code(flags));

    assert(hk_macho_section_flags(img, sizeof(img), 2, &flags) == HK_MACHO_OK);
    assert(flags == 0);
    assert(!hk_macho_section_is_code(flags));

    // Past the last section, and NO_SECT.
    assert(hk_macho_section_flags(img, sizeof(img), 3, &flags) == HK_MACHO_NOT_FOUND);
    assert(hk_macho_section_flags(img, sizeof(img), 0, &flags) == HK_MACHO_NOT_FOUND);
    printf("  section-flags-and-code-check: PASS\n");
}

static void test_runtime_address_code_classification(void) {
    _Alignas(8) uint8_t img[SEG_IMG_SIZE];
    const uint64_t base = 0x100000000ull;
    build_segment_image(img, base);
    size_t text = SEG_TEXT_OFF + HK_SEGMENT_COMMAND_64_SIZE;
    size_t data = text + HK_SECTION_64_SIZE;
    put_u64(img, text + 32, base + 0x400);
    put_u64(img, text + 40, 0x40);
    put_u64(img, data + 32, base + 0x500);
    put_u64(img, data + 40, 0x40);
    bool code = false;
    assert(hk_macho_runtime_address_is_code(
               img, sizeof(img), 0, base + 0x410, &code) == HK_MACHO_OK);
    assert(code);
    assert(hk_macho_runtime_address_is_code(
               img, sizeof(img), 0, base + 0x510, &code) == HK_MACHO_OK);
    assert(!code);
    assert(hk_macho_runtime_address_is_code(
               img, sizeof(img), 0, base + 0x900, &code) == HK_MACHO_NOT_FOUND);
    printf("  runtime-address-code-classification: PASS\n");
}

static void test_section_array_must_fit_in_cmdsize(void) {
    // A segment claiming more sections than its own cmdsize can hold would
    // walk off the end of the command.
    uint8_t *img = (uint8_t *)aligned_alloc(8, SEG_IMG_SIZE);  // heap: over-reads are ASan-visible
    assert(img != NULL);
    build_segment_image(img, 0x100000000ull);
    put_u32(img, SEG_TEXT_OFF + 64, 99);  // nsects=99 in a 232-byte command

    uint32_t flags = 0;
    assert(hk_macho_section_flags(img, sizeof(uint8_t) * SEG_IMG_SIZE, 1, &flags) == HK_MACHO_MALFORMED);

    // The case that makes this guard load-bearing rather than cosmetic:
    // section 50 of the claimed 99 would be read at ~4KB into a 1KB image.
    // Without the check that is a real out-of-bounds read, which is what the
    // teeth-check against a guard-less build confirms ASan reports.
    assert(hk_macho_section_flags(img, SEG_IMG_SIZE, 50, &flags) == HK_MACHO_MALFORMED);

    free(img);
    printf("  section-array-must-fit-in-cmdsize: PASS\n");
}

static void test_truncated_segment_command_rejected(void) {
    _Alignas(8) uint8_t img[SEG_IMG_SIZE];
    build_segment_image(img, 0x100000000ull);
    // Shrink __LINKEDIT below the 72-byte segment structure, keeping the
    // command list self-consistent so the failure is about the segment.
    put_u32(img, SEG_LE_OFF + 4, 8);
    put_u32(img, 20, SEG_TEXT_CMDSIZE + 8);
    put_u32(img, 16, 2);

    hk_macho_segment_t seg;
    assert(hk_macho_find_segment(img, sizeof(img), "__LINKEDIT", &seg) == HK_MACHO_MALFORMED);
    printf("  truncated-segment-command-rejected: PASS\n");
}

// ---- loaded-image translation -------------------------------------------

static void test_loaded_image_symtab_translation(void) {
    // The test that proves translation actually happens. A decoy symbol table
    // sits where FILE-offset logic would read (image + symoff) holding
    // n_value 0xDEAD; the real one sits where __LINKEDIT translation lands,
    // holding 0x4000. A missing or wrong translation returns 0xDEAD.
    _Alignas(8) uint8_t *img = (uint8_t *)aligned_alloc(8, SEG_IMG_SIZE);
    assert(img != NULL);

    // Model a real load: the image prefers text_vmaddr and is mapped at `img`,
    // so its slide is the difference. (Unsigned wraparound here is exact and
    // cancels in the translation, whichever way the addresses compare.)
    const uint64_t text_vmaddr = 0x100000000ull;
    build_segment_image(img, text_vmaddr);
    uintptr_t slide = (uintptr_t)img - (uintptr_t)text_vmaddr;

    hk_symbol_table_view_t view;
    assert(hk_macho_symtab_view_for_loaded_image(img, SEG_IMG_SIZE, slide, &view) == HK_MACHO_OK);

    // linkedit_base = slide + vmaddr(0x140) - fileoff(0x40) = img + 0x100,
    // so symoff 0x80 lands at img + 0x180 -- the real table, not the decoy.
    assert((const uint8_t *)view.nlist == img + REAL_NLIST_OFF);
    assert((const uint8_t *)view.strings == img + REAL_STR_OFF);
    assert(view.nlist_count == 1 && view.strings_size == 8);

    hk_symbol_query_t q;
    q.name = "alpha";
    q.convention = HK_SYMBOL_NAME_C;
    q.visibility = HK_SYMBOL_VISIBILITY_ANY;
    hk_symbol_match_t m;
    assert(hk_symbol_table_find(&view, &q, &m));
    assert(m.n_value == 0x4000);   // translated
    assert(m.n_value != 0xDEAD);   // and demonstrably not the file-offset decoy

    // The file-image path on the same bytes reaches the decoy, which is what
    // makes the two paths genuinely different rather than incidentally equal.
    hk_symbol_table_view_t file_view;
    assert(hk_macho_find_symtab_view(img, SEG_IMG_SIZE, &file_view) == HK_MACHO_OK);
    assert(hk_symbol_table_find(&file_view, &q, &m));
    assert(m.n_value == 0xDEAD);

    free(img);
    printf("  loaded-image-symtab-translation: PASS\n");
}

static void test_loaded_image_validates_against_linkedit(void) {
    _Alignas(8) uint8_t img[SEG_IMG_SIZE];
    hk_symbol_table_view_t view;
    const uint64_t V = 0x100000000ull;
    uintptr_t slide = (uintptr_t)img - (uintptr_t)V;

    // symoff before __LINKEDIT's file range.
    build_segment_image(img, V);
    put_u32(img, SEG_SYMTAB_OFF + 8, 0x10);  // < fileoff 0x40
    assert(hk_macho_symtab_view_for_loaded_image(img, SEG_IMG_SIZE, slide, &view) == HK_MACHO_MALFORMED);

    // nlist span running past __LINKEDIT's end.
    build_segment_image(img, V);
    put_u32(img, SEG_SYMTAB_OFF + 12, 0xFFFFFF);
    assert(hk_macho_symtab_view_for_loaded_image(img, SEG_IMG_SIZE, slide, &view) == HK_MACHO_MALFORMED);

    // string table running past __LINKEDIT's end.
    build_segment_image(img, V);
    put_u32(img, SEG_SYMTAB_OFF + 20, 0xFFFFFF);
    assert(hk_macho_symtab_view_for_loaded_image(img, SEG_IMG_SIZE, slide, &view) == HK_MACHO_MALFORMED);

    // No __LINKEDIT at all: report it as missing, not as a bad range.
    _Alignas(8) uint8_t bare[IMG_SIZE];
    build_symtab_image(bare);
    assert(hk_macho_symtab_view_for_loaded_image(bare, IMG_SIZE, 0, &view) == HK_MACHO_NOT_FOUND);
    printf("  loaded-image-validates-against-linkedit: PASS\n");
}


typedef struct { char names[4][17]; uint32_t n; } segs_t;
static bool seg_cb(void *ctx, uint32_t index, const hk_macho_segment_t *seg) {
    segs_t *g = (segs_t *)ctx;
    if (g->n < 4) { memcpy(g->names[g->n], seg->segname, 17); }
    assert(index == g->n);   // indices are dense and in load-command order
    g->n++;
    return true;
}
static bool seg_stop_cb(void *ctx, uint32_t index, const hk_macho_segment_t *seg) {
    (void)index; (void)seg;
    (*(uint32_t *)ctx)++;
    return false;  // stop after the first
}

static void test_iterate_segments(void) {
    _Alignas(8) uint8_t img[SEG_IMG_SIZE];
    build_segment_image(img, 0x100000000ull);

    segs_t g;
    memset(&g, 0, sizeof(g));
    assert(hk_macho_iterate_segments(img, sizeof(img), seg_cb, &g) == HK_MACHO_OK);
    assert(g.n == 2);
    assert(strcmp(g.names[0], "__TEXT") == 0);
    assert(strcmp(g.names[1], "__LINKEDIT") == 0);

    uint32_t seen = 0;
    assert(hk_macho_iterate_segments(img, sizeof(img), seg_stop_cb, &seen) == HK_MACHO_OK);
    assert(seen == 1);  // early stop honored

    // A truncated segment command is reported, not silently skipped.
    _Alignas(8) uint8_t bad[SEG_IMG_SIZE];
    build_segment_image(bad, 0x100000000ull);
    put_u32(bad, SEG_LE_OFF + 4, 8);
    put_u32(bad, 20, SEG_TEXT_CMDSIZE + 8);
    put_u32(bad, 16, 2);
    memset(&g, 0, sizeof(g));
    assert(hk_macho_iterate_segments(bad, sizeof(bad), seg_cb, &g) == HK_MACHO_MALFORMED);

    assert(hk_macho_iterate_segments(NULL, 16, seg_cb, &g) == HK_MACHO_NOT_MACHO);
    assert(hk_macho_iterate_segments(img, sizeof(img), NULL, &g) == HK_MACHO_NOT_MACHO);
    printf("  iterate-segments: PASS\n");
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

// ---- peek header --------------------------------------------------------

// peek_header exists to break a cycle: a loaded image's safe read bound is
// HK_MACHO_HEADER_64_SIZE + sizeofcmds, which is only readable from the header,
// but read_header validates that region against the size you are still trying
// to compute. So the case that matters is exactly the one read_header rejects.
static void test_peek_header_without_the_command_region(void) {
    uint8_t img[SEG_IMG_SIZE];
    build_segment_image(img, 0x100000000ull);

    // Only the fixed header is readable. read_header refuses...
    hk_macho_header_t h;
    assert(hk_macho_read_header(img, HK_MACHO_HEADER_64_SIZE, &h) == HK_MACHO_MALFORMED);
    // ...peek_header answers, and its answer is what makes the real bound
    // computable.
    memset(&h, 0, sizeof(h));
    assert(hk_macho_peek_header(img, HK_MACHO_HEADER_64_SIZE, &h) == HK_MACHO_OK);
    assert(h.magic == HK_MH_MAGIC_64);
    assert(h.ncmds == 3);
    assert(h.sizeofcmds == SEG_SIZEOFCMDS);

    const size_t bound = HK_MACHO_HEADER_64_SIZE + h.sizeofcmds;
    // The derived bound is exactly what read_header now accepts, and one byte
    // less is not -- so the derivation is tight, not merely sufficient.
    assert(hk_macho_read_header(img, bound, &h) == HK_MACHO_OK);
    assert(hk_macho_read_header(img, bound - 1, &h) == HK_MACHO_MALFORMED);
    // And it is enough for a real load-command walk.
    uintptr_t start = 0, end = 0;
    assert(hk_macho_image_span_for_loaded_image(img, bound, 0, &start, &end) == HK_MACHO_OK);
    printf("  peek-header-without-the-command-region: PASS\n");
}

// A narrower check, not a laxer one: everything read_header rejects on the
// magic or the fixed-header size, peek_header rejects identically.
static void test_peek_header_is_not_laxer(void) {
    uint8_t img[SEG_IMG_SIZE];
    build_segment_image(img, 0x100000000ull);
    hk_macho_header_t h;

    assert(hk_macho_peek_header(NULL, SEG_IMG_SIZE, &h) == HK_MACHO_NOT_MACHO);
    assert(hk_macho_peek_header(img, SEG_IMG_SIZE, NULL) == HK_MACHO_NOT_MACHO);
    // Too small for even the magic, then too small for the fixed header.
    assert(hk_macho_peek_header(img, 2, &h) == HK_MACHO_TOO_SMALL);
    assert(hk_macho_peek_header(img, HK_MACHO_HEADER_64_SIZE - 1, &h) == HK_MACHO_TOO_SMALL);

    // Every magic read_header discriminates, peek_header discriminates the
    // same way -- checked against read_header directly so the two cannot drift.
    const uint32_t magics[] = {HK_MH_CIGAM_64, HK_MH_MAGIC, HK_MH_CIGAM,
                               HK_FAT_MAGIC, HK_FAT_CIGAM, 0xDEADBEEFu};
    for (size_t i = 0; i < sizeof(magics) / sizeof(magics[0]); i++) {
        uint8_t bad[SEG_IMG_SIZE];
        build_segment_image(bad, 0x100000000ull);
        put_u32(bad, 0, magics[i]);
        hk_macho_status_t peeked = hk_macho_peek_header(bad, SEG_IMG_SIZE, &h);
        hk_macho_status_t read = hk_macho_read_header(bad, SEG_IMG_SIZE, &h);
        assert(peeked != HK_MACHO_OK);
        assert(peeked == read);
    }
    printf("  peek-header-is-not-laxer: PASS\n");
}

// ---- image span ---------------------------------------------------------

// The span is what answers "does this address belong to this image", so both
// ends and the slide have to be right. The fixture's __TEXT is
// [text_vmaddr, +0x1000) and __LINKEDIT is [text_vmaddr+0x140, +0x200), so
// __TEXT alone determines the span -- __LINKEDIT is entirely inside it.
static void test_image_span(void) {
    uint8_t img[SEG_IMG_SIZE];
    const uint64_t base = 0x100000000ull;
    build_segment_image(img, base);

    uintptr_t start = 0, end = 0;
    assert(hk_macho_image_span_for_loaded_image(img, SEG_IMG_SIZE, 0, &start, &end)
           == HK_MACHO_OK);
    assert(start == (uintptr_t)base);
    assert(end == (uintptr_t)(base + 0x1000));

    // The slide moves both ends by exactly the slide, which is the whole point
    // of taking one: a catalog entry records where an image actually landed.
    const uintptr_t slide = 0x7F000000u;
    assert(hk_macho_image_span_for_loaded_image(img, SEG_IMG_SIZE, slide, &start, &end)
           == HK_MACHO_OK);
    assert(start == (uintptr_t)base + slide);
    assert(end == (uintptr_t)(base + 0x1000) + slide);

    assert(hk_macho_image_span_for_loaded_image(NULL, SEG_IMG_SIZE, 0, &start, &end)
           == HK_MACHO_NOT_MACHO);
    assert(hk_macho_image_span_for_loaded_image(img, SEG_IMG_SIZE, 0, NULL, &end)
           == HK_MACHO_NOT_MACHO);
    printf("  image-span: PASS\n");
}

// __PAGEZERO is the reason this function cannot just take min(vmaddr): the
// main executable maps it at 0 with a huge vmsize so null-ish dereferences
// fault. Counting it would start the span at 0 and make almost any address
// "inside" the image -- the exact opposite of a containment check.
static void test_image_span_excludes_pagezero(void) {
    uint8_t img[SEG_IMG_SIZE];
    const uint64_t base = 0x100000000ull;
    build_segment_image(img, base);

    // Turn __LINKEDIT into a __PAGEZERO at vmaddr 0 spanning 4GB, exactly as a
    // real main executable declares it.
    write_segment(img, SEG_LE_OFF, "__PAGEZERO", HK_SEGMENT_COMMAND_64_SIZE,
                  0, 0x100000000ull, 0, 0, 0);

    uintptr_t start = 0, end = 0;
    assert(hk_macho_image_span_for_loaded_image(img, SEG_IMG_SIZE, 0, &start, &end)
           == HK_MACHO_OK);
    // __TEXT alone, not [0, 4GB).
    assert(start == (uintptr_t)base);
    assert(end == (uintptr_t)(base + 0x1000));

    // A zero-length segment maps nothing and must not widen the span either.
    build_segment_image(img, base);
    write_segment(img, SEG_LE_OFF, "__HUGE", HK_SEGMENT_COMMAND_64_SIZE,
                  base - 0x9000, 0, 0, 0, 0);
    assert(hk_macho_image_span_for_loaded_image(img, SEG_IMG_SIZE, 0, &start, &end)
           == HK_MACHO_OK);
    assert(start == (uintptr_t)base);
    printf("  image-span-excludes-pagezero: PASS\n");
}

// A segment BELOW __TEXT and one ABOVE __TEXT each have to move their own end
// of the span -- a min/max that only ever tracked one direction would pass the
// fixture above by accident, since there __TEXT happens to be both.
static void test_image_span_tracks_both_ends(void) {
    uint8_t img[SEG_IMG_SIZE];
    const uint64_t base = 0x100000000ull;

    build_segment_image(img, base);
    write_segment(img, SEG_LE_OFF, "__BELOW", HK_SEGMENT_COMMAND_64_SIZE,
                  base - 0x2000, 0x1000, 0x40, 0x200, 0);
    uintptr_t start = 0, end = 0;
    assert(hk_macho_image_span_for_loaded_image(img, SEG_IMG_SIZE, 0, &start, &end)
           == HK_MACHO_OK);
    assert(start == (uintptr_t)(base - 0x2000));   // the low segment moved start
    assert(end == (uintptr_t)(base + 0x1000));     // __TEXT still ends it

    build_segment_image(img, base);
    write_segment(img, SEG_LE_OFF, "__ABOVE", HK_SEGMENT_COMMAND_64_SIZE,
                  base + 0x5000, 0x1000, 0x40, 0x200, 0);
    assert(hk_macho_image_span_for_loaded_image(img, SEG_IMG_SIZE, 0, &start, &end)
           == HK_MACHO_OK);
    assert(start == (uintptr_t)base);              // __TEXT still starts it
    assert(end == (uintptr_t)(base + 0x6000));     // the high segment moved end
    printf("  image-span-tracks-both-ends: PASS\n");
}

int main(void) {
    test_valid_header();
    test_selects_thin_fat32_and_fat64_slices();
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
    test_find_segment();
    test_segname_is_not_read_as_a_c_string();
    test_section_flags_and_code_check();
    test_runtime_address_code_classification();
    test_section_array_must_fit_in_cmdsize();
    test_truncated_segment_command_rejected();
    test_loaded_image_symtab_translation();
    test_loaded_image_validates_against_linkedit();
    test_iterate_segments();
    test_peek_header_without_the_command_region();
    test_peek_header_is_not_laxer();
    test_image_span();
    test_image_span_excludes_pagezero();
    test_image_span_tracks_both_ends();
    test_null_tolerance();
    printf("all mach-o tests passed\n");
    return 0;
}
