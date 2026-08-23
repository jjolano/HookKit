// HookKit 3.0 -- base types shared by every new hk_ header.
//
// Pure C, no Foundation, no Objective-C. Milestone 3 (ABI freeze candidate)
// per docs/3.0/PUBLIC_C_ABI.md and docs/3.0/IMPLEMENTATION_STATUS.md.
// Packaged by canonical HookKit beneath <HookKit/>, alongside the retained
// Objective-C compatibility facade at <HookKit.h>.

#ifndef HOOKKIT_BASE_H
#define HOOKKIT_BASE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ABI version. Bump the minor/patch per docs/3.0/PUBLIC_C_ABI.md's
// versioning rules -- struct_size/struct_version on every extensible
// struct below is what lets an older consumer link against a newer
// runtime (unknown trailing fields ignored) and a newer consumer detect
// an older one (struct_size smaller than what it needs).
#define HK_ABI_VERSION_3_0 0x00030000u

// Every extensible public structure in the new ABI opens with these two
// fields, in this order, always named exactly this. A structure smaller
// than the minimum size needed for its required fields is rejected by the
// runtime; unknown trailing fields (struct_size larger than expected) are
// ignored, not errors.
#define HK_STRUCT_HEADER \
    uint32_t struct_size; \
    uint32_t struct_version

// Process-lifetime-stable identifier. One process-instance nonce plus an
// atomic/locked monotonic counter (implementation side, not part of the
// ABI) -- every runtime, plan, request, installed hook, domain, report,
// and artifact gets one. No cross-process meaning is claimed; a
// caller-supplied stable_hook_id/stable_domain_id is what gives
// cross-launch correlation, not this.
typedef struct {
    uint64_t high;
    uint64_t low;
} hk_id_t;

// API status: whether the API call itself completed, never a statement
// about whether every hook became active. Per-hook outcome is
// hk_outcome_t (HookKitResults.h), read from a report or a hook result --
// never inferred from this alone.
typedef enum {
    HK_STATUS_OK = 0,
    HK_STATUS_INVALID_ARGUMENT,
    HK_STATUS_INVALID_STATE,
    HK_STATUS_OUT_OF_MEMORY,
    HK_STATUS_UNAVAILABLE,
    HK_STATUS_INTERNAL_ERROR,
    HK_STATUS_SHUTTING_DOWN,
} hk_status_t;

// Input strings/bytes passed through a _spec struct are deep-copied when
// added to a plan -- the caller's buffer need not outlive the call.
// Output views (returned from a report/snapshot/result) are owned by that
// object and stay valid only until it is released.
typedef struct {
    const char *data;
    size_t length;
} hk_string_view_t;

typedef struct {
    const uint8_t *data;
    size_t size;
} hk_bytes_view_t;

#ifdef __cplusplus
}
#endif

#endif // HOOKKIT_BASE_H
