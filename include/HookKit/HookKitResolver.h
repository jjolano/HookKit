// Minimal loaded-image symbol lookup for native HookKit 3 consumers.

#ifndef HOOKKIT_RESOLVER_H
#define HOOKKIT_RESOLVER_H

#include "HookKitBase.h"
#include "HookKitRuntime.h"

#ifdef __cplusplus
extern "C" {
#endif

// Finds a symbol in a currently loaded image without creating a hook.
// `image_path == NULL` searches the loaded-image set; otherwise the match is
// an exact loaded path. Names use the same C/Mach-O normalization as HookKit
// symbol targets, and private symbols are allowed. Returns HK_STATUS_UNAVAILABLE
// when the image or symbol cannot be resolved.
hk_status_t hk_runtime_find_symbol(
    hk_runtime_t *runtime,
    const char *image_path,
    const char *symbol_name,
    void **out_address);

#ifdef __cplusplus
}
#endif

#endif // HOOKKIT_RESOLVER_H
