#include "hk_native.h"
#include "../Internal/HKPointerAuth.h"

#if defined(__arm64__) || defined(__aarch64__)

#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct hk_image {
    void *map;                      // owned mmap of an on-disk Mach-O, or NULL for cache images
    size_t map_size;
    intptr_t slide;
    const struct nlist_64 *nlist;
    uint32_t nlist_count;
    const char *strings;
    uint32_t strings_size;
    void *dl_handle;                // fallback for exported symbols
    uint32_t *sect_flags;           // section flags in n_sect order (loaded image), or NULL
    uint32_t sect_count;
};

// Pointer authentication (arm64e only; identity on plain arm64): private
// symbols that resolve into instruction sections are returned signed with the
// function-pointer key, discriminator 0 -- the same recipe hk_native.c uses
// for the originals it hands out. Data symbols stay unsigned.
#define hk_sym_strip(p) ((void *)hk_pac_strip_code((uintptr_t)(p)))
#define hk_sym_sign(p)  ((void *)hk_pac_make_callable((uintptr_t)(p)))

#pragma mark - Loaded image lookup

static bool find_loaded_image(const char *path, const struct mach_header_64 **out_header, intptr_t *out_slide) {
    uint32_t count = _dyld_image_count();

    for(uint32_t i = 0; i < count; i++) {
        const char *name = _dyld_get_image_name(i);

        if(name && strcmp(name, path) == 0) {
            *out_header = (const struct mach_header_64 *)_dyld_get_image_header(i);
            *out_slide = _dyld_get_image_vmaddr_slide(i);
            return true;
        }
    }

    // Second pass through symlinks. Shared cache images have no file on disk,
    // so realpath fails for them -- they can only match the pass above.
    char resolved[PATH_MAX];

    if(!realpath(path, resolved)) {
        return false;
    }

    for(uint32_t i = 0; i < count; i++) {
        const char *name = _dyld_get_image_name(i);
        char other[PATH_MAX];

        if(name && realpath(name, other) && strcmp(other, resolved) == 0) {
            *out_header = (const struct mach_header_64 *)_dyld_get_image_header(i);
            *out_slide = _dyld_get_image_vmaddr_slide(i);
            return true;
        }
    }

    return false;
}

#pragma mark - dyld shared cache local symbols

// Cached images keep their local symbols in the cache files but outside the
// mapped regions, so they have to be read from disk. iOS 16+ splits them into a
// .symbols subcache; iOS 15 and earlier embed them in the main cache. Both use
// the same header layout, so one probe loop covers both.

extern const void *_dyld_get_shared_cache_range(size_t *length) __attribute__((weak_import));

// Byte offsets into dyld_cache_header. Fixed since the format was introduced;
// the header only ever grew past them.
#define HK_DCH_MAGIC              0
#define HK_DCH_MAPPING_OFFSET     16
#define HK_DCH_LOCAL_SYMS_OFFSET  72
#define HK_DCH_LOCAL_SYMS_SIZE    80
#define HK_DCH_MIN_MAPPING_OFFSET 88    // header must reach at least this far for the fields above to exist

struct hk_local_symbols_info {
    uint32_t nlistOffset;
    uint32_t nlistCount;
    uint32_t stringsOffset;
    uint32_t stringsSize;
    uint32_t entriesOffset;
    uint32_t entriesCount;
};

struct hk_cache_symbols {
    const uint8_t *map;
    size_t map_size;
    const struct nlist_64 *nlist;
    uint32_t nlist_count;
    const char *strings;
    uint32_t strings_size;
    const uint8_t *entries;
    uint32_t entries_count;
    size_t entry_stride;            // 12 (uint32 dylibOffset) or 16 (uint64)
    // Sorted (dylibOffset, entryIndex) index over `entries`, built once so
    // bind_cache_symbols is O(log entries) instead of a linear scan per
    // image. The NULL-image private-symbol scan opens every loaded image, so
    // the linear scan was ~5000 comparisons × ~600 images per lookup.
    struct hk_cache_entry_index *entry_index;
    uint32_t entry_index_count;
    bool valid;
};

