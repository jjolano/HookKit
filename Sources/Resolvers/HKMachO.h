// Mach-O container parsing -- Milestone 5. Locates load commands in a
// 64-bit Mach-O image and produces the symbol table view that
// HKSymbolTable.h consumes, closing the path: raw buffer -> LC_SYMTAB ->
// symbol search.
//
// PURE buffer logic over a caller-supplied image. Parsing a Mach-O is
// arithmetic over bytes; only *obtaining* the bytes (mmap, dyld) is
// device-only. So all of this is host-tested against synthetic images.
//
// Relationship to native/hk_symbols.c (surveyed before writing): 2.x walks
// load commands twice -- once bounded against a mmap'd file size
// (bind_ondisk_symbols) and once unbounded over a live dyld-validated header
// (collect_section_flags). Both are inside `#if defined(__arm64__)`, both use
// Apple's headers, and neither can run here. This is the single bounded walk,
// host-testable, with the structure layouts declared locally so it builds
// off-Apple. The 2.x path is untouched and still serves the 2.x runtime.
//
// Two robustness properties, both tested:
//   - Every read is bounded by the caller-declared size. A truncated or
//     hostile image cannot cause a read past the buffer, and a malformed
//     cmdsize cannot cause an infinite loop (cmdsize < 8 is rejected, so the
//     cursor always advances).
//   - All multi-byte reads go through memcpy, so a buffer at any alignment
//     is safe to parse. Casting to a struct pointer would be undefined
//     behaviour on a misaligned buffer; a parser handed arbitrary bytes must
//     not assume. The one place alignment still matters is the *typed* nlist
//     pointer handed out by hk_macho_find_symtab_view, which callers
//     dereference -- that one is validated and rejected if misaligned, rather
//     than returned as an unsound pointer.

#ifndef HK_RESOLVERS_MACHO_H
#define HK_RESOLVERS_MACHO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "HKSymbolTable.h"  // hk_symbol_table_view_t

