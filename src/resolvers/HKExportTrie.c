// Mach-O export trie walking. See HKExportTrie.h for the format, the reuse
// survey, and the safety properties.
//
// Trie node layout (dyld's format):
//   terminal_size : ULEB128
//   if terminal_size > 0:
//       flags : ULEB128
//       then, depending on flags:
//           REEXPORT          -> ordinal : ULEB128, imported_name : cstring
//           STUB_AND_RESOLVER -> stub : ULEB128, resolver : ULEB128
//           otherwise         -> address : ULEB128
//   child_count : uint8
//   child_count times:
//       edge_string : cstring
//       child_offset : ULEB128   (from the start of the trie)

#include "HKExportTrie.h"

#include <string.h>

bool hk_export_read_uleb128(const uint8_t *buffer, size_t size,
                            size_t *offset, uint64_t *out_value) {
    if (!buffer || !offset || !out_value) {
        return false;
    }
    uint64_t result = 0;
    unsigned shift = 0;
    unsigned bytes_read = 0;

    for (;;) {
        if (*offset >= size) {
            return false;  // truncated: the encoding runs past the buffer
        }
        // A 64-bit ULEB128 is at most 10 bytes (10 * 7 = 70 bits); anything
        // longer is malformed, not merely large.
        if (++bytes_read > 10u) {
            return false;
        }
        uint8_t byte = buffer[(*offset)++];
        uint64_t chunk = (uint64_t)(byte & 0x7fu);

        // At shift 63 only one bit still fits; any wider value would overflow.
        if (shift == 63u && chunk > 1u) {
            return false;
        }
        if (shift < 64u) {
            result |= chunk << shift;
        } else if (chunk != 0u) {
            return false;  // bits beyond 64
        }

        if ((byte & 0x80u) == 0u) {
            break;
        }
        shift += 7u;
    }
    *out_value = result;
    return true;
}

// Decodes a terminal node's payload. Reads are bounded by `terminal_end`, not
// just the trie: a terminal must stay inside its own declared size.
static hk_export_status_t decode_terminal(const uint8_t *trie, size_t offset,
                                          size_t terminal_end,
                                          hk_export_symbol_t *out) {
    uint64_t flags = 0;
    if (!hk_export_read_uleb128(trie, terminal_end, &offset, &flags)) {
        return HK_EXPORT_MALFORMED;
    }

    memset(out, 0, sizeof(*out));
    out->flags = flags;
    out->is_weak = (flags & HK_EXPORT_FLAGS_WEAK_DEFINITION) != 0;

    uint64_t kind = flags & HK_EXPORT_FLAGS_KIND_MASK;
    out->is_thread_local = (kind == HK_EXPORT_FLAGS_KIND_THREAD_LOCAL);
    out->is_absolute = (kind == HK_EXPORT_FLAGS_KIND_ABSOLUTE);

    if (flags & HK_EXPORT_FLAGS_REEXPORT) {
        out->is_reexport = true;
        uint64_t ordinal = 0;
        if (!hk_export_read_uleb128(trie, terminal_end, &offset, &ordinal)) {
            return HK_EXPORT_MALFORMED;
        }
        out->other = ordinal;
        // The imported-name string may follow; it is not needed to report
        // that this is a re-export, and following it requires another image.
        return HK_EXPORT_UNSUPPORTED_KIND;
    }

    if (flags & HK_EXPORT_FLAGS_STUB_AND_RESOLVER) {
        out->is_stub_and_resolver = true;
        uint64_t stub = 0, resolver = 0;
        if (!hk_export_read_uleb128(trie, terminal_end, &offset, &stub) ||
            !hk_export_read_uleb128(trie, terminal_end, &offset, &resolver)) {
            return HK_EXPORT_MALFORMED;
        }
        out->address = stub;
        out->other = resolver;
        return HK_EXPORT_OK;
    }

    uint64_t address = 0;
    if (!hk_export_read_uleb128(trie, terminal_end, &offset, &address)) {
        return HK_EXPORT_MALFORMED;
    }
    out->address = address;
    return HK_EXPORT_OK;
}

hk_export_status_t hk_export_trie_find(const void *trie_base, size_t size,
                                       const char *name,
                                       hk_export_symbol_t *out_symbol) {
    if (!trie_base || !name || !out_symbol || size == 0) {
        return HK_EXPORT_INVALID_ARGUMENT;
    }
    const uint8_t *trie = (const uint8_t *)trie_base;
    size_t node = 0;
    const char *remaining = name;

    for (unsigned depth = 0; depth <= HK_EXPORT_TRIE_MAX_DEPTH; depth++) {
        if (node >= size) {
            return HK_EXPORT_MALFORMED;  // child offset outside the trie
        }
        size_t p = node;

        uint64_t terminal_size = 0;
        if (!hk_export_read_uleb128(trie, size, &p, &terminal_size)) {
            return HK_EXPORT_MALFORMED;
        }
        if (terminal_size > (uint64_t)(size - p)) {
            return HK_EXPORT_MALFORMED;  // terminal data overruns the trie
        }

        // The whole name is consumed and this node carries a definition.
        if (*remaining == '\0' && terminal_size > 0) {
            return decode_terminal(trie, p, p + (size_t)terminal_size, out_symbol);
        }

        p += (size_t)terminal_size;  // skip any terminal data
        if (p >= size) {
            return HK_EXPORT_MALFORMED;  // no room for the child count
        }
        uint8_t child_count = trie[p++];

        bool descended = false;
        for (uint8_t i = 0; i < child_count; i++) {
            size_t edge_start = p;
            while (p < size && trie[p] != '\0') {
                p++;
            }
            if (p >= size) {
                return HK_EXPORT_MALFORMED;  // unterminated edge string
            }
            size_t edge_length = p - edge_start;
            p++;  // step over the NUL

            uint64_t child_offset = 0;
            if (!hk_export_read_uleb128(trie, size, &p, &child_offset)) {
                return HK_EXPORT_MALFORMED;
            }

            // First matching edge wins, as dyld does; remaining siblings are
            // irrelevant once we descend. A zero-length edge matches
            // trivially and consumes no name characters -- that is precisely
            // the cycle the depth cap exists to break.
            if (!descended &&
                strncmp(remaining, (const char *)(trie + edge_start), edge_length) == 0) {
                remaining += edge_length;
                node = (size_t)child_offset;
                descended = true;
                break;
            }
        }
        if (!descended) {
            return HK_EXPORT_NOT_FOUND;
        }
    }
    // Past the depth cap: the trie is cyclic (or absurdly deep), which cannot
    // be distinguished from corruption and is treated the same way.
    return HK_EXPORT_MALFORMED;
}