struct hk_cache_entry_index {
    uint64_t dylib_offset;
    uint32_t entry_index;
};

static int entry_index_cmp(const void *a, const void *b) {
    uint64_t da = ((const struct hk_cache_entry_index *)a)->dylib_offset;
    uint64_t db = ((const struct hk_cache_entry_index *)b)->dylib_offset;
    return da < db ? -1 : da > db ? 1 : 0;
}

static uint64_t entry_dylib_offset(const uint8_t *entry, size_t stride) {
    if(stride == 16) {
        uint64_t value;
        memcpy(&value, entry, sizeof(value));
        return value;
    }

    uint32_t value;
    memcpy(&value, entry, sizeof(value));
    return value;
}

static uint32_t entry_nlist_start(const uint8_t *entry, size_t stride) {
    uint32_t value;
    memcpy(&value, entry + (stride == 16 ? 8 : 4), sizeof(value));
    return value;
}

static uint32_t entry_nlist_count(const uint8_t *entry, size_t stride) {
    uint32_t value;
    memcpy(&value, entry + (stride == 16 ? 12 : 8), sizeof(value));
    return value;
}

// Discriminates the 32-bit and 64-bit entry layouts: with the wrong stride the
// nlist ranges read as garbage and overrun the symbol table almost immediately.
static bool entries_plausible(const uint8_t *entries, uint32_t count, size_t stride,
                              uint32_t nlist_total, size_t available) {
    if(count == 0 || (size_t)count * stride > available) {
        return false;
    }

    uint32_t probe = count < 8 ? count : 8;

    for(uint32_t i = 0; i < probe; i++) {
        const uint8_t *entry = entries + ((size_t)i * stride);

        if((uint64_t)entry_nlist_start(entry, stride) + entry_nlist_count(entry, stride) > nlist_total) {
            return false;
        }
    }

    return true;
}

static bool parse_cache_symbols(const uint8_t *map, size_t map_size, struct hk_cache_symbols *out) {
    if(map_size < 0x100 || memcmp(map + HK_DCH_MAGIC, "dyld_v1", 7) != 0) {
        return false;
    }

    uint32_t mapping_offset;
    memcpy(&mapping_offset, map + HK_DCH_MAPPING_OFFSET, sizeof(mapping_offset));

    if(mapping_offset < HK_DCH_MIN_MAPPING_OFFSET) {
        return false;
    }

    uint64_t syms_offset;
    uint64_t syms_size;
    memcpy(&syms_offset, map + HK_DCH_LOCAL_SYMS_OFFSET, sizeof(syms_offset));
    memcpy(&syms_size, map + HK_DCH_LOCAL_SYMS_SIZE, sizeof(syms_size));

    if(syms_offset == 0 || syms_size < sizeof(struct hk_local_symbols_info)) {
        return false;
    }

    if(syms_offset > map_size || syms_size > map_size - syms_offset) {
        return false;
    }

    const uint8_t *region = map + syms_offset;
    struct hk_local_symbols_info info;
    memcpy(&info, region, sizeof(info));

    // Every sub-range must sit inside the local symbols region.
    if((uint64_t)info.nlistOffset + ((uint64_t)info.nlistCount * sizeof(struct nlist_64)) > syms_size) {
        return false;
    }

    if((uint64_t)info.stringsOffset + info.stringsSize > syms_size) {
        return false;
    }

    if(info.entriesOffset > syms_size) {
        return false;
    }

    const uint8_t *entries = region + info.entriesOffset;
    size_t entries_available = (size_t)(syms_size - info.entriesOffset);
    size_t stride = 0;

    // A separate .symbols subcache implies the 64-bit layout, but probe rather
    // than assume: validate one and fall back to the other.
    if(entries_plausible(entries, info.entriesCount, 16, info.nlistCount, entries_available)) {
        stride = 16;
    } else if(entries_plausible(entries, info.entriesCount, 12, info.nlistCount, entries_available)) {
        stride = 12;
    } else {
        return false;
    }

    out->map = map;
    out->map_size = map_size;
    out->nlist = (const struct nlist_64 *)(region + info.nlistOffset);
    out->nlist_count = info.nlistCount;
    out->strings = (const char *)(region + info.stringsOffset);
    out->strings_size = info.stringsSize;
    out->entries = entries;
    out->entries_count = info.entriesCount;
    out->entry_stride = stride;
    out->valid = true;
    return true;
}

