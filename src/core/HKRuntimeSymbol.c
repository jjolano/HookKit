#include "HKRuntimeInternal.h"

#if defined(__APPLE__)
#include "HKImageCatalog.h"
#include "../resolvers/HKMachO.h"
#include "../resolvers/HKSymbolResolve.h"

#include <dlfcn.h>
#include <mach-o/dyld.h>

#if !defined(__LP64__)
#include <pthread.h>
#include "../../vendor/substitute/substitute.h"
#endif
#endif

#include <string.h>
#include "../internal/HKPointerAuth.h"

#if defined(__APPLE__) && defined(__LP64__)
typedef struct {
    const char *symbol_name;
    void *address;
    bool classification_failed;
} hk_runtime_symbol_lookup_t;

static bool hk_runtime_find_symbol_64(void *opaque, size_t index,
                                      const hk_image_entry_t *entry) {
    (void)index;
    hk_runtime_symbol_lookup_t *lookup = opaque;
    hk_macho_header_t header;
    if (!entry || !entry->header ||
        hk_macho_peek_header(entry->header, HK_MACHO_HEADER_64_SIZE,
                             &header) != HK_MACHO_OK) {
        return true;
    }

    hk_symbol_resolution_t resolved;
    if (hk_resolve_loaded_image_symbol(
            entry->header, HK_MACHO_HEADER_64_SIZE + header.sizeofcmds,
            entry->slide, lookup->symbol_name, HK_SYMBOL_NAME_C,
            HK_SYMBOL_VISIBILITY_PRIVATE_ALLOWED, &resolved) == HK_RESOLVE_OK &&
        resolved.address != 0) {
        bool is_code = false;
        hk_macho_status_t classification;
        if (resolved.source == HK_RESOLVE_SOURCE_SYMBOL_TABLE &&
            resolved.n_sect != 0) {
            uint32_t flags = 0;
            classification = hk_macho_section_flags(
                entry->header, HK_MACHO_HEADER_64_SIZE + header.sizeofcmds,
                resolved.n_sect, &flags);
            is_code = classification == HK_MACHO_OK &&
                      hk_macho_section_is_code(flags);
        } else {
            classification = hk_macho_runtime_address_is_code(
                entry->header, HK_MACHO_HEADER_64_SIZE + header.sizeofcmds,
                entry->slide, resolved.address, &is_code);
        }
        if (classification != HK_MACHO_OK) {
            lookup->classification_failed = true;
            return false;
        }
        lookup->address = (void *)(is_code
            ? hk_pac_make_callable(resolved.address) : resolved.address);
        return false;
    }
    return true;
}
#endif

#if defined(__APPLE__) && !defined(__LP64__)
typedef void *(*hk_ms_get_image_fn)(const char *);
typedef void *(*hk_ms_find_symbol_fn)(void *, const char *);
typedef struct substitute_image *(*hk_sub_open_image_fn)(const char *);
typedef void (*hk_sub_close_image_fn)(struct substitute_image *);
typedef int (*hk_sub_find_private_fn)(struct substitute_image *, const char **,
                                      void **, size_t);

typedef struct {
    pthread_mutex_t lock;
    void *handle;
    hk_ms_get_image_fn get_image;
    hk_ms_find_symbol_fn find_symbol;
    hk_sub_open_image_fn open_image;
    hk_sub_close_image_fn close_image;
    hk_sub_find_private_fn find_private;
} hk_runtime_legacy_resolver_t;

static hk_runtime_legacy_resolver_t g_legacy_resolver = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

static void *hk_runtime_resolver_symbol(void *handle, const char *first,
                                        const char *second) {
    void *symbol = dlsym(handle, first);
    return symbol ? symbol : (second ? dlsym(handle, second) : NULL);
}

