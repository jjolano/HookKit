// Import slot resolution -- Milestone 5. Answers "which pointer slot in this
// image binds to symbol X", which is the question a rebind engine
// (Milestone 6) has to answer before it can redirect anything.
//
// Mechanism: a symbol-pointer section (`__got`, `__auth_got`,
// `__la_symbol_ptr`, ...) holds one pointer per imported symbol. Its
// `reserved1` field is an index into LC_DYSYMTAB's *indirect symbol table*,
// where entry `reserved1 + i` gives the symbol table index that slot `i`
// binds to. Following that to the nlist and then the string table yields the
// name. All of it is arithmetic over bytes, so all of it is host-testable;
// only obtaining the image, and later *writing* a slot, is device-only.
//
// Reuse survey (done before writing): vendor/fishhook/fishhook.c performs
// exactly this walk in perform_rebinding_with_section, and is the reference
// for the mechanism. It is not reusable here, for two reasons. First, its walk
// is fused with the parts that only exist on device -- VM protection changes,
// arm64e pointer re-signing for `__auth_got`, and its own rebinding list.
// Second, and more importantly, it performs *no bounds validation at all*:
// `indirect_symtab + section->reserved1`, `symtab[symtab_index]`, and
// `strtab + strtab_offset` are each dereferenced unchecked. That is sound in
// fishhook's context because dyld has already validated the image, but a
// parser handed arbitrary bytes cannot assume it. Every one of those three
// steps is bounds-checked below.

#ifndef HK_RESOLVERS_IMPORT_SLOTS_H
#define HK_RESOLVERS_IMPORT_SLOTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "HKMachO.h"
#include "HKSymbolTable.h"
#include "HKSymbolResolve.h"  // hk_symbol_candidates_t: one normalization rule

#ifdef __cplusplus
extern "C" {
#endif

// 64-bit images only, matching the rest of these resolvers.
#define HK_IMPORT_POINTER_SIZE 8u

// Indirect symbol table entries that name no symbol (Mach-O's own sentinels).
#define HK_INDIRECT_SYMBOL_LOCAL 0x80000000u
#define HK_INDIRECT_SYMBOL_ABS   0x40000000u

typedef enum {
    HK_IMPORT_OK = 0,
    HK_IMPORT_NOT_FOUND,
    HK_IMPORT_INVALID_ARGUMENT,
    HK_IMPORT_MALFORMED,
    HK_IMPORT_NAME_TOO_LONG,
} hk_import_status_t;

// The two __LINKEDIT tables a slot walk needs, borrowed not owned.
typedef struct {
    const uint8_t *indirect_symbols;  // raw bytes; entries are read via memcpy,
                                      // so no alignment is required of them
    uint32_t indirect_count;
    hk_symbol_table_view_t symbols;   // for names
} hk_import_tables_t;

typedef struct {
    uint8_t n_sect;          // 1-based section holding this slot
    char sectname[17];       // e.g. "__got", "__la_symbol_ptr"
    uint32_t slot_index;     // index of the slot within its section
    uint64_t slot_vmaddr;    // UNSLID address of the slot; add the image slide
    uint32_t symtab_index;   // index into the symbol table
    const char *symbol_name; // borrowed from the string table, NUL-terminated within it
    bool is_lazy;            // S_LAZY_SYMBOL_POINTERS vs S_NON_LAZY_SYMBOL_POINTERS
} hk_import_slot_t;

// Collects the tables from an image laid out as on disk.
hk_import_status_t hk_import_tables_from_file_image(const void *image, size_t size,
                                                    hk_import_tables_t *out_tables);

// Same for a loaded image, translating both tables through __LINKEDIT.
hk_import_status_t hk_import_tables_from_loaded_image(const void *header,
                                                      size_t header_region_size,
                                                      uintptr_t slide,
                                                      hk_import_tables_t *out_tables);

// Visits every import slot that names a symbol, in section then slot order.
// Slots whose indirect entry is LOCAL/ABS name no symbol and are skipped --
// they are not imports. Returning false stops early.
//
// `image`/`image_size` bound the section walk (the header region); `tables`
// supplies the __LINKEDIT data. They are separate because on a loaded image
// those live in different places.
typedef bool (*hk_import_slot_visit_fn)(void *ctx, const hk_import_slot_t *slot);
hk_import_status_t hk_import_slots_iterate(const void *image, size_t image_size,
                                           const hk_import_tables_t *tables,
                                           hk_import_slot_visit_fn visit, void *ctx);

// Finds the first slot binding to `name`. Uses the same linker-form candidate
// expansion as hk_resolve_symbol (HKSymbolResolve.h), so `malloc` matches the
// stored `_malloc` under the same rule, applied in the same one place.
hk_import_status_t hk_import_slots_find(const void *image, size_t image_size,
                                        const hk_import_tables_t *tables,
                                        const char *name,
                                        hk_symbol_name_convention_t convention,
                                        hk_import_slot_t *out_slot);

#ifdef __cplusplus
}
#endif

#endif // HK_RESOLVERS_IMPORT_SLOTS_H