#ifdef __cplusplus
extern "C" {
#endif

// Magic values (<mach-o/loader.h>, <mach-o/fat.h>), declared locally.
#define HK_MH_MAGIC_64 0xfeedfacfu
#define HK_MH_CIGAM_64 0xcffaedfeu
#define HK_MH_MAGIC    0xfeedfaceu
#define HK_MH_CIGAM    0xcefaedfeu
#define HK_FAT_MAGIC   0xcafebabeu
#define HK_FAT_CIGAM   0xbebafecau

// Load command types used here; more can be added as resolvers need them.
#define HK_LC_SYMTAB             0x02u
#define HK_LC_DYSYMTAB           0x0bu
#define HK_LC_SEGMENT_64         0x19u
#define HK_LC_UUID               0x1bu
#define HK_LC_DYLD_INFO          0x22u
#define HK_LC_DYLD_INFO_ONLY     0x80000022u
#define HK_LC_DYLD_EXPORTS_TRIE  0x80000033u
#define HK_LC_DYLD_CHAINED_FIXUPS 0x80000034u

// On-disk sizes of the structures parsed here. Asserted against the real
// Apple layouts in Tests/Host/test_macho.c.
#define HK_MACHO_HEADER_64_SIZE     32u
#define HK_LOAD_COMMAND_SIZE         8u
#define HK_SYMTAB_COMMAND_SIZE      24u
#define HK_SEGMENT_COMMAND_64_SIZE  72u
#define HK_SECTION_64_SIZE          80u
#define HK_LINKEDIT_DATA_CMD_SIZE   16u
#define HK_DYLD_INFO_COMMAND_SIZE   48u
#define HK_DYSYMTAB_COMMAND_SIZE    80u

// Section flag bits that mark a section as containing instructions -- the
// code-vs-data distinction (arm64e signs code pointers, not data pointers).
#define HK_S_ATTR_PURE_INSTRUCTIONS 0x80000000u
#define HK_S_ATTR_SOME_INSTRUCTIONS 0x00000400u

// The low byte of a section's flags is its type.
#define HK_SECTION_TYPE                  0x000000ffu
#define HK_S_NON_LAZY_SYMBOL_POINTERS    0x00000006u
#define HK_S_LAZY_SYMBOL_POINTERS        0x00000007u

typedef enum {
    HK_MACHO_OK = 0,
    HK_MACHO_TOO_SMALL,               // buffer smaller than the structure being read
    HK_MACHO_NOT_MACHO,               // no recognized magic
    HK_MACHO_FAT_UNSUPPORTED,         // universal binary: pick a slice first
    HK_MACHO_BYTE_ORDER_UNSUPPORTED,  // byte-swapped image; no swapping is done
    HK_MACHO_NOT_64_BIT,              // 32-bit Mach-O (the legacy armv7 lane's 2.x path handles those)
    HK_MACHO_MALFORMED,               // structurally invalid: commands overrun, bad cmdsize, bad symtab offsets
    HK_MACHO_NOT_FOUND,               // well-formed, but the requested command is absent
} hk_macho_status_t;

typedef struct {
    uint32_t magic;
    uint32_t cputype;
    uint32_t cpusubtype;
    uint32_t filetype;
    uint32_t ncmds;
    uint32_t sizeofcmds;
    uint32_t flags;
} hk_macho_header_t;

// Validates the magic and that the load-command region fits within `size`.
hk_macho_status_t hk_macho_read_header(const void *image, size_t size,
                                       hk_macho_header_t *out_header);

// Visits each load command in order. `offset` is the command's offset from
// the start of the image. Returning false stops iteration early (reported as
// HK_MACHO_OK -- an early stop is a caller decision, not an error).
typedef bool (*hk_macho_lc_visit_fn)(void *ctx, uint32_t cmd, size_t offset,
                                     uint32_t cmdsize);
hk_macho_status_t hk_macho_iterate_load_commands(const void *image, size_t size,
                                                 hk_macho_lc_visit_fn visit,
                                                 void *ctx);

// Finds the first load command of type `cmd`. HK_MACHO_NOT_FOUND if absent.
hk_macho_status_t hk_macho_find_load_command(const void *image, size_t size,
                                             uint32_t cmd, size_t *out_offset,
                                             uint32_t *out_cmdsize);

// Builds the symbol table view for HKSymbolTable.h from LC_SYMTAB, with every
// referenced range validated against `size`. The returned view points INTO
// `image`, so it stays valid only as long as the image does.
//
// FILE-IMAGE LAYOUT ONLY, stated because it is a real limitation: LC_SYMTAB's
// symoff/stroff are file offsets, so this is correct for an image laid out as
// on disk (what 2.x's bind_ondisk_symbols mmaps). A *loaded* image is
// scattered at segment VM addresses, where the offsets must be translated
// through the __LINKEDIT segment instead. That translation needs segment
// parsing and is deliberately not done here rather than guessed at.
hk_macho_status_t hk_macho_find_symtab_view(const void *image, size_t size,
                                            hk_symbol_table_view_t *out_view);

// ---- segments and sections ---------------------------------------------

typedef struct {
    char segname[17];   // NUL-terminated copy; the on-disk field is 16 bytes
                        // and need not be terminated, so it is never read as
                        // a C string in place.
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t fileoff;
    uint64_t filesize;
    uint32_t maxprot;
    uint32_t initprot;
    uint32_t nsects;
    uint32_t flags;
    size_t command_offset;  // offset of the LC_SEGMENT_64 command in the image
    uint32_t cmdsize;
} hk_macho_segment_t;

// Visits every LC_SEGMENT_64 in load-command order. Returning false stops
// early. Needed to work out an image's VM layout -- computing its mapped span,
// or placing each segment at its vmaddr -- which name-by-name lookup cannot do.
typedef bool (*hk_macho_segment_visit_fn)(void *ctx, uint32_t index,
                                          const hk_macho_segment_t *segment);
hk_macho_status_t hk_macho_iterate_segments(const void *image, size_t size,
                                            hk_macho_segment_visit_fn visit, void *ctx);

// Finds the first LC_SEGMENT_64 whose name equals `segname` (compared over at
// most 16 bytes, since the on-disk field is fixed-width and may be
// unterminated). HK_MACHO_NOT_FOUND if absent.
hk_macho_status_t hk_macho_find_segment(const void *image, size_t size,
                                        const char *segname,
                                        hk_macho_segment_t *out_segment);

// Section flags for a 1-based section index, numbered across all segments in
// load-command order -- exactly the numbering an nlist's `n_sect` uses, and
// the same ordering HookKit 2.x's collect_section_flags builds. n_sect 0 is
// NO_SECT and is rejected.
//
// Unlike the 2.x version this is fully bounded: a segment's section array
// must fit inside the segment command's own cmdsize, so a corrupt `nsects`
// cannot walk off the end. (2.x reads sections without that check because it
// only ever runs on a live, dyld-validated image.)
hk_macho_status_t hk_macho_section_flags(const void *image, size_t size,
                                         uint8_t n_sect, uint32_t *out_flags);

// One section_64, as parsed. `n_sect` numbering matches an nlist's field:
// 1-based, counted across all segments in load-command order.
typedef struct {
    char sectname[17];   // NUL-terminated copies; the on-disk fields are fixed
    char segname[17];    // 16-byte and need not be terminated
    uint64_t addr;       // unslid VM address
    uint64_t size;
    uint32_t offset;     // file offset of the section's contents
    uint32_t flags;
    uint32_t reserved1;  // for symbol-pointer sections: index into the indirect symbol table
    uint32_t reserved2;
} hk_macho_section_t;

// Visits every section in n_sect order. Returning false stops early.
typedef bool (*hk_macho_section_visit_fn)(void *ctx, uint8_t n_sect,
                                          const hk_macho_section_t *section);
hk_macho_status_t hk_macho_iterate_sections(const void *image, size_t size,
                                            hk_macho_section_visit_fn visit, void *ctx);

// Full information for one 1-based section index.
hk_macho_status_t hk_macho_section_info(const void *image, size_t size,
                                        uint8_t n_sect, hk_macho_section_t *out_section);

// Locates LC_DYSYMTAB's indirect symbol table -- the array mapping each
// symbol-pointer slot to a symbol table index. Reports the declared FILE
// offset and entry count without validating them, since what they must be
// validated against differs between file and loaded layouts (see
// HKImportSlots.h, which does that validation).
hk_macho_status_t hk_macho_find_indirect_symbols(const void *image, size_t size,
                                                 uint32_t *out_file_offset,
                                                 uint32_t *out_count);

// Whether a section's flags mark it as containing instructions. The check a
// caller uses to decide whether a resolved symbol is code (and so, on arm64e,
// whether its address should be signed rather than left a plain pointer).
static inline bool hk_macho_section_is_code(uint32_t section_flags) {
    return (section_flags & (HK_S_ATTR_PURE_INSTRUCTIONS | HK_S_ATTR_SOME_INSTRUCTIONS)) != 0;
}

// ---- loaded-image symbol table -----------------------------------------

// The loaded-image counterpart to hk_macho_find_symtab_view, lifting that
// function's file-image-only limitation.
//
// A loaded image is scattered at segment VM addresses, so LC_SYMTAB's file
// offsets do not index the mapped header. They are instead relative to the
// __LINKEDIT segment, whose mapped base is
//     linkedit_base = slide + linkedit.vmaddr - linkedit.fileoff
// and the symbol table lives at linkedit_base + symoff / + stroff. `header`
// is the image's mach header as mapped, `slide` its ASLR slide (on device,
// _dyld_get_image_header / _dyld_get_image_vmaddr_slide).
//
// `header_region_size` bounds only the LOAD-COMMAND parsing -- it is how many
// bytes are safely readable from `header`. The returned view deliberately
// points OUTSIDE that region, into __LINKEDIT, which is the correct result
// for a real loaded image; both referenced ranges are validated to lie within
// __LINKEDIT's declared file range, so a corrupt LC_SYMTAB cannot produce a
// pointer into unrelated memory.
hk_macho_status_t hk_macho_symtab_view_for_loaded_image(const void *header,
                                                        size_t header_region_size,
                                                        uintptr_t slide,
                                                        hk_symbol_table_view_t *out_view);

// Locates the export trie's byte range. Modern images carry
// LC_DYLD_EXPORTS_TRIE; older ones put export_off/export_size inside
// LC_DYLD_INFO(_ONLY). Both are checked, in that order, so a caller never has
// to know which form an image uses. HK_MACHO_NOT_FOUND if the image has
// neither (or declares an empty trie).
//
// FILE-IMAGE LAYOUT, same as hk_macho_find_symtab_view: the returned offset
// is into `image`. For a loaded image the value is a __LINKEDIT-relative file
// offset and needs the same translation
// hk_macho_symtab_view_for_loaded_image performs.
hk_macho_status_t hk_macho_find_export_trie(const void *image, size_t size,
                                            size_t *out_offset, size_t *out_size);

// Locates the LC_DYLD_CHAINED_FIXUPS payload -- the modern (iOS 15+) import
// mechanism, which the LC_DYSYMTAB indirect-symbol path does not cover.
// HK_MACHO_NOT_FOUND if the image uses the older mechanism instead.
hk_macho_status_t hk_macho_find_chained_fixups(const void *image, size_t size,
                                               size_t *out_offset, size_t *out_size);

// Loaded-image counterpart, translating through __LINKEDIT like the others.
hk_macho_status_t hk_macho_chained_fixups_for_loaded_image(const void *header,
                                                           size_t header_region_size,
                                                           uintptr_t slide,
                                                           const void **out_blob,
                                                           size_t *out_size);

// Loaded-image counterpart: the trie's declared offset is __LINKEDIT-relative,
// so it needs the same translation hk_macho_symtab_view_for_loaded_image
// performs. Returns a pointer into the mapped __LINKEDIT, validated to lie
// within that segment's declared file range.
hk_macho_status_t hk_macho_export_trie_for_loaded_image(const void *header,
                                                        size_t header_region_size,
                                                        uintptr_t slide,
                                                        const void **out_trie,
                                                        size_t *out_size);

#ifdef __cplusplus
}
#endif

#endif // HK_RESOLVERS_MACHO_H
