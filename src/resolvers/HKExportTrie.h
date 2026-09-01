// Mach-O export trie walking -- Milestone 5. The proper resolver for
// *exported* symbols: dyld stores exports in a ULEB128-encoded prefix tree
// (LC_DYLD_EXPORTS_TRIE, or LC_DYLD_INFO(_ONLY)'s export_off/export_size),
// not in the symbol table. HKSymbolTable.h remains the private-symbol path.
//
// PURE buffer logic, fully host-testable. Nothing here is device-specific.
//
// Reuse survey (done before writing): no retained component decodes ULEB128 or
// walks an export trie. src/native/hk_symbols.c only reads the symbol table and
// the shared cache's own index. So this is new code, not a second copy.
//
// Safety, all tested: ULEB128 decoding is bounds-checked and rejects both
// overlong encodings (>10 bytes) and values that would overflow 64 bits;
// child offsets are range-checked; and a depth cap breaks cycles, which a
// trie with a zero-length edge pointing back at an ancestor would otherwise
// spin on forever.

#ifndef HK_RESOLVERS_EXPORT_TRIE_H
#define HK_RESOLVERS_EXPORT_TRIE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Export flags (<mach-o/loader.h>), declared locally so this builds off-Apple.
#define HK_EXPORT_FLAGS_KIND_MASK          0x03u
#define HK_EXPORT_FLAGS_KIND_REGULAR       0x00u
#define HK_EXPORT_FLAGS_KIND_THREAD_LOCAL  0x01u
#define HK_EXPORT_FLAGS_KIND_ABSOLUTE      0x02u
#define HK_EXPORT_FLAGS_WEAK_DEFINITION    0x04u
#define HK_EXPORT_FLAGS_REEXPORT           0x08u
#define HK_EXPORT_FLAGS_STUB_AND_RESOLVER  0x10u

// Real tries are only as deep as a symbol name is long. The cap exists purely
// to break cycles: an edge with a zero-length string pointing back at an
// ancestor consumes no name characters, so nothing else bounds the walk.
#define HK_EXPORT_TRIE_MAX_DEPTH 128u

typedef enum {
    HK_EXPORT_OK = 0,
    HK_EXPORT_NOT_FOUND,
    HK_EXPORT_MALFORMED,          // truncated, overlong ULEB, bad offset, or cyclic
    HK_EXPORT_INVALID_ARGUMENT,
    HK_EXPORT_UNSUPPORTED_KIND,   // found, but it is a re-export (see below)
} hk_export_status_t;

typedef struct {
    uint64_t flags;

    // For a regular export, the symbol's offset FROM THE IMAGE BASE (the
    // mach header address) -- not a file offset and not an absolute address.
    // For a stub-and-resolver export this is the stub offset, with the
    // resolver offset in `other`.
    uint64_t address;
    uint64_t other;

    bool is_weak;
    bool is_thread_local;
    bool is_absolute;
    bool is_reexport;
    bool is_stub_and_resolver;
} hk_export_symbol_t;

// Looks up `name` in the export trie at [trie, trie+size).
//
// EXACT NAME ONLY: the trie stores names in linker form, so a C symbol
// `malloc` is present as `_malloc` and must be queried that way. Convention
// normalization (the leading-underscore handling HKSymbolTable does
// internally) deliberately is NOT repeated here -- resolver selection owns
// convention normalization above this parser.
//
// Returns HK_EXPORT_UNSUPPORTED_KIND for a re-export: those name another
// dylib rather than carrying an address, so resolving one requires following
// into a different image via the image catalog. The out-parameter is still
// filled (flags, `other` = the library ordinal) so a caller can see what it
// found; it simply has no address to offer yet.
hk_export_status_t hk_export_trie_find(const void *trie, size_t size,
                                       const char *name,
                                       hk_export_symbol_t *out_symbol);

// Exposed for its own sake because it is the piece most worth testing
// directly: decodes one ULEB128 at *offset, advancing it. Returns false on a
// truncated, overlong (>10 byte), or 64-bit-overflowing encoding.
bool hk_export_read_uleb128(const uint8_t *buffer, size_t size,
                            size_t *offset, uint64_t *out_value);

#ifdef __cplusplus
}
#endif

#endif // HK_RESOLVERS_EXPORT_TRIE_H