// Mapped once for the process lifetime: the region is large and read-only, and
// every cached image needs it.
static const struct hk_cache_symbols *cache_symbols(void) {
    static struct hk_cache_symbols symbols;
    static dispatch_once_t once = 0;

    dispatch_once(&once, ^{
        // Deliberately NOT routed through HKJBPath. That prefixes the
        // jailbreak root (/var/jb/...) onto paths a rootless install
        // relocates, and the shared cache is not one of them -- it stays at
        // its system path on every jailbreak. Prefixing here would only ever
        // produce paths that do not exist.
        static const char *candidates[] = {
            "/System/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_arm64e.symbols",
            "/System/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_arm64.symbols",
            "/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64e",
            "/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64"
        };

        for(size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
            int fd = open(candidates[i], O_RDONLY);

            if(fd < 0) {
                continue;
            }

            struct stat st;

            if(fstat(fd, &st) != 0 || st.st_size <= 0) {
                close(fd);
                continue;
            }

            void *map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
            close(fd);

            if(map == MAP_FAILED) {
                continue;
            }

            if(parse_cache_symbols((const uint8_t *)map, (size_t)st.st_size, &symbols)) {
                // Index the entries once (process lifetime, like the map):
                // bind_cache_symbols binary-searches it. The table is static
                // while the process lives — the cache file never changes.
                symbols.entry_index = malloc((size_t)symbols.entries_count * sizeof(struct hk_cache_entry_index));

                if(symbols.entry_index) {
                    for(uint32_t i = 0; i < symbols.entries_count; i++) {
                        symbols.entry_index[i].dylib_offset = entry_dylib_offset(symbols.entries + ((size_t)i * symbols.entry_stride), symbols.entry_stride);
                        symbols.entry_index[i].entry_index = i;
                    }

                    qsort(symbols.entry_index, symbols.entries_count, sizeof(struct hk_cache_entry_index), entry_index_cmp);
                    symbols.entry_index_count = symbols.entries_count;
                }

                return;
            }

            munmap(map, (size_t)st.st_size);
        }
    });

    return symbols.valid ? &symbols : NULL;
}

// A cached dylib's mach_header and the cache base are both slid by the same
// amount, so their difference is exactly the unslid file offset the local
// symbols entries are keyed by.
static bool cache_image_offset(const void *header, uint64_t *out_offset) {
    if(!_dyld_get_shared_cache_range) {
        return false;
    }

    size_t length = 0;
    const void *base = _dyld_get_shared_cache_range(&length);

    if(!base || length == 0) {
        return false;
    }

    uintptr_t addr = (uintptr_t)header;
    uintptr_t start = (uintptr_t)base;

    if(addr < start || addr >= start + length) {
        return false;
    }

    *out_offset = (uint64_t)(addr - start);
    return true;
}

static bool bind_entry(const struct hk_cache_symbols *symbols, const uint8_t *entry, struct hk_image *image) {
    image->nlist = symbols->nlist + entry_nlist_start(entry, symbols->entry_stride);
    image->nlist_count = entry_nlist_count(entry, symbols->entry_stride);
    image->strings = symbols->strings;
    image->strings_size = symbols->strings_size;
    return true;
}

static bool bind_cache_symbols(const void *header, struct hk_image *image) {
    const struct hk_cache_symbols *symbols = cache_symbols();
    uint64_t dylib_offset = 0;

    if(!symbols || !cache_image_offset(header, &dylib_offset)) {
        return false;
    }

    if(symbols->entry_index) {
        // Binary search the sorted index; the linear scan is the OOM fallback.
        size_t lo = 0;
        size_t hi = symbols->entry_index_count;

        while(lo < hi) {
            size_t mid = (lo + hi) / 2;

            if(symbols->entry_index[mid].dylib_offset < dylib_offset) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }

        if(lo < symbols->entry_index_count && symbols->entry_index[lo].dylib_offset == dylib_offset) {
            return bind_entry(symbols, symbols->entries + ((size_t)symbols->entry_index[lo].entry_index * symbols->entry_stride), image);
        }

        return false;
    }

    for(uint32_t i = 0; i < symbols->entries_count; i++) {
        const uint8_t *entry = symbols->entries + ((size_t)i * symbols->entry_stride);

        if(entry_dylib_offset(entry, symbols->entry_stride) != dylib_offset) {
            continue;
        }

        return bind_entry(symbols, entry, image);
    }

    return false;
}