static bool hk_runtime_load_legacy_resolver_locked(
    hk_runtime_legacy_resolver_t *resolver) {
    if ((resolver->get_image && resolver->find_symbol) ||
        (resolver->open_image && resolver->close_image && resolver->find_private)) {
        return true;
    }

    hk_ms_get_image_fn get_image = (hk_ms_get_image_fn)hk_runtime_resolver_symbol(
        RTLD_DEFAULT, "MSGetImageByName", "SubGetImageByName");
    hk_ms_find_symbol_fn find_symbol = (hk_ms_find_symbol_fn)hk_runtime_resolver_symbol(
        RTLD_DEFAULT, "MSFindSymbol", "SubFindSymbol");
    hk_sub_open_image_fn open_image = (hk_sub_open_image_fn)dlsym(
        RTLD_DEFAULT, "substitute_open_image");
    hk_sub_close_image_fn close_image = (hk_sub_close_image_fn)dlsym(
        RTLD_DEFAULT, "substitute_close_image");
    hk_sub_find_private_fn find_private = (hk_sub_find_private_fn)dlsym(
        RTLD_DEFAULT, "substitute_find_private_syms");

    if ((get_image && find_symbol) || (open_image && close_image && find_private)) {
        resolver->get_image = get_image;
        resolver->find_symbol = find_symbol;
        resolver->open_image = open_image;
        resolver->close_image = close_image;
        resolver->find_private = find_private;
        return true;
    }

    static const char *const paths[] = {
        "/usr/lib/libsubstitute.0.dylib",
        "/Library/Frameworks/CydiaSubstrate.framework/CydiaSubstrate",
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        void *handle = dlopen(paths[i], RTLD_LAZY | RTLD_LOCAL);
        if (!handle) {
            continue;
        }
        get_image = (hk_ms_get_image_fn)hk_runtime_resolver_symbol(
            handle, "MSGetImageByName", "SubGetImageByName");
        find_symbol = (hk_ms_find_symbol_fn)hk_runtime_resolver_symbol(
            handle, "MSFindSymbol", "SubFindSymbol");
        open_image = (hk_sub_open_image_fn)dlsym(handle, "substitute_open_image");
        close_image = (hk_sub_close_image_fn)dlsym(handle, "substitute_close_image");
        find_private = (hk_sub_find_private_fn)dlsym(
            handle, "substitute_find_private_syms");
        if ((get_image && find_symbol) || (open_image && close_image && find_private)) {
            resolver->handle = handle;
            resolver->get_image = get_image;
            resolver->find_symbol = find_symbol;
            resolver->open_image = open_image;
            resolver->close_image = close_image;
            resolver->find_private = find_private;
            return true;
        }
        dlclose(handle);
    }
    return false;
}

static void *hk_runtime_find_legacy_in_image(const char *path,
                                             const char *symbol_name) {
    hk_runtime_legacy_resolver_t *resolver = &g_legacy_resolver;
    pthread_mutex_lock(&resolver->lock);
    bool ready = hk_runtime_load_legacy_resolver_locked(resolver);
    hk_ms_get_image_fn get_image = resolver->get_image;
    hk_ms_find_symbol_fn find_symbol = resolver->find_symbol;
    hk_sub_open_image_fn open_image = resolver->open_image;
    hk_sub_close_image_fn close_image = resolver->close_image;
    hk_sub_find_private_fn find_private = resolver->find_private;
    pthread_mutex_unlock(&resolver->lock);
    if (!ready) {
        return NULL;
    }

    if (open_image && close_image && find_private) {
        struct substitute_image *image = open_image(path);
        if (image) {
            const char *names[] = { symbol_name };
            void *addresses[] = { NULL };
            (void)find_private(image, names, addresses, 1);
            close_image(image);
            if (addresses[0]) {
                return addresses[0];
            }
        }
    }
    if (get_image && find_symbol) {
        void *image = get_image(path);
        if (image) {
            return find_symbol(image, symbol_name);
        }
    }
    return NULL;
}

static void *hk_runtime_find_symbol_legacy(const char *image_path,
                                           const char *symbol_name) {
    if (image_path) {
        return hk_runtime_find_legacy_in_image(image_path, symbol_name);
    }

    void *address = dlsym(RTLD_DEFAULT, symbol_name);
    if (!address && symbol_name[0] == '_') {
        address = dlsym(RTLD_DEFAULT, symbol_name + 1);
    }
    if (address) {
        return address;
    }
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const char *path = _dyld_get_image_name(i);
        address = path ? hk_runtime_find_legacy_in_image(path, symbol_name) : NULL;
        if (address) {
            return address;
        }
    }
    return NULL;
}
#endif

hk_status_t hk_runtime_find_symbol(hk_runtime_t *runtime, const char *image_path,
                                   const char *symbol_name, void **out_address) {
    if (out_address) {
        *out_address = NULL;
    }
    if (!runtime || !symbol_name || !symbol_name[0] || !out_address) {
        return HK_STATUS_INVALID_ARGUMENT;
    }

#if defined(__APPLE__) && defined(__LP64__)
    if (!runtime->catalog) {
        return HK_STATUS_UNAVAILABLE;
    }
    hk_image_selector_t selector;
    memset(&selector, 0, sizeof(selector));
    selector.struct_size = sizeof(selector);
    selector.struct_version = HK_ABI_VERSION_3_0;
    selector.kind = image_path ? HK_IMAGE_EXACT_PATH : HK_IMAGE_ANY_LOADED;
    selector.path = image_path;

    hk_runtime_symbol_lookup_t lookup = {
        .symbol_name = symbol_name,
    };
    (void)hk_image_catalog_match(runtime->catalog, &selector,
                                 hk_runtime_find_symbol_64, &lookup);
    if (lookup.classification_failed) {
        return HK_STATUS_UNAVAILABLE;
    }
    *out_address = lookup.address;
    return lookup.address ? HK_STATUS_OK : HK_STATUS_UNAVAILABLE;
#elif defined(__APPLE__)
    *out_address = hk_runtime_find_symbol_legacy(image_path, symbol_name);
    return *out_address ? HK_STATUS_OK : HK_STATUS_UNAVAILABLE;
#else
    return HK_STATUS_UNAVAILABLE;
#endif
}
