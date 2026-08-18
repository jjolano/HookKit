// Milestone 5 conformance harness: runs every resolver over a REAL Mach-O
// image and reports what it found.
//
// Why this exists. Every parser in Sources/Resolvers/ is verified against
// synthetic fixtures written by the same person who wrote the parser. That
// catches logic errors -- it has caught several -- but it cannot tell you
// whether a binary Apple actually ships parses correctly, because the fixture
// and the parser share every assumption. This tool closes that gap by running
// the parsers over binaries pulled off a real device, so the input is one
// nobody here constructed.
//
// It is a TOOL, not a test: the specimens are third-party binaries and are not
// committed. Run it against images pulled from a device (see
// docs/3.0/IMPLEMENTATION_STATUS.md for how the run was done and what it
// found). Cross-check its output against nm/otool for independent ground truth.
//
//   cc -o macho_conformance Tools/conformance/macho_conformance.c \
//      Sources/Resolvers/*.c && ./macho_conformance <image> [symbol...]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../../Sources/Resolvers/HKChainedFixups.h"
#include "../../Sources/Resolvers/HKImportSlots.h"
#include "../../Sources/Resolvers/HKMachO.h"
#include "../../Sources/Resolvers/HKSymbolResolve.h"

// Fat handling lives HERE, in the tool, not in the library: the library
// correctly refuses a universal binary (HK_MACHO_FAT_UNSUPPORTED) because a
// caller must choose a slice first. Real shipped binaries are usually fat, so
// this is the division doing its job, not a gap.
#define FAT_MAGIC_BE 0xbebafecau  // 0xcafebabe seen little-endian
#define CPU_TYPE_ARM64 0x0100000cu

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// Returns the offset/size of the first arm64 slice, or the whole file if thin.
static bool select_slice(const uint8_t *buf, size_t size, size_t *out_off,
                         size_t *out_size, const char **out_note) {
    *out_off = 0;
    *out_size = size;
    *out_note = "thin";
    if (size < 8) {
        return true;
    }
    uint32_t magic;
    memcpy(&magic, buf, 4);
    if (magic != FAT_MAGIC_BE) {
        return true;
    }
    uint32_t n = be32(buf + 4);
    for (uint32_t i = 0; i < n; i++) {
        size_t e = 8 + (size_t)i * 20;
        if (e + 20 > size) {
            return false;
        }
        uint32_t cputype = be32(buf + e);
        uint32_t cpusub = be32(buf + e + 4);
        uint32_t off = be32(buf + e + 8);
        uint32_t len = be32(buf + e + 12);
        if (cputype == CPU_TYPE_ARM64 && (cpusub & 0xffffffu) == 0) {
            if ((size_t)off + len > size) {
                return false;
            }
            *out_off = off;
            *out_size = len;
            *out_note = "fat: selected arm64 slice";
            return true;
        }
    }
    return false;
}

static int count_sections(const void *img, size_t size);
static bool count_section_cb(void *ctx, uint8_t n_sect, const hk_macho_section_t *s) {
    (void)n_sect; (void)s;
    (*(int *)ctx)++;
    return true;
}
static int count_sections(const void *img, size_t size) {
    int n = 0;
    hk_macho_iterate_sections(img, size, count_section_cb, &n);
    return n;
}

typedef struct { int count; char first[128]; } slot_tally_t;
static bool slot_cb(void *ctx, const hk_import_slot_t *slot) {
    slot_tally_t *t = (slot_tally_t *)ctx;
    if (t->count == 0) {
        snprintf(t->first, sizeof(t->first), "%s @ 0x%llx (%s)",
                 slot->symbol_name, (unsigned long long)slot->slot_vmaddr,
                 slot->is_lazy ? "lazy" : "non-lazy");
    }
    t->count++;
    return true;
}

