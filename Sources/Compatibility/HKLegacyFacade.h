// Internal bridge used by the canonical HKSubstitutor facade. Its five
// mutating operations use the 3.0 plan/engine lifecycle.

#ifndef HK_LEGACY_FACADE3_H
#define HK_LEGACY_FACADE3_H

#include <stddef.h>
#include <stdint.h>

#include "../../Headers/HookKit/HookKit.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    HK_LEGACY_OK = 0,
    HK_LEGACY_ERR = 1 << 0,
    HK_LEGACY_ERR_NOT_SUPPORTED = 1 << 1,
    HK_LEGACY_ERR_INVALID_ARGUMENT = 1 << 2,
    HK_LEGACY_ERR_PARTIAL = 1 << 3,
};

int hk_legacy_hook_objc(void *dispatch_class, void *selector,
                         void *replacement, void **out_original);
int hk_legacy_hook_function(void *function, void *replacement,
                             void **out_original);
int hk_legacy_hook_memory(void *target, const void *data, size_t size);
int hk_legacy_hook_swift_method(void *metadata, const char *name,
                                 void *replacement, void **out_original);
int hk_legacy_hook_swift_slot(void *metadata, uint32_t index,
                               void *replacement, void **out_original);

// Spec builders for the facade's batched path. `id_buf` receives the unique
// stable_hook_id (plans reject duplicate ids), which must stay alive until
// hk_plan_add_hook deep-copies the spec. Same validation as the single calls.
int hk_legacy_build_objc_spec(void *dispatch_class, void *selector,
                               void *replacement, void **out_original,
                               char *id_buf, size_t id_cap,
                               hk_hook_spec_t *out_spec);
int hk_legacy_build_function_spec(void *function, void *replacement,
                                   void **out_original,
                                   char *id_buf, size_t id_cap,
                                   hk_hook_spec_t *out_spec);

// Apply N pre-built specs through ONE shared runtime/plan lifecycle.
// originals[i] is the caller's out_old_ptr slot (may be NULL); it is written
// exactly like the single-call path (early publication at prepare where the
// engine supports it, final value after commit, cleared on failure).
// out_results[i] receives an HK_LEGACY_* status per op. Returns the aggregate:
// HK_LEGACY_OK if all succeeded, HK_LEGACY_ERR_PARTIAL if some, HK_LEGACY_ERR otherwise.
int hk_legacy_apply_specs(const hk_hook_spec_t *specs,
                           void **const *originals,
                           size_t count,
                           int *out_results);

#ifdef __cplusplus
}
#endif

#endif
