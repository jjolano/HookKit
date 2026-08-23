// Internal bridge used by the canonical HKSubstitutor facade. Its five
// mutating operations use the 3.0 plan/engine lifecycle.

#ifndef HK_LEGACY_FACADE3_H
#define HK_LEGACY_FACADE3_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int hk3_legacy_hook_objc(void *dispatch_class, void *selector,
                         void *replacement, void **out_original);
int hk3_legacy_hook_function(void *function, void *replacement,
                             void **out_original);
int hk3_legacy_hook_memory(void *target, const void *data, size_t size);
int hk3_legacy_hook_swift_method(void *metadata, const char *name,
                                 void *replacement, void **out_original);
int hk3_legacy_hook_swift_slot(void *metadata, uint32_t index,
                               void *replacement, void **out_original);

#ifdef __cplusplus
}
#endif

#endif