typedef struct { int count; uint32_t first_ordinal; uint64_t first_off; } bind_tally_t;
static bool bind_cb(void *ctx, const hk_chained_bind_t *b) {
    bind_tally_t *t = (bind_tally_t *)ctx;
    if (t->count == 0) {
        t->first_ordinal = b->import_ordinal;
        t->first_off = b->slot_image_offset;
    }
    t->count++;
    return true;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <mach-o image> [symbol...]\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 2; }
    struct stat st;
    if (stat(argv[1], &st) != 0) { perror("stat"); fclose(f); return 2; }
    size_t file_size = (size_t)st.st_size;
    uint8_t *file_buf = (uint8_t *)malloc(file_size);
    if (!file_buf || fread(file_buf, 1, file_size, f) != file_size) {
        fprintf(stderr, "read failed\n"); fclose(f); free(file_buf); return 2;
    }
    fclose(f);

    size_t off = 0, size = 0;
    const char *note = "";
    if (!select_slice(file_buf, file_size, &off, &size, &note)) {
        fprintf(stderr, "no arm64 slice\n"); free(file_buf); return 1;
    }
    const uint8_t *img = file_buf + off;
    printf("== %s\n   %zu bytes, %s\n", argv[1], file_size, note);

    hk_macho_header_t hdr;
    hk_macho_status_t ms = hk_macho_read_header(img, size, &hdr);
    printf("   header:            status=%d filetype=%u ncmds=%u sizeofcmds=%u\n",
           ms, hdr.filetype, hdr.ncmds, hdr.sizeofcmds);
    if (ms != HK_MACHO_OK) { free(file_buf); return 1; }

    hk_macho_segment_t seg;
    printf("   __TEXT:            status=%d\n",
           hk_macho_find_segment(img, size, "__TEXT", &seg));
    ms = hk_macho_find_segment(img, size, "__LINKEDIT", &seg);
    printf("   __LINKEDIT:        status=%d fileoff=0x%llx filesize=0x%llx\n",
           ms, (unsigned long long)seg.fileoff, (unsigned long long)seg.filesize);
    printf("   sections:          %d\n", count_sections(img, size));

    hk_symbol_table_view_t sym;
    ms = hk_macho_find_symtab_view(img, size, &sym);
    printf("   LC_SYMTAB:         status=%d nsyms=%u strsize=%zu\n",
           ms, sym.nlist_count, sym.strings_size);

    size_t t_off = 0, t_size = 0;
    ms = hk_macho_find_export_trie(img, size, &t_off, &t_size);
    printf("   export trie:       status=%d size=%zu\n", ms, t_size);

    // Import slots via the older LC_DYSYMTAB mechanism.
    hk_import_tables_t tables;
    hk_import_status_t is = hk_import_tables_from_file_image(img, size, &tables);
    slot_tally_t slots = {0, ""};
    hk_import_status_t iter = HK_IMPORT_OK;
    if (is == HK_IMPORT_OK && tables.indirect_symbols) {
        iter = hk_import_slots_iterate(img, size, &tables, slot_cb, &slots);
    }
    printf("   indirect symbols:  status=%d n=%u  slots: status=%d n=%d\n",
           is, tables.indirect_count, iter, slots.count);
    if (slots.count) printf("     first slot:      %s\n", slots.first);

    // Chained fixups: the modern mechanism.
    size_t c_off = 0, c_size = 0;
    ms = hk_macho_find_chained_fixups(img, size, &c_off, &c_size);
    printf("   chained fixups:    status=%d size=%zu\n", ms, c_size);
    if (ms == HK_MACHO_OK) {
        hk_chained_fixups_t cf;
        hk_chained_status_t cs = hk_chained_fixups_parse(img + c_off, c_size, &cf);
        printf("     parse:           status=%d imports=%u fmt=%u syms_fmt=%u\n",
               cs, cf.imports_count, cf.imports_format, cf.symbols_format);
        if (cs == HK_CHAINED_OK) {
            for (uint32_t i = 0; i < cf.imports_count && i < 3; i++) {
                hk_chained_import_t im;
                if (hk_chained_import_at(&cf, i, &im) == HK_CHAINED_OK) {
                    printf("     import[%u]:       %-28s ord=%d weak=%d\n",
                           i, im.symbol_name, im.lib_ordinal, im.weak_import);
                }
            }
            // Traversal needs the LOADED layout; a file image's segment_offset
            // is a VM offset, so this is expected to be inconsistent here and
            // is reported rather than hidden.
            bind_tally_t binds = {0, 0, 0};
            hk_chained_status_t bs =
                hk_chained_fixups_iterate_binds(&cf, img, size, bind_cb, &binds);
            printf("     binds (file lay): status=%d n=%d\n", bs, binds.count);
        }
    }

    // Symbol resolution over whichever sources this image actually has.
    hk_symbol_sources_t sources;
    hk_resolve_status_t rs = hk_symbol_sources_from_file_image(img, size, &sources);
    printf("   sources:           status=%d symtab=%s trie=%s\n", rs,
           sources.symbol_table.nlist ? "yes" : "no",
           sources.export_trie ? "yes" : "no");
    for (int i = 2; i < argc; i++) {
        hk_symbol_resolution_t r;
        hk_resolve_status_t st = hk_resolve_symbol(&sources, argv[i], HK_SYMBOL_NAME_C,
                                                   HK_SYMBOL_VISIBILITY_ANY, &r);
        printf("     resolve %-24s status=%d src=%d raw=0x%llx\n",
               argv[i], st, r.source, (unsigned long long)r.raw_value);
    }

    free(file_buf);
    return 0;
}