#pragma mark - On-disk Mach-O symbol table

static bool bind_ondisk_symbols(const char *path, struct hk_image *image) {
    int fd = open(path, O_RDONLY);

    if(fd < 0) {
        return false;
    }

    struct stat st;

    if(fstat(fd, &st) != 0 || (size_t)st.st_size < sizeof(struct mach_header_64)) {
        close(fd);
        return false;
    }

    size_t size = (size_t)st.st_size;
    void *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    if(map == MAP_FAILED) {
        return false;
    }

    const struct mach_header_64 *mh = (const struct mach_header_64 *)map;

    // Thin arm64 images only. iOS ships thin binaries; a fat header here means
    // we simply fall through to dlsym.
    if(mh->magic != MH_MAGIC_64) {
        munmap(map, size);
        return false;
    }

    const uint8_t *cursor = (const uint8_t *)map + sizeof(struct mach_header_64);
    const uint8_t *limit = (const uint8_t *)map + size;

    if((uint64_t)sizeof(struct mach_header_64) + mh->sizeofcmds > size) {
        munmap(map, size);
        return false;
    }

    for(uint32_t i = 0; i < mh->ncmds; i++) {
        if(cursor + sizeof(struct load_command) > limit) {
            break;
        }

        const struct load_command *lc = (const struct load_command *)cursor;

        if(lc->cmdsize < sizeof(struct load_command) || cursor + lc->cmdsize > limit) {
            break;
        }

        if(lc->cmd == LC_SYMTAB && lc->cmdsize >= sizeof(struct symtab_command)) {
            const struct symtab_command *symtab = (const struct symtab_command *)lc;
            uint64_t symend = (uint64_t)symtab->symoff + ((uint64_t)symtab->nsyms * sizeof(struct nlist_64));
            uint64_t strend = (uint64_t)symtab->stroff + symtab->strsize;

            if(symtab->nsyms == 0 || symend > size || strend > size) {
                break;
            }

            image->map = map;
            image->map_size = size;
            image->nlist = (const struct nlist_64 *)((const uint8_t *)map + symtab->symoff);
            image->nlist_count = symtab->nsyms;
            image->strings = (const char *)map + symtab->stroff;
            image->strings_size = symtab->strsize;
            return true;
        }

        cursor += lc->cmdsize;
    }

    munmap(map, size);
    return false;
}

#pragma mark - Section flags

// Section flags of a *loaded* image's header, in n_sect order (1-based,
// concatenated across segments in load-command order). Used to tell code
// symbols (instruction sections, PAC-signed on arm64e) from data symbols
// (plain pointers). The header belongs to a live image, so the command list
// is dyld-validated; only a degenerate cmdsize is guarded.
static uint32_t *collect_section_flags(const struct mach_header_64 *header, uint32_t *out_count) {
    uint32_t count = 0;
    const uint8_t *cursor = (const uint8_t *)header + sizeof(struct mach_header_64);

    for(uint32_t i = 0; i < header->ncmds; i++) {
        const struct load_command *lc = (const struct load_command *)cursor;

        if(lc->cmdsize < sizeof(struct load_command)) {
            break;
        }

        if(lc->cmd == LC_SEGMENT_64) {
            count += ((const struct segment_command_64 *)lc)->nsects;
        }

        cursor += lc->cmdsize;
    }

    if(count == 0) {
        *out_count = 0;
        return NULL;
    }

    uint32_t *flags = malloc((size_t)count * sizeof(uint32_t));

    if(!flags) {
        *out_count = 0;
        return NULL;
    }

    uint32_t idx = 0;
    cursor = (const uint8_t *)header + sizeof(struct mach_header_64);

    for(uint32_t i = 0; i < header->ncmds; i++) {
        const struct load_command *lc = (const struct load_command *)cursor;

        if(lc->cmdsize < sizeof(struct load_command)) {
            break;
        }

        if(lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *seg = (const struct segment_command_64 *)lc;
            const struct section_64 *sect = (const struct section_64 *)(seg + 1);

            for(uint32_t s = 0; s < seg->nsects; s++) {
                flags[idx++] = sect[s].flags;
            }
        }

        cursor += lc->cmdsize;
    }

    *out_count = count;
    return flags;
}

