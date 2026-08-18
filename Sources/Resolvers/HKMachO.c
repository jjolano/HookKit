// Mach-O container parsing. See HKMachO.h for the contract, the reuse
// survey, and the two robustness properties (bounded reads, alignment-safe
// reads) this implementation exists to guarantee.

#include "HKMachO.h"

#include <string.h>

// All multi-byte reads go through memcpy rather than a struct-pointer cast:
// the caller's buffer may be at any alignment, and a misaligned load through
// a `uint32_t *` is undefined behaviour (UBSan traps it). The compiler folds
// these back into single loads where the alignment is known.
static uint32_t read_u32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint64_t read_u64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

hk_macho_status_t hk_macho_read_header(const void *image, size_t size,
                                       hk_macho_header_t *out_header) {
    if (!image || !out_header) {
        return HK_MACHO_NOT_MACHO;
    }
    if (size < sizeof(uint32_t)) {
        return HK_MACHO_TOO_SMALL;
    }
    const uint8_t *base = (const uint8_t *)image;

    uint32_t magic = read_u32(base);
    switch (magic) {
    case HK_MH_MAGIC_64:
        break;  // the only supported form
    case HK_MH_CIGAM_64:
        return HK_MACHO_BYTE_ORDER_UNSUPPORTED;
    case HK_MH_MAGIC:
    case HK_MH_CIGAM:
        return HK_MACHO_NOT_64_BIT;
    case HK_FAT_MAGIC:
    case HK_FAT_CIGAM:
        return HK_MACHO_FAT_UNSUPPORTED;
    default:
        return HK_MACHO_NOT_MACHO;
    }

    if (size < HK_MACHO_HEADER_64_SIZE) {
        return HK_MACHO_TOO_SMALL;
    }

    out_header->magic      = magic;
    out_header->cputype    = read_u32(base + 4);
    out_header->cpusubtype = read_u32(base + 8);
    out_header->filetype   = read_u32(base + 12);
    out_header->ncmds      = read_u32(base + 16);
    out_header->sizeofcmds = read_u32(base + 20);
    out_header->flags      = read_u32(base + 24);
    // base + 28 is `reserved`, deliberately not surfaced.

    // The whole load-command region must lie inside the buffer. Written as a
    // subtraction so no addition can overflow.
    if (out_header->sizeofcmds > size - HK_MACHO_HEADER_64_SIZE) {
        return HK_MACHO_MALFORMED;
    }
    return HK_MACHO_OK;
}

hk_macho_status_t hk_macho_iterate_load_commands(const void *image, size_t size,
                                                 hk_macho_lc_visit_fn visit,
                                                 void *ctx) {
    if (!visit) {
        return HK_MACHO_NOT_MACHO;
    }
    hk_macho_header_t header;
    hk_macho_status_t status = hk_macho_read_header(image, size, &header);
    if (status != HK_MACHO_OK) {
        return status;
    }
    const uint8_t *base = (const uint8_t *)image;

    size_t offset = HK_MACHO_HEADER_64_SIZE;
    const size_t end = HK_MACHO_HEADER_64_SIZE + header.sizeofcmds;  // validated to fit

    for (uint32_t i = 0; i < header.ncmds; i++) {
        // Invariant: offset <= end, so `end - offset` never underflows.
        if (end - offset < HK_LOAD_COMMAND_SIZE) {
            return HK_MACHO_MALFORMED;  // ncmds claims more than sizeofcmds holds
        }
        uint32_t cmd = read_u32(base + offset);
        uint32_t cmdsize = read_u32(base + offset + 4);

        // cmdsize < 8 would mean the cursor never advances -- reject before
        // any chance of an infinite loop. 64-bit Mach-O also requires
        // 8-byte-aligned command sizes.
        if (cmdsize < HK_LOAD_COMMAND_SIZE || (cmdsize % 8u) != 0u) {
            return HK_MACHO_MALFORMED;
        }
        if (cmdsize > end - offset) {
            return HK_MACHO_MALFORMED;  // command overruns the region
        }

        if (!visit(ctx, cmd, offset, cmdsize)) {
            return HK_MACHO_OK;  // caller-requested early stop, not an error
        }
        offset += cmdsize;
    }
    return HK_MACHO_OK;
}

