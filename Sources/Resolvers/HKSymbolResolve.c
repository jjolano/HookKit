// Resolver selection. See HKSymbolResolve.h for the two decisions this layer
// owns and why they belong here rather than in either mechanism.

#include "HKSymbolResolve.h"

#include <string.h>

// At most two candidates: the name as given, then the underscore-prefixed
// linker form. Ordered -- an exact match wins over the prefixed one.
#define MAX_CANDIDATES 2

typedef struct {
    const char *names[MAX_CANDIDATES];
    unsigned count;
    char storage[HK_RESOLVE_MAX_NAME + 2];  // room for '_' + name + NUL
} candidates_t;

static hk_resolve_status_t build_candidates(const char *name,
                                            hk_symbol_name_convention_t convention,
                                            candidates_t *out) {
    size_t length = strlen(name);
    if (length == 0) {
        return HK_RESOLVE_INVALID_ARGUMENT;
    }
    if (length > HK_RESOLVE_MAX_NAME) {
        return HK_RESOLVE_NAME_TOO_LONG;
    }

    out->count = 0;
    out->names[out->count++] = name;  // exactly what the caller asked for

    // MACHO_EXACT means "the table/trie string, verbatim" -- no second form.
    // Every other convention (C, C++ mangled, Swift mangled) acquires Mach-O's
    // leading underscore, so the prefixed form is also a legitimate candidate.
    // Note this is applied even when `name` already starts with '_': a C
    // symbol literally named `_hidden` appears as `__hidden`, and only the
    // prefixed candidate would find it.
    if (convention != HK_SYMBOL_NAME_MACHO_EXACT) {
        out->storage[0] = '_';
        memcpy(out->storage + 1, name, length + 1);
        out->names[out->count++] = out->storage;
    }
    return HK_RESOLVE_OK;
}

// Tries every candidate against the export trie. Returns true if one was
// found (including a re-export, which is reported through `out_status`).
static bool try_export_trie(const hk_symbol_sources_t *sources,
                            const candidates_t *candidates,
                            hk_symbol_resolution_t *out,
                            hk_resolve_status_t *out_status) {
    if (!sources->export_trie || sources->export_trie_size == 0) {
        return false;
    }
    for (unsigned i = 0; i < candidates->count; i++) {
        hk_export_symbol_t symbol;
        hk_export_status_t status = hk_export_trie_find(
            sources->export_trie, sources->export_trie_size, candidates->names[i], &symbol);

        if (status == HK_EXPORT_OK) {
            memset(out, 0, sizeof(*out));
            out->source = HK_RESOLVE_SOURCE_EXPORT_TRIE;
            out->raw_value = symbol.address;
            out->address = sources->header_address + (uintptr_t)symbol.address;
            out->export_flags = symbol.flags;
            out->is_weak = symbol.is_weak;
            *out_status = HK_RESOLVE_OK;
            return true;
        }
        if (status == HK_EXPORT_UNSUPPORTED_KIND) {
            // A re-export names another dylib. It IS the symbol, so the search
            // stops here rather than falling through to a different symbol
            // that happens to share the name -- but there is no address to
            // give, and none is invented.
            memset(out, 0, sizeof(*out));
            out->source = HK_RESOLVE_SOURCE_EXPORT_TRIE;
            out->export_flags = symbol.flags;
            out->is_weak = symbol.is_weak;
            *out_status = HK_RESOLVE_UNSUPPORTED;
            return true;
        }
        if (status == HK_EXPORT_MALFORMED) {
            *out_status = HK_RESOLVE_MALFORMED_IMAGE;
            return true;
        }
    }
    return false;
}

static bool try_symbol_table(const hk_symbol_sources_t *sources,
                             const candidates_t *candidates,
                             hk_symbol_visibility_t visibility,
                             hk_symbol_resolution_t *out,
                             hk_resolve_status_t *out_status) {
    if (!sources->symbol_table.nlist || sources->symbol_table.nlist_count == 0) {
        return false;
    }
    for (unsigned i = 0; i < candidates->count; i++) {
        hk_symbol_query_t query;
        query.name = candidates->names[i];
        // MACHO_EXACT switches off HKSymbolTable's own normalization: the
        // candidate list above has already applied it, once.
        query.convention = HK_SYMBOL_NAME_MACHO_EXACT;
        query.visibility = visibility;

        hk_symbol_match_t match;
        if (hk_symbol_table_find(&sources->symbol_table, &query, &match)) {
            memset(out, 0, sizeof(*out));
            out->source = HK_RESOLVE_SOURCE_SYMBOL_TABLE;
            out->raw_value = match.n_value;
            out->address = (uintptr_t)match.n_value + sources->slide;
            out->n_sect = match.n_sect;
            *out_status = HK_RESOLVE_OK;
            return true;
        }
    }
    return false;
}

