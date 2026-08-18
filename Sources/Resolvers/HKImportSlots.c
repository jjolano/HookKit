// Import slot resolution. See HKImportSlots.h for the mechanism and for why
// fishhook's version of this walk is a reference rather than a reuse.

#include "HKImportSlots.h"

#include <string.h>

static uint32_t read_u32_at(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

// Returns the NUL-terminated string at `offset`, or NULL if the offset is out
// of range or the string is not terminated before the table ends. Never reads
// past `strings_size` -- the check fishhook omits.
static const char *bounded_string(const char *strings, size_t strings_size, uint32_t offset) {
    if (offset >= strings_size) {
        return NULL;
    }
    for (size_t i = offset; i < strings_size; i++) {
        if (strings[i] == '\0') {
            return strings + offset;
        }
    }
    return NULL;  // runs to the end of the table without a terminator
}

// ---- table collection ---------------------------------------------------

hk_import_status_t hk_import_tables_from_file_image(const void *image, size_t size,
                                                    hk_import_tables_t *out_tables) {
    if (!image || !out_tables) {
        return HK_IMPORT_INVALID_ARGUMENT;
    }
    memset(out_tables, 0, sizeof(*out_tables));

    hk_symbol_table_view_t symbols;
    hk_macho_status_t status = hk_macho_find_symtab_view(image, size, &symbols);
    if (status == HK_MACHO_OK) {
        out_tables->symbols = symbols;
    } else if (status != HK_MACHO_NOT_FOUND) {
        return HK_IMPORT_MALFORMED;
    }

    uint32_t offset = 0, count = 0;
    status = hk_macho_find_indirect_symbols(image, size, &offset, &count);
    if (status == HK_MACHO_NOT_FOUND) {
        return HK_IMPORT_OK;  // no indirect symbols: no import slots to report
    }
    if (status != HK_MACHO_OK) {
        return HK_IMPORT_MALFORMED;
    }

    // The whole table must lie inside the image.
    uint64_t bytes = (uint64_t)count * sizeof(uint32_t);
    if (offset > size || bytes > (uint64_t)(size - offset)) {
        return HK_IMPORT_MALFORMED;
    }
    out_tables->indirect_symbols = (const uint8_t *)image + offset;
    out_tables->indirect_count = count;
    return HK_IMPORT_OK;
}

hk_import_status_t hk_import_tables_from_loaded_image(const void *header,
                                                      size_t header_region_size,
                                                      uintptr_t slide,
                                                      hk_import_tables_t *out_tables) {
    if (!header || !out_tables) {
        return HK_IMPORT_INVALID_ARGUMENT;
    }
    memset(out_tables, 0, sizeof(*out_tables));

    hk_symbol_table_view_t symbols;
    hk_macho_status_t status =
        hk_macho_symtab_view_for_loaded_image(header, header_region_size, slide, &symbols);
    if (status == HK_MACHO_OK) {
        out_tables->symbols = symbols;
    } else if (status != HK_MACHO_NOT_FOUND) {
        return HK_IMPORT_MALFORMED;
    }

    uint32_t offset = 0, count = 0;
    status = hk_macho_find_indirect_symbols(header, header_region_size, &offset, &count);
    if (status == HK_MACHO_NOT_FOUND) {
        return HK_IMPORT_OK;
    }
    if (status != HK_MACHO_OK) {
        return HK_IMPORT_MALFORMED;
    }

    // The indirect table lives in __LINKEDIT like the symbol table, so its
    // file offset needs the same translation and the same range validation.
    hk_macho_segment_t linkedit;
    status = hk_macho_find_segment(header, header_region_size, "__LINKEDIT", &linkedit);
    if (status != HK_MACHO_OK) {
        return (status == HK_MACHO_NOT_FOUND) ? HK_IMPORT_OK : HK_IMPORT_MALFORMED;
    }
    if (linkedit.filesize > UINT64_MAX - linkedit.fileoff ||
        linkedit.fileoff > linkedit.vmaddr) {
        return HK_IMPORT_MALFORMED;
    }
    uint64_t le_start = linkedit.fileoff;
    uint64_t le_end = linkedit.fileoff + linkedit.filesize;
    uint64_t bytes = (uint64_t)count * sizeof(uint32_t);
    if (offset < le_start || offset > le_end || bytes > le_end - offset) {
        return HK_IMPORT_MALFORMED;
    }
    uintptr_t linkedit_base = slide + (uintptr_t)(linkedit.vmaddr - linkedit.fileoff);

    out_tables->indirect_symbols = (const uint8_t *)(linkedit_base + (uintptr_t)offset);
    out_tables->indirect_count = count;
    return HK_IMPORT_OK;
}

// ---- slot iteration -----------------------------------------------------

typedef struct {
    const hk_import_tables_t *tables;
    hk_import_slot_visit_fn visit;
    void *ctx;
    bool malformed;
    bool stopped;
} slots_ctx_t;

static bool slots_visit_section(void *ctx, uint8_t n_sect, const hk_macho_section_t *section) {
    slots_ctx_t *s = (slots_ctx_t *)ctx;

    uint32_t type = section->flags & HK_SECTION_TYPE;
    bool lazy = (type == HK_S_LAZY_SYMBOL_POINTERS);
    if (type != HK_S_NON_LAZY_SYMBOL_POINTERS && !lazy) {
        return true;  // not a symbol-pointer section
    }
    if (section->size % HK_IMPORT_POINTER_SIZE != 0) {
        s->malformed = true;  // a pointer section must hold whole pointers
        return false;
    }
    uint64_t slot_count = section->size / HK_IMPORT_POINTER_SIZE;

    // CHECK 1 (fishhook omits): the section's window into the indirect symbol
    // table must actually fit inside it. `reserved1` is attacker-influenced
    // data in a malformed image, not a trusted index.
    if ((uint64_t)section->reserved1 + slot_count > (uint64_t)s->tables->indirect_count) {
        s->malformed = true;
        return false;
    }

    for (uint64_t i = 0; i < slot_count; i++) {
        uint32_t entry = read_u32_at(s->tables->indirect_symbols +
                                     ((size_t)section->reserved1 + (size_t)i) * sizeof(uint32_t));
        if ((entry & (HK_INDIRECT_SYMBOL_LOCAL | HK_INDIRECT_SYMBOL_ABS)) != 0) {
            continue;  // names no symbol: not an import
        }

        // CHECK 2 (fishhook omits): the symbol table index must be in range.
        if (entry >= s->tables->symbols.nlist_count) {
            s->malformed = true;
            return false;
        }
        const hk_macho_nlist64_t *sym = &s->tables->symbols.nlist[entry];

        // CHECK 3 (fishhook omits): the name must be inside the string table
        // AND terminated within it.
        const char *name = bounded_string(s->tables->symbols.strings,
                                          s->tables->symbols.strings_size, sym->n_strx);
        if (!name) {
            s->malformed = true;
            return false;
        }

        hk_import_slot_t slot;
        memset(&slot, 0, sizeof(slot));
        slot.n_sect = n_sect;
        memcpy(slot.sectname, section->sectname, sizeof(slot.sectname));
        slot.slot_index = (uint32_t)i;
        slot.slot_vmaddr = section->addr + i * HK_IMPORT_POINTER_SIZE;
        slot.symtab_index = entry;
        slot.symbol_name = name;
        slot.is_lazy = lazy;

        if (!s->visit(s->ctx, &slot)) {
            s->stopped = true;
            return false;
        }
    }
    return true;
}

hk_import_status_t hk_import_slots_iterate(const void *image, size_t image_size,
                                           const hk_import_tables_t *tables,
                                           hk_import_slot_visit_fn visit, void *ctx) {
    if (!image || !tables || !visit) {
        return HK_IMPORT_INVALID_ARGUMENT;
    }
    if (!tables->indirect_symbols || tables->indirect_count == 0 ||
        !tables->symbols.nlist || !tables->symbols.strings) {
        return HK_IMPORT_OK;  // nothing to walk is not a failure
    }
    slots_ctx_t s;
    s.tables = tables;
    s.visit = visit;
    s.ctx = ctx;
    s.malformed = false;
    s.stopped = false;

    hk_macho_status_t status =
        hk_macho_iterate_sections(image, image_size, slots_visit_section, &s);
    if (status != HK_MACHO_OK) {
        return HK_IMPORT_MALFORMED;
    }
    return s.malformed ? HK_IMPORT_MALFORMED : HK_IMPORT_OK;
}

typedef struct {
    const hk_symbol_candidates_t *candidates;
    hk_import_slot_t match;
    bool found;
} find_ctx_t;

static bool find_visit(void *ctx, const hk_import_slot_t *slot) {
    find_ctx_t *f = (find_ctx_t *)ctx;
    for (unsigned i = 0; i < f->candidates->count; i++) {
        if (strcmp(slot->symbol_name, f->candidates->names[i]) == 0) {
            f->match = *slot;
            f->found = true;
            return false;
        }
    }
    return true;
}

hk_import_status_t hk_import_slots_find(const void *image, size_t image_size,
                                        const hk_import_tables_t *tables,
                                        const char *name,
                                        hk_symbol_name_convention_t convention,
                                        hk_import_slot_t *out_slot) {
    if (!image || !tables || !name || !out_slot) {
        return HK_IMPORT_INVALID_ARGUMENT;
    }
    // The same normalization rule as hk_resolve_symbol, from the same place.
    hk_symbol_candidates_t candidates;
    hk_resolve_status_t rc = hk_symbol_build_candidates(name, convention, &candidates);
    if (rc == HK_RESOLVE_NAME_TOO_LONG) {
        return HK_IMPORT_NAME_TOO_LONG;
    }
    if (rc != HK_RESOLVE_OK) {
        return HK_IMPORT_INVALID_ARGUMENT;
    }

    find_ctx_t f;
    f.candidates = &candidates;
    f.found = false;
    memset(&f.match, 0, sizeof(f.match));

    hk_import_status_t status =
        hk_import_slots_iterate(image, image_size, tables, find_visit, &f);
    if (status != HK_IMPORT_OK) {
        return status;
    }
    if (!f.found) {
        return HK_IMPORT_NOT_FOUND;
    }
    *out_slot = f.match;
    return HK_IMPORT_OK;
}