typedef struct {
    uint32_t wanted;
    size_t offset;
    uint32_t cmdsize;
    bool found;
} find_lc_ctx_t;

static bool find_lc_visit(void *ctx, uint32_t cmd, size_t offset, uint32_t cmdsize) {
    find_lc_ctx_t *f = (find_lc_ctx_t *)ctx;
    if (cmd == f->wanted) {
        f->offset = offset;
        f->cmdsize = cmdsize;
        f->found = true;
        return false;  // first match wins; stop
    }
    return true;
}

hk_macho_status_t hk_macho_find_load_command(const void *image, size_t size,
                                             uint32_t cmd, size_t *out_offset,
                                             uint32_t *out_cmdsize) {
    if (!out_offset || !out_cmdsize) {
        return HK_MACHO_NOT_MACHO;
    }
    find_lc_ctx_t f;
    f.wanted = cmd;
    f.offset = 0;
    f.cmdsize = 0;
    f.found = false;

    hk_macho_status_t status = hk_macho_iterate_load_commands(image, size, find_lc_visit, &f);
    if (status != HK_MACHO_OK) {
        return status;
    }
    if (!f.found) {
        return HK_MACHO_NOT_FOUND;
    }
    *out_offset = f.offset;
    *out_cmdsize = f.cmdsize;
    return HK_MACHO_OK;
}

hk_macho_status_t hk_macho_find_symtab_view(const void *image, size_t size,
                                            hk_symbol_table_view_t *out_view) {
    if (!out_view) {
        return HK_MACHO_NOT_MACHO;
    }
    size_t offset = 0;
    uint32_t cmdsize = 0;
    hk_macho_status_t status =
        hk_macho_find_load_command(image, size, HK_LC_SYMTAB, &offset, &cmdsize);
    if (status != HK_MACHO_OK) {
        return status;
    }
    if (cmdsize < HK_SYMTAB_COMMAND_SIZE) {
        return HK_MACHO_MALFORMED;  // truncated LC_SYMTAB
    }

    const uint8_t *base = (const uint8_t *)image;
    uint32_t symoff  = read_u32(base + offset + 8);
    uint32_t nsyms   = read_u32(base + offset + 12);
    uint32_t stroff  = read_u32(base + offset + 16);
    uint32_t strsize = read_u32(base + offset + 20);

    // Both referenced ranges must lie inside the image. The nlist span is
    // computed in 64 bits so a large nsyms cannot wrap.
    uint64_t nlist_bytes = (uint64_t)nsyms * (uint64_t)sizeof(hk_macho_nlist64_t);
    if (symoff > size || nlist_bytes > (uint64_t)(size - symoff)) {
        return HK_MACHO_MALFORMED;
    }
    if (stroff > size || strsize > size - stroff) {
        return HK_MACHO_MALFORMED;
    }

    // The view hands out a TYPED nlist pointer, which callers dereference --
    // so unlike this file's own memcpy-based reads, it genuinely requires
    // 8-byte alignment (hk_macho_nlist64_t contains a uint64_t). Any real
    // linker-produced image satisfies this; a hostile or corrupt one might
    // not, and a misaligned load is undefined behaviour, so reject rather
    // than hand back a pointer that is unsound to use.
    if ((((uintptr_t)base + symoff) % _Alignof(hk_macho_nlist64_t)) != 0) {
        return HK_MACHO_MALFORMED;
    }

    out_view->nlist = (const hk_macho_nlist64_t *)(const void *)(base + symoff);
    out_view->nlist_count = nsyms;
    out_view->strings = (const char *)(base + stroff);
    out_view->strings_size = strsize;
    return HK_MACHO_OK;
}

// ---- segments and sections ---------------------------------------------

