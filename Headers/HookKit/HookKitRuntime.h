// HookKit 3.0 -- runtime handle, executor/diagnostic callback types,
// install context, and runtime-level functions (create/shutdown/release/
// drain_pending). See docs/3.0/PUBLIC_C_ABI.md ("Runtime configuration and
// threading") and docs/3.0/THREADING_AND_INSTALL_CONTEXT.md (pending).
//
// Two types referenced by the master spec's hk_runtime_config_t
// (section 6.29) were never actually defined in its text --
// hk_plan_config_t and hk_diagnostic_callback_fn. Real gaps, not
// oversights on this file's part: filled in here (hk_diagnostic_callback_fn)
// and in HookKitPlan.h (hk_plan_config_t), each noted where it happens.

#ifndef HOOKKIT_RUNTIME_H
#define HOOKKIT_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "HookKitBase.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hk_runtime hk_runtime_t;
typedef struct hk_report hk_report_t;
typedef struct hk_artifact_snapshot hk_artifact_snapshot_t;

// Where in the process lifecycle an engine is certified to install --
// engines declare which of these they support; the router refuses a
// candidate whose required context the runtime/caller cannot satisfy.
typedef enum {
    HK_INSTALL_CONTEXT_EARLY_PROCESS = 0,
    HK_INSTALL_CONTEXT_MAIN_THREAD_SERIALIZED,
    HK_INSTALL_CONTEXT_RUNTIME_SERIALIZED,
    HK_INSTALL_CONTEXT_ARBITRARY_RUNTIME,
} hk_install_context_t;

// No internal worker thread is created (docs/3.0/ARCHITECTURE.md invariant #7).
// Deferred work remains queued until the caller invokes hk_runtime_drain_pending().
// `submit` is retained for a future event source; no current engine claims
// future-image reach or registers an automatic image callback.
typedef void (*hk_task_fn)(void *task_context);

typedef bool (*hk_executor_submit_fn)(
    void *executor_context,
    hk_task_fn task,
    void *task_context);

// Real gap in the master spec's text, filled in here: a minimal
// string-message callback. No default logging exists (spec section 28.1)
// -- this is the only way diagnostics leave the runtime short of a full
// report/artifact query.
typedef void (*hk_diagnostic_callback_fn)(
    void *diagnostic_context,
    hk_string_view_t message);

typedef struct {
    HK_STRUCT_HEADER;

    hk_executor_submit_fn submit;   // reserved for a future event source
    void *executor_context;

    hk_diagnostic_callback_fn diagnostic_callback;  // NULL: no diagnostics emitted
    void *diagnostic_context;

    hk_install_context_t install_context;
} hk_runtime_config_t;

hk_status_t hk_runtime_create(
    const hk_runtime_config_t *config,
    hk_runtime_t **out_runtime);

// Does not generically unhook active targets -- installed hooks and
// original slots that live replacements still use outlive this call.
// Releasing is a wrapper-lifetime operation, not an uninstall.
void hk_runtime_shutdown(hk_runtime_t *runtime);
void hk_runtime_release(hk_runtime_t *runtime);

hk_id_t hk_runtime_owner_id(const hk_runtime_t *runtime);

// Applies queued late-image delta work (Milestone 12). A request requiring
// autonomous late application cannot claim that reach unless the runtime
// has an execution path -- an executor, or a caller who actually calls
// this -- capable of applying it. Returns a report of what was applied.
hk_status_t hk_runtime_drain_pending(
    hk_runtime_t *runtime,
    hk_report_t **out_report);

void hk_report_release(hk_report_t *report);

#ifdef __cplusplus
}
#endif

#endif // HOOKKIT_RUNTIME_H
