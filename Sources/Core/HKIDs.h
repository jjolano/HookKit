// HookKit 3.0 -- process-lifetime-unique ID generation. Internal; every
// hk_runtime_t/hk_plan_t/hk_hook_t/etc. gets one of these at creation.
// See docs/3.0/PUBLIC_C_ABI.md, "Stable IDs".

#ifndef HK_CORE_IDS_H
#define HK_CORE_IDS_H

#include "../../Headers/HookKit/HookKitBase.h"

#ifdef __cplusplus
extern "C" {
#endif

// One process-instance nonce (computed once, lazily, on first call) plus
// an atomically-incremented monotonic counter -- spec section 6.3. Two IDs
// from the same process share `high`; `low` is unique per call and
// monotonically increasing, so it also orders creation within a process.
// No cross-process meaning is claimed: the nonce is time+pid+ASLR entropy,
// not cryptographic -- process-lifetime uniqueness is all the ABI promises,
// so there's no reason to pull in arc4random and its own availability
// questions across the iOS 9 lane's older deployment floor.
hk_id_t hk_id_generate(void);

#ifdef __cplusplus
}
#endif

#endif // HK_CORE_IDS_H