// Reads a segment command's fields. `offset`/`cmdsize` come from the bounded
// iteration, so the command itself is known to be inside the image; this only
// has to check that the command is large enough to hold the structure.
static bool read_segment(const uint8_t *base, size_t offset, uint32_t cmdsize,
                         hk_macho_segment_t *out) {
    if (cmdsize < HK_SEGMENT_COMMAND_64_SIZE) {
        return false;
    }
    // segname is a fixed 16-byte field that need not be NUL-terminated, so it
    // is copied out and terminated rather than ever read as a C string.
    memcpy(out->segname, base + offset + 8, 16);
    out->segname[16] = '\0';

    out->vmaddr   = read_u64(base + offset + 24);
    out->vmsize   = read_u64(base + offset + 32);
    out->fileoff  = read_u64(base + offset + 40);
    out->filesize = read_u64(base + offset + 48);
    out->maxprot  = read_u32(base + offset + 56);
    out->initprot = read_u32(base + offset + 60);
    out->nsects   = read_u32(base + offset + 64);
    out->flags    = read_u32(base + offset + 68);
    out->command_offset = offset;
    out->cmdsize = cmdsize;
    return true;
}

// A segment's section array lives inside the segment command itself, so it
// must fit within that command's cmdsize. 2.x omits this check because it
// only runs on dyld-validated images; a parser handed arbitrary bytes cannot.
static bool sections_fit(const hk_macho_segment_t *seg) {
    uint64_t need = (uint64_t)HK_SEGMENT_COMMAND_64_SIZE +
                    (uint64_t)seg->nsects * (uint64_t)HK_SECTION_64_SIZE;
    return need <= (uint64_t)seg->cmdsize;
}

typedef struct {
    const uint8_t *base;
    const char *wanted;
    hk_macho_segment_t segment;
    bool found;
    bool malformed;
} find_segment_ctx_t;

static bool find_segment_visit(void *ctx, uint32_t cmd, size_t offset, uint32_t cmdsize) {
    find_segment_ctx_t *f = (find_segment_ctx_t *)ctx;
    if (cmd != HK_LC_SEGMENT_64) {
        return true;
    }
    hk_macho_segment_t seg;
    if (!read_segment(f->base, offset, cmdsize, &seg)) {
        f->malformed = true;
        return false;
    }
    if (strcmp(seg.segname, f->wanted) == 0) {
        f->segment = seg;
        f->found = true;
        return false;
    }
    return true;
}

hk_macho_status_t hk_macho_find_segment(const void *image, size_t size,
                                        const char *segname,
                                        hk_macho_segment_t *out_segment) {
    if (!image || !segname || !out_segment) {
        return HK_MACHO_NOT_MACHO;
    }
    find_segment_ctx_t f;
    f.base = (const uint8_t *)image;
    f.wanted = segname;
    f.found = false;
    f.malformed = false;
    memset(&f.segment, 0, sizeof(f.segment));

    hk_macho_status_t status =
        hk_macho_iterate_load_commands(image, size, find_segment_visit, &f);
    if (status != HK_MACHO_OK) {
        return status;
    }
    if (f.malformed) {
        return HK_MACHO_MALFORMED;
    }
    if (!f.found) {
        return HK_MACHO_NOT_FOUND;
    }
    *out_segment = f.segment;
    return HK_MACHO_OK;
}

typedef struct {
    const uint8_t *base;
    uint32_t wanted;    // 1-based section index
    uint32_t running;   // sections counted in preceding segments
    uint32_t flags;
    bool found;
    bool malformed;
} section_ctx_t;

static bool section_visit(void *ctx, uint32_t cmd, size_t offset, uint32_t cmdsize) {
    section_ctx_t *s = (section_ctx_t *)ctx;
    if (cmd != HK_LC_SEGMENT_64) {
        return true;
    }
    hk_macho_segment_t seg;
    if (!read_segment(s->base, offset, cmdsize, &seg) || !sections_fit(&seg)) {
        s->malformed = true;
        return false;
    }
    if (s->wanted <= s->running + seg.nsects) {
        uint32_t index_in_segment = s->wanted - s->running - 1;  // 0-based
        size_t section_offset = offset + HK_SEGMENT_COMMAND_64_SIZE +
                                (size_t)index_in_segment * HK_SECTION_64_SIZE;
        s->flags = read_u32(s->base + section_offset + 64);  // section_64.flags
        s->found = true;
        return false;
    }
    s->running += seg.nsects;
    return true;
}