#pragma mark - Public API

// Common open core. `want_dlopen` controls the dlsym fallback handle: the
// handle-based API keeps it (find_symbol falls back to dlsym for exported
// symbols when the symtab is missing), while the NULL-image scan path skips
// it — dlsym(RTLD_DEFAULT) already missed before the scan runs, so the
// handle's only unique case (a symbol exported solely by an RTLD_LOCAL
// image) is out of scope there, and dlopen+dlclose per image is the scan's
// dominant cost (~600 images per lookup).
static hk_image *open_image_common(const char *path, bool want_dlopen) {
    if(!path || !*path) {
        return NULL;
    }

    const struct mach_header_64 *header = NULL;
    intptr_t slide = 0;

    // Without a loaded image there is no slide, and therefore no runtime
    // address any symbol could resolve to.
    if(!find_loaded_image(path, &header, &slide)) {
        return NULL;
    }

    struct hk_image *image = calloc(1, sizeof(struct hk_image));

    if(!image) {
        return NULL;
    }

    image->slide = slide;

    if(want_dlopen) {
        image->dl_handle = dlopen(path, RTLD_LAZY | RTLD_LOCAL | RTLD_NOLOAD);
    }

    image->sect_flags = collect_section_flags(header, &image->sect_count);

    if(!bind_cache_symbols(header, image)) {
        bind_ondisk_symbols(path, image);
    }

    // Even with no symbol table the handle is useful: dlsym still resolves
    // exported symbols.
    return (hk_image *)image;
}

hk_image *hk_native_open_image(const char *path) {
    return open_image_common(path, true);
}

// Scan-path variant: no dlopen/dlclose per image (see open_image_common).
HK_INTERNAL hk_image *hk_native_open_image_scan(const char *path) {
    return open_image_common(path, false);
}

void hk_native_close_image(hk_image *image) {
    if(!image) {
        return;
    }

    if(image->map) {
        munmap(image->map, image->map_size);
    }

    if(image->dl_handle) {
        dlclose(image->dl_handle);
    }

    free(image->sect_flags);
    free(image);
}

// Substrate-style names arrive without the leading underscore that Mach-O
// symbol tables carry ("malloc"); C++ mangled names keep theirs.
static bool symbol_matches(const char *entry, const char *name) {
    if(strcmp(entry, name) == 0) {
        return true;
    }

    return entry[0] == '_' && strcmp(entry + 1, name) == 0;
}

static void *find_nlist_symbol(hk_image *image, const char *name,
                               uint64_t *out_raw_value) {
    if(image->nlist && image->strings) {
        for(uint32_t i = 0; i < image->nlist_count; i++) {
            const struct nlist_64 *symbol = &image->nlist[i];

            if(symbol->n_un.n_strx == 0 || symbol->n_un.n_strx >= image->strings_size) {
                continue;
            }

            if((symbol->n_type & N_TYPE) != N_SECT || symbol->n_value == 0) {
                continue;
            }

            if(symbol_matches(image->strings + symbol->n_un.n_strx, name)) {
                void *addr = (void *)(uintptr_t)((intptr_t)symbol->n_value + image->slide);

                if(out_raw_value) {
                    *out_raw_value = symbol->n_value;
                }

                // Sign code only: n_sect identifies the Mach-O section the
                // symbol lives in. Instruction sections (code) get the
                // function-pointer signature; data symbols and unknown
                // sections stay unsigned (fail closed -- the callers that
                // consume these strip them anyway).
                if(symbol->n_sect > 0 && symbol->n_sect <= image->sect_count
                   && (image->sect_flags[symbol->n_sect - 1]
                       & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS))) {
                    addr = hk_sym_sign(hk_sym_strip(addr));
                }

                return addr;
            }
        }
    }

    return NULL;
}

