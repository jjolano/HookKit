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

// On-disk sizes of the structures parsed here. Asserted against the real
// Apple layouts in Tests/Host/test_macho.c.
#define HK_MACHO_HEADER_64_SIZE     32u
#define HK_LOAD_COMMAND_SIZE         8u
#define HK_SYMTAB_COMMAND_SIZE      24u

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

#ifdef __cplusplus
}
#endif

#endif // HK_RESOLVERS_MACHO_H
