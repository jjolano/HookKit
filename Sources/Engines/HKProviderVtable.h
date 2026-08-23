// Provider inline adapter seam. The two concrete adapters share the same
// lifecycle; only discovery, activation, validation, and the vendor call vary.

#ifndef HK_ENGINES_PROVIDER_VTABLE_H
#define HK_ENGINES_PROVIDER_VTABLE_H

#include <stdbool.h>
#include <stdint.h>

#include "../Core/HKEngineInternal.h"
#include "HKRelocInlineEngine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HK_PROVIDER_DOBBY = 1,
    HK_PROVIDER_GUM = 2,
    HK_PROVIDER_ELLEKIT = 3,
    // Cydia Substrate / libsubstitute's audited function-hook ABI.  This is
    // a 3.0 engine adapter, not the retired 2.x backend router.
    HK_PROVIDER_SUBSTITUTE = 4,
} hk_provider_kind_t;

// All callbacks are supplied by the platform binding or a host test seam.
// discover/validate are read-only. prepare may activate a provider but must
// not touch the requested target. hook returns zero only on the provider's
// reported success. out_original may be NULL when HookKit supplied the
// continuation and the provider ABI supports its no-original mode.
typedef bool (*hk_provider_discover_fn)(void *provider_ctx);
typedef bool (*hk_provider_prepare_fn)(void *provider_ctx,
                                       hk_prepare_diag_t *out_diag);
typedef bool (*hk_provider_validate_fn)(void *provider_ctx,
                                        const hk_hook_spec_t *spec,
                                        hk_prepare_diag_t *out_diag);
typedef int (*hk_provider_hook_fn)(void *provider_ctx,
                                   void *target,
                                   void *replacement,
                                   void **out_original);
typedef bool (*hk_provider_verify_fn)(void *provider_ctx,
                                      void *target,
                                      void *replacement);
// Optional safe read used to capture/revalidate provider entry bytes without
// dereferencing an untrusted target directly.  A NULL callback preserves the
// established direct-read path for providers whose validate callback proves
// the range readable.
typedef bool (*hk_provider_read_fn)(void *provider_ctx, const void *target,
                                    uint8_t *out, size_t size);

// Caller-owned, like every other engine context. There is deliberately no
// provider factory: each audited provider supplies only its differing
// callbacks through this one seam.
typedef struct {
    hk_provider_kind_t kind;
    void *provider_ctx;
    hk_provider_discover_fn discover;
    hk_provider_prepare_fn prepare;
    hk_provider_validate_fn validate;
    hk_provider_hook_fn hook;
    hk_provider_verify_fn verify;
    hk_provider_read_fn read;

    // Optional hybrid continuation seam. `max_overwrite_size` is an audited
    // provider-ABI bound, not a guess: HookKit relocates that whole span
    // before asking a late-publishing provider to patch the target.
    size_t max_overwrite_size;
    hk_reloc_alloc_fn alloc;
    hk_reloc_seal_fn seal;
    hk_reloc_free_fn free_page;
    void *seam_ctx;
} hk_provider_engine_ctx_t;

const hk_engine_vtable_t *hk_dobby_provider_vtable(void);
const hk_engine_vtable_t *hk_gum_provider_vtable(void);
const hk_engine_vtable_t *hk_ellekit_provider_vtable(void);
const hk_engine_vtable_t *hk_substitute_provider_vtable(void);

#ifdef __cplusplus
}
#endif

#endif // HK_ENGINES_PROVIDER_VTABLE_H