void *hk_native_find_symbol(hk_image *image, const char *name) {
    if(!image || !name || !*name) {
        return NULL;
    }

    void *found = find_nlist_symbol(image, name, NULL);
    if(found) {
        return found;
    }

    if(image->dl_handle) {
        return dlsym(image->dl_handle, name);
    }

    return NULL;
}

void *hk_native_find_loaded_cache_symbol(const void *header, intptr_t slide,
                                         const char *name,
                                         uint64_t *out_raw_value) {
    if(out_raw_value) {
        *out_raw_value = 0;
    }
    if(!header || !name || !*name) {
        return NULL;
    }

    struct hk_image image;
    memset(&image, 0, sizeof(image));
    image.slide = slide;
    image.sect_flags = collect_section_flags(header, &image.sect_count);
    if(!bind_cache_symbols(header, &image)) {
        free(image.sect_flags);
        return NULL;
    }

    void *found = find_nlist_symbol(&image, name, out_raw_value);
    free(image.sect_flags);
    return found;
}

#pragma mark - Loaded cache entries (fast NULL-image lookup)

// The NULL-image private-symbol lookup used to open every loaded image one at
// a time (find_loaded_image strcmp walk + bind_cache_symbols bsearch +
// collect_section_flags + linear nlist scan per image) — hundreds of images
// per lookup. The dyld shared cache's local-symbols table is already mapped
// once for the process (cache_symbols), so the fast path binds each LOADED
// cache image's nlist range ONCE and answers later lookups with a scan of the
// bound ranges only, no per-image open/scan/close. Non-cache images
// (jailbreak dylibs) are not covered here — the caller falls back to the
// backend walk for those.
//
// Only loaded entries are scanned: a symbol in an unloaded cache dylib has no
// runtime address (the old per-image walk could not find it either).
// ponytail: this is a linear scan over the loaded ranges per lookup (~100k
// nlists), not an index; a name hash table is the upgrade if profiling ever
// shows the scan is the hot spot.

struct hk_cache_loaded_entry {
    uint32_t nlist_start;
    uint32_t nlist_count;
    intptr_t slide;                  // this dylib's slide (cache images slide as one region)
    uint32_t *sect_flags;            // section flags in n_sect order (NULL if uncollected)
    uint32_t sect_count;
};

static struct hk_cache_loaded_entry *hk_cache_loaded = NULL;
static uint32_t hk_cache_loaded_count = 0;

// Binds every loaded cache image's nlist range + section flags once. Static
// while the process lives (the cache file never changes; entries keyed by
// dylib offset, which is slide-independent). dispatch_once: the same
// once-semantics as cache_symbols() — a concurrent NULL-image lookup must not
// double-build.
static void build_loaded_entries(void) {
    static dispatch_once_t once = 0;

    dispatch_once(&once, ^{
        const struct hk_cache_symbols *symbols = cache_symbols();

        if(symbols) {
            // Preallocate generously: at most one loaded entry per cache dylib,
            // and there cannot be more loaded images than _dyld_image_count().
            uint32_t image_count = _dyld_image_count();
            struct hk_cache_loaded_entry *entries =
                calloc((size_t)image_count, sizeof(struct hk_cache_loaded_entry));

            if(entries) {
                uint32_t used = 0;

                for(uint32_t i = 0; i < image_count; i++) {
                    const struct mach_header_64 *header = (const struct mach_header_64 *)_dyld_get_image_header(i);
                    uint64_t offset = 0;

                    if(!header || !cache_image_offset(header, &offset)) {
                        continue;   // not a cache image
                    }

                    // Find the entry for this dylib offset: binary search the
                    // sorted index when present, linear scan otherwise.
                    int found = -1;

                    if(symbols->entry_index) {
                        size_t lo = 0;
                        size_t hi = symbols->entry_index_count;

                        while(lo < hi) {
                            size_t mid = (lo + hi) / 2;

                            if(symbols->entry_index[mid].dylib_offset < offset) {
                                lo = mid + 1;
                            } else {
                                hi = mid;
                            }
                        }

                        if(lo < symbols->entry_index_count
                           && symbols->entry_index[lo].dylib_offset == offset) {
                            found = (int)symbols->entry_index[lo].entry_index;
                        }
                    } else {
                        for(uint32_t e = 0; e < symbols->entries_count; e++) {
                            const uint8_t *entry = symbols->entries + ((size_t)e * symbols->entry_stride);

                            if(entry_dylib_offset(entry, symbols->entry_stride) == offset) {
                                found = (int)e;
                                break;
                            }
                        }
                    }

                    if(found < 0) {
                        continue;
                    }

                    const uint8_t *entry = symbols->entries + ((size_t)found * symbols->entry_stride);
                    struct hk_cache_loaded_entry *le = &entries[used++];
                    le->nlist_start = entry_nlist_start(entry, symbols->entry_stride);
                    le->nlist_count = entry_nlist_count(entry, symbols->entry_stride);
                    le->slide = _dyld_get_image_vmaddr_slide(i);
                    le->sect_flags = collect_section_flags(header, &le->sect_count);
                }

                if(used > 0) {
                    hk_cache_loaded = entries;
                    hk_cache_loaded_count = used;
                } else {
                    free(entries);
                }
            }
        }
    });
}

