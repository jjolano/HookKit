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