hk_macho_status_t hk_macho_section_flags(const void *image, size_t size,
                                         uint8_t n_sect, uint32_t *out_flags) {
    if (!image || !out_flags) {
        return HK_MACHO_NOT_MACHO;
    }
    if (n_sect == 0) {
        return HK_MACHO_NOT_FOUND;  // NO_SECT: the symbol is in no section
    }
    section_ctx_t s;
    s.base = (const uint8_t *)image;
    s.wanted = n_sect;
    s.running = 0;
    s.flags = 0;
    s.found = false;
    s.malformed = false;

    hk_macho_status_t status =
        hk_macho_iterate_load_commands(image, size, section_visit, &s);
    if (status != HK_MACHO_OK) {
        return status;
    }
    if (s.malformed) {
        return HK_MACHO_MALFORMED;
    }
    if (!s.found) {
        return HK_MACHO_NOT_FOUND;  // fewer sections than n_sect claims
    }
    *out_flags = s.flags;
    return HK_MACHO_OK;
}

// ---- loaded-image symbol table -----------------------------------------

hk_macho_status_t hk_macho_symtab_view_for_loaded_image(const void *header,
                                                        size_t header_region_size,
                                                        uintptr_t slide,
                                                        hk_symbol_table_view_t *out_view) {
    if (!header || !out_view) {
        return HK_MACHO_NOT_MACHO;
    }

    hk_macho_segment_t linkedit;
    hk_macho_status_t status =
        hk_macho_find_segment(header, header_region_size, "__LINKEDIT", &linkedit);
    if (status != HK_MACHO_OK) {
        return status;
    }

    size_t cmd_offset = 0;
    uint32_t cmdsize = 0;
    status = hk_macho_find_load_command(header, header_region_size, HK_LC_SYMTAB,
                                        &cmd_offset, &cmdsize);
    if (status != HK_MACHO_OK) {
        return status;
    }
    if (cmdsize < HK_SYMTAB_COMMAND_SIZE) {
        return HK_MACHO_MALFORMED;
    }

    const uint8_t *base = (const uint8_t *)header;
    uint64_t symoff  = read_u32(base + cmd_offset + 8);
    uint64_t nsyms   = read_u32(base + cmd_offset + 12);
    uint64_t stroff  = read_u32(base + cmd_offset + 16);
    uint64_t strsize = read_u32(base + cmd_offset + 20);

    // Both tables must lie inside __LINKEDIT's declared file range, so a
    // corrupt LC_SYMTAB cannot point at unrelated memory.
    if (linkedit.filesize > UINT64_MAX - linkedit.fileoff) {
        return HK_MACHO_MALFORMED;
    }
    uint64_t le_start = linkedit.fileoff;
    uint64_t le_end = linkedit.fileoff + linkedit.filesize;

    uint64_t nlist_bytes = nsyms * (uint64_t)sizeof(hk_macho_nlist64_t);  // nsyms is 32-bit; no overflow in 64
    if (symoff < le_start || symoff > le_end || nlist_bytes > le_end - symoff) {
        return HK_MACHO_MALFORMED;
    }
    if (stroff < le_start || stroff > le_end || strsize > le_end - stroff) {
        return HK_MACHO_MALFORMED;
    }

    // linkedit_base = slide + vmaddr - fileoff. The subtraction is done first
    // and guarded: a fileoff larger than vmaddr is malformed, not a wrap.
    if (linkedit.fileoff > linkedit.vmaddr) {
        return HK_MACHO_MALFORMED;
    }
    uintptr_t linkedit_base = slide + (uintptr_t)(linkedit.vmaddr - linkedit.fileoff);
    uintptr_t nlist_addr = linkedit_base + (uintptr_t)symoff;

    // Same typed-pointer alignment requirement as the file-image path.
    if ((nlist_addr % _Alignof(hk_macho_nlist64_t)) != 0) {
        return HK_MACHO_MALFORMED;
    }

    out_view->nlist = (const hk_macho_nlist64_t *)(const void *)nlist_addr;
    out_view->nlist_count = (uint32_t)nsyms;
    out_view->strings = (const char *)(linkedit_base + (uintptr_t)stroff);
    out_view->strings_size = (size_t)strsize;
    return HK_MACHO_OK;
}