// Fast NULL-image private-symbol lookup: one scan over the nlist ranges of
// the LOADED cache dylibs (bound once), with the same filters, slide math and
// code-symbol signing as hk_native_find_symbol. Exported symbols are served
// by dlsym at the call site; this covers the private/non-exported ones.
HK_INTERNAL void *hk_native_find_cache_symbol(const char *name) {
    if(!name || !*name) {
        return NULL;
    }

    build_loaded_entries();   // dispatch_once: safe to call concurrently

    if(!hk_cache_loaded) {
        return NULL;
    }

    const struct hk_cache_symbols *symbols = cache_symbols();

    if(!symbols) {
        return NULL;
    }

    for(uint32_t e = 0; e < hk_cache_loaded_count; e++) {
        const struct hk_cache_loaded_entry *le = &hk_cache_loaded[e];

        if((uint64_t)le->nlist_start + le->nlist_count > symbols->nlist_count) {
            continue;   // defensive; the parser validated the ranges
        }

        for(uint32_t i = 0; i < le->nlist_count; i++) {
            const struct nlist_64 *symbol = &symbols->nlist[le->nlist_start + i];

            if(symbol->n_un.n_strx == 0 || symbol->n_un.n_strx >= symbols->strings_size) {
                continue;
            }

            if((symbol->n_type & N_TYPE) != N_SECT || symbol->n_value == 0) {
                continue;
            }

            if(symbol_matches(symbols->strings + symbol->n_un.n_strx, name)) {
                void *addr = (void *)(uintptr_t)((intptr_t)symbol->n_value + le->slide);

                // Same code-symbol signing rule as hk_native_find_symbol.
                if(symbol->n_sect > 0 && symbol->n_sect <= le->sect_count
                   && le->sect_flags
                   && (le->sect_flags[symbol->n_sect - 1]
                       & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS))) {
                    addr = hk_sym_sign(hk_sym_strip(addr));
                }

                return addr;
            }
        }
    }

    return NULL;
}

#else   // !arm64

hk_image *hk_native_open_image(const char *path) {
    (void)path;
    return NULL;
}

hk_image *hk_native_open_image_scan(const char *path) {
    (void)path;
    return NULL;
}

void hk_native_close_image(hk_image *image) {
    (void)image;
}

void *hk_native_find_symbol(hk_image *image, const char *name) {
    (void)image; (void)name;
    return NULL;
}

void *hk_native_find_cache_symbol(const char *name) {
    (void)name;
    return NULL;
}

void *hk_native_find_loaded_cache_symbol(const void *header, intptr_t slide,
                                         const char *name,
                                         uint64_t *out_raw_value) {
    (void)header;
    (void)slide;
    (void)name;
    if(out_raw_value) {
        *out_raw_value = 0;
    }
    return NULL;
}

#endif