hk_resolve_status_t hk_resolve_symbol(const hk_symbol_sources_t *sources,
                                      const char *name,
                                      hk_symbol_name_convention_t convention,
                                      hk_symbol_visibility_t visibility,
                                      hk_symbol_resolution_t *out_resolution) {
    if (!sources || !name || !out_resolution) {
        return HK_RESOLVE_INVALID_ARGUMENT;
    }
    candidates_t candidates;
    hk_resolve_status_t status = build_candidates(name, convention, &candidates);
    if (status != HK_RESOLVE_OK) {
        return status;
    }

    memset(out_resolution, 0, sizeof(*out_resolution));
    hk_resolve_status_t found_status = HK_RESOLVE_NOT_FOUND;

    switch (visibility) {
    case HK_SYMBOL_VISIBILITY_EXPORTED_ONLY:
        if (sources->export_trie && sources->export_trie_size > 0) {
            // The trie is dyld's authority on what is exported: absence from
            // it means not exported, so there is nothing to fall back to.
            if (try_export_trie(sources, &candidates, out_resolution, &found_status)) {
                return found_status;
            }
            return HK_RESOLVE_NOT_FOUND;
        }
        // No trie: the symbol table filtered to N_EXT is the best answer left.
        if (try_symbol_table(sources, &candidates, HK_SYMBOL_VISIBILITY_EXPORTED_ONLY,
                             out_resolution, &found_status)) {
            return found_status;
        }
        return HK_RESOLVE_NOT_FOUND;

    case HK_SYMBOL_VISIBILITY_PRIVATE_ALLOWED:
        // Private symbols only exist in the symbol table, so look there first;
        // the trie still covers an export stripped from the symbol table.
        if (try_symbol_table(sources, &candidates, HK_SYMBOL_VISIBILITY_ANY,
                             out_resolution, &found_status)) {
            return found_status;
        }
        if (try_export_trie(sources, &candidates, out_resolution, &found_status)) {
            return found_status;
        }
        return HK_RESOLVE_NOT_FOUND;

    case HK_SYMBOL_VISIBILITY_ANY:
    default:
        if (try_export_trie(sources, &candidates, out_resolution, &found_status)) {
            return found_status;
        }
        if (try_symbol_table(sources, &candidates, HK_SYMBOL_VISIBILITY_ANY,
                             out_resolution, &found_status)) {
            return found_status;
        }
        return HK_RESOLVE_NOT_FOUND;
    }
}

// ---- source collection --------------------------------------------------

hk_resolve_status_t hk_symbol_sources_from_file_image(const void *image, size_t size,
                                                      hk_symbol_sources_t *out_sources) {
    if (!image || !out_sources) {
        return HK_RESOLVE_INVALID_ARGUMENT;
    }
    memset(out_sources, 0, sizeof(*out_sources));
    out_sources->header_address = (uintptr_t)image;

    // An nlist n_value is an unslid VM address, so the "slide" that maps it
    // into this buffer is (buffer - __TEXT.vmaddr). Computing it here is what
    // makes symbol-table and export-trie addresses directly comparable.
    hk_macho_segment_t text;
    hk_macho_status_t text_status = hk_macho_find_segment(image, size, "__TEXT", &text);
    if (text_status == HK_MACHO_OK) {
        out_sources->slide = (uintptr_t)image - (uintptr_t)text.vmaddr;
    } else if (text_status == HK_MACHO_NOT_FOUND) {
        out_sources->slide = (uintptr_t)image;  // no __TEXT: treat vmaddr as 0
    } else {
        return HK_RESOLVE_MALFORMED_IMAGE;
    }

    // Either source may legitimately be absent.
    hk_symbol_table_view_t view;
    hk_macho_status_t status = hk_macho_find_symtab_view(image, size, &view);
    if (status == HK_MACHO_OK) {
        out_sources->symbol_table = view;
    } else if (status != HK_MACHO_NOT_FOUND) {
        return HK_RESOLVE_MALFORMED_IMAGE;
    }

    size_t trie_offset = 0, trie_size = 0;
    status = hk_macho_find_export_trie(image, size, &trie_offset, &trie_size);
    if (status == HK_MACHO_OK) {
        out_sources->export_trie = (const uint8_t *)image + trie_offset;
        out_sources->export_trie_size = trie_size;
    } else if (status != HK_MACHO_NOT_FOUND) {
        return HK_RESOLVE_MALFORMED_IMAGE;
    }
    return HK_RESOLVE_OK;
}

hk_resolve_status_t hk_symbol_sources_from_loaded_image(const void *header,
                                                        size_t header_region_size,
                                                        uintptr_t slide,
                                                        hk_symbol_sources_t *out_sources) {
    if (!header || !out_sources) {
        return HK_RESOLVE_INVALID_ARGUMENT;
    }
    memset(out_sources, 0, sizeof(*out_sources));
    out_sources->header_address = (uintptr_t)header;
    out_sources->slide = slide;  // supplied by dyld on device

    hk_symbol_table_view_t view;
    hk_macho_status_t status =
        hk_macho_symtab_view_for_loaded_image(header, header_region_size, slide, &view);
    if (status == HK_MACHO_OK) {
        out_sources->symbol_table = view;
    } else if (status != HK_MACHO_NOT_FOUND) {
        return HK_RESOLVE_MALFORMED_IMAGE;
    }

    const void *trie = NULL;
    size_t trie_size = 0;
    status = hk_macho_export_trie_for_loaded_image(header, header_region_size, slide,
                                                   &trie, &trie_size);
    if (status == HK_MACHO_OK) {
        out_sources->export_trie = (const uint8_t *)trie;
        out_sources->export_trie_size = trie_size;
    } else if (status != HK_MACHO_NOT_FOUND) {
        return HK_RESOLVE_MALFORMED_IMAGE;
    }
    return HK_RESOLVE_OK;
}
