// Mach-O symbol table search. See HKSymbolTable.h for the contract and for
// why this is a pure, host-testable function rather than part of
// native/hk_symbols.c.

#include "HKSymbolTable.h"

// Compares the NUL-terminated string starting at strings[offset] against
// `name`, reading NOTHING at or past strings_size. Returns false (no match)
// if the table string runs to the end without a terminator -- a truncated
// string table must never cause a read past the buffer, which is exactly
// what a plain strcmp() on an unterminated table would do.
static bool bounded_string_equals(const char *strings, size_t strings_size,
                                  size_t offset, const char *name) {
    if (offset >= strings_size) {
        return false;
    }
    for (size_t i = 0;; i++) {
        size_t pos = offset + i;
        if (pos >= strings_size) {
            return false;  // unterminated within the table: refuse, don't over-read
        }
        char table_char = strings[pos];
        if (table_char != name[i]) {
            return false;
        }
        if (table_char == '\0') {
            return true;  // both strings ended at the same point
        }
    }
}

// Does the table string at `offset` match `name` under `convention`?
static bool name_matches(const char *strings, size_t strings_size,
                         size_t offset, const char *name,
                         hk_symbol_name_convention_t convention) {
    if (bounded_string_equals(strings, strings_size, offset, name)) {
        return true;
    }
    if (convention == HK_SYMBOL_NAME_MACHO_EXACT) {
        // Exact mode means exactly what the table holds -- no underscore
        // normalization, so "_malloc" in the table does not match "malloc".
        return false;
    }
    // C / C++-mangled / Swift-mangled names all acquire Mach-O's leading
    // underscore ("_malloc", "__ZN3Foo3barEv", "_$s4test..."), so a query
    // written in source form must also match the underscored table form.
    if (offset < strings_size && strings[offset] == '_') {
        return bounded_string_equals(strings, strings_size, offset + 1, name);
    }
    return false;
}

static bool visibility_permits(uint8_t n_type, hk_symbol_visibility_t visibility) {
    if (visibility == HK_SYMBOL_VISIBILITY_EXPORTED_ONLY) {
        return (n_type & HK_N_EXT) != 0;
    }
    // ANY and PRIVATE_ALLOWED both accept local and external symbols here;
    // see the header on why they are not faked into a difference.
    return true;
}

bool hk_symbol_table_find(const hk_symbol_table_view_t *view,
                          const hk_symbol_query_t *query,
                          hk_symbol_match_t *out_match) {
    if (!view || !query || !out_match) {
        return false;
    }
    if (!view->nlist || !view->strings || view->strings_size == 0) {
        return false;
    }
    if (!query->name || query->name[0] == '\0') {
        return false;
    }

    for (uint32_t i = 0; i < view->nlist_count; i++) {
        const hk_macho_nlist64_t *sym = &view->nlist[i];

        // A STAB entry is debug information; its n_sect/n_value do not carry
        // symbol meaning. Some STAB types (N_BNSYM 0x2e, N_ENSYM 0x4e) even
        // satisfy the N_SECT test below by coincidence, so this rejection
        // must come first and be on its own terms.
        if ((sym->n_type & HK_N_STAB) != 0) {
            continue;
        }
        // Only a symbol actually defined in a section, at a real address, can
        // be a hook target -- undefined/absolute/prebound entries cannot.
        if ((sym->n_type & HK_N_TYPE) != HK_N_SECT || sym->n_value == 0) {
            continue;
        }
        if (sym->n_strx == 0) {
            continue;  // index 0 is the empty string: an unnamed entry
        }
        if (!visibility_permits(sym->n_type, query->visibility)) {
            continue;
        }
        if (!name_matches(view->strings, view->strings_size,
                          (size_t)sym->n_strx, query->name, query->convention)) {
            continue;
        }

        out_match->n_value = sym->n_value;
        out_match->n_sect = sym->n_sect;
        out_match->n_type = sym->n_type;
        out_match->index = i;
        return true;  // first match in table order (documented rule)
    }
    return false;
}
