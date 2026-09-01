// HookKit 3.0 -- runtime handle, executor/diagnostic callback types,
// install context, and runtime-level functions (create/shutdown/release/
// drain_pending). See docs/3.0/PUBLIC_C_ABI.md ("Runtime configuration and
// threading") and docs/3.0/THREADING_AND_INSTALL_CONTEXT.md (pending).

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

// Reserved/no-op callback types retained for HookKit 3.0 ABI compatibility.
typedef void (*hk_task_fn)(void *task_context);

typedef bool (*hk_executor_submit_fn)(
    void *executor_context,
    hk_task_fn task,
    void *task_context);

typedef void (*hk_diagnostic_callback_fn)(
    void *diagnostic_context,
    hk_string_view_t message);

// Called once for each selectable backend group discoverable on a runtime.
// Engines that share a group (for example every built-in function/memory
// engine, which reports "HookKit") are collapsed to a single call. `backend_id`
// is the group token, suitable as-is for
// hk_runtime_create_with_backend_override(); `display_name` is its
// human-readable label (falls back to the token). Both strings are borrowed
// from the runtime and valid only for the duration of the call.
// Return false to stop enumeration early; that is still a successful call.
typedef bool (*hk_backend_enumerator_fn)(
    void *context,
    hk_string_view_t backend_id,
    hk_string_view_t display_name);

typedef struct {
    HK_STRUCT_HEADER;

    // Reserved/no-op in HookKit 3.0; the runtime stores these fields but
    // never invokes either callback.
    // ponytail: these four fields cannot be removed before a major ABI break.
    hk_executor_submit_fn submit;
    void *executor_context;

    hk_diagnostic_callback_fn diagnostic_callback;
    void *diagnostic_context;

    hk_install_context_t install_context;
} hk_runtime_config_t;

hk_status_t hk_runtime_create(
    const hk_runtime_config_t *config,
    hk_runtime_t **out_runtime);

// Creates a runtime with a strict per-runtime backend override. A non-NULL,
// comma/space-separated `backend_ids` list leaves only the named
// function/memory engines, plus the built-in ObjC engine, eligible.
// An empty or all-invalid list leaves no function/memory route; NULL uses
// normal automatic routing. IDs are returned by hk_runtime_enumerate_backends().
hk_status_t hk_runtime_create_with_backend_override(
    const hk_runtime_config_t *config,
    const char *backend_ids,
    hk_runtime_t **out_runtime);

// Process-wide shared singleton — lazy, HK_INSTALL_CONTEXT_EARLY_PROCESS,
// pthread_once memo like HKRuntime.c dlopen_preflight cache. Cuts per-hook
// calloc+HKIDs.c ID cost. Keep per-TU statics for compat; this is the
// preferred path for new code and for the hookkit Logos generator.
// ponytail: one pthread_once + one calloc per process, not per-TU
hk_runtime_t *hk_shared_runtime(void);

// Does not generically unhook active targets -- installed hooks and
// original slots that live replacements still use outlive this call.
// Releasing is a wrapper-lifetime operation, not an uninstall.
void hk_runtime_shutdown(hk_runtime_t *runtime);
void hk_runtime_release(hk_runtime_t *runtime);

hk_id_t hk_runtime_owner_id(const hk_runtime_t *runtime);

// Enumerates backend engines that are discoverable and supported by this
// runtime's platform, in routing order. Discovery never activates a provider
// or mutates a target.
hk_status_t hk_runtime_enumerate_backends(
    hk_runtime_t *runtime,
    hk_backend_enumerator_fn enumerator,
    void *context);

// No internal worker thread is created (docs/3.0/ARCHITECTURE.md invariant #7).
// Deferred work remains queued until the caller invokes this function.
// Applies queued late-image delta work (Milestone 12) and returns a report of
// what was applied.
hk_status_t hk_runtime_drain_pending(
    hk_runtime_t *runtime,
    hk_report_t **out_report);

void hk_report_release(hk_report_t *report);

#ifdef __cplusplus
}
#endif

#endif // HOOKKIT_RUNTIME_H
