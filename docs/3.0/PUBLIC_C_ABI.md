# HookKit 3.0 — Public C ABI

Status: **implemented**. The normative declarations live under
`include/HookKit/`, with compile tests in C, Objective-C, C++, and
Objective-C++ under `tests/host/`. See `IMPLEMENTATION_STATUS.md`.

Companion docs: `ARCHITECTURE.md` (invariants this ABI exists to encode),
`REACHABILITY_VECTOR.md`, `ORIGINAL_AND_CONTINUATION_MODEL.md`,
`ARTIFACT_MANIFEST.md`, and `ENGINE_CONTRACT.md` (pending).

## Naming and versioning

The public API uses the `hk_` prefix exclusively. Every extensible public
structure opens with:

```c
uint32_t struct_size;
uint32_t struct_version;
```

```c
#define HK_ABI_VERSION_3_0 0x00030000u
```

Unknown trailing fields are ignored when `struct_size` exceeds what the
current implementation expects. A structure smaller than the minimum size
needed for its required fields is rejected.

## Opaque handles

```c
typedef struct hk_runtime hk_runtime_t;
typedef struct hk_plan hk_plan_t;
typedef struct hk_domain hk_domain_t;
typedef struct hk_hook hk_hook_t;
typedef struct hk_installed_hook hk_installed_hook_t;
typedef struct hk_original_slot hk_original_slot_t;
typedef struct hk_report hk_report_t;
typedef struct hk_artifact_snapshot hk_artifact_snapshot_t;
```

## Stable IDs

```c
typedef struct {
    uint64_t high;
    uint64_t low;
} hk_id_t;
```

One process-instance nonce plus an atomic/locked monotonic counter. Every
runtime, plan, request, installed hook, domain, report, and artifact gets
one. IDs are stable for the process lifetime only — no cross-process
identity is claimed; a caller-provided stable hook name is what gives
cross-launch correlation.

## API status

```c
typedef enum {
    HK_STATUS_OK = 0,
    HK_STATUS_INVALID_ARGUMENT,
    HK_STATUS_INVALID_STATE,
    HK_STATUS_OUT_OF_MEMORY,
    HK_STATUS_UNAVAILABLE,
    HK_STATUS_INTERNAL_ERROR,
    HK_STATUS_SHUTTING_DOWN,
} hk_status_t;
```

This says whether the *API call* completed — not whether every hook became
active. Per-hook outcome is `hk_outcome_t` (below), read from the report or
the hook result.

## String and byte views

```c
typedef struct {
    const char *data;
    size_t length;
} hk_string_view_t;

typedef struct {
    const uint8_t *data;
    size_t size;
} hk_bytes_view_t;
```

Input strings/bytes are deep-copied when added to a plan. Output views are
owned by their report or snapshot and stay valid until that object is
released.

## Original requirement

```c
typedef enum {
    HK_ORIGINAL_NONE = 0,
    HK_ORIGINAL_DIRECT_PREDECESSOR,
    HK_ORIGINAL_CALLABLE_CONTINUATION,
} hk_original_requirement_t;
```

`NONE`: replacement never calls prior behavior. `DIRECT_PREDECESSOR`: an
existing predecessor is acceptable (untouched function body after
rebinding, previous IMP, previous Swift slot). `CALLABLE_CONTINUATION`: a
callable ABI-compatible path must execute the displaced behavior and
continue through the original body. These two are never interchangeable.

## Continuation policy

```c
typedef enum {
    HK_CONTINUATION_ANY = 0,
    HK_CONTINUATION_NO_DYNAMIC_EXECUTABLE_MEMORY,
    HK_CONTINUATION_FORBIDDEN,
} hk_continuation_policy_t;
```

`ANY`: direct, static, dynamic, or inspected provider-internal continuation
may be considered. `NO_DYNAMIC_EXECUTABLE_MEMORY`: direct or proven-static
only; anonymous/generated executable mappings forbidden. `FORBIDDEN`: no
relocated stub, static continuation slot, branch island, generated
trampoline, or provider-internal equivalent.

`original_requirement = HK_ORIGINAL_CALLABLE_CONTINUATION` with
`continuation_policy = HK_CONTINUATION_FORBIDDEN` is rejected as
contradictory before route analysis even runs.

## Continuation kind and mapping kind

```c
typedef enum {
    HK_CONTINUATION_KIND_NONE = 0,
    HK_CONTINUATION_KIND_DIRECT_PREDECESSOR,
    HK_CONTINUATION_KIND_STATIC,
    HK_CONTINUATION_KIND_DYNAMIC,
    HK_CONTINUATION_KIND_PROVIDER_INTERNAL,
    HK_CONTINUATION_KIND_UNKNOWN,
} hk_continuation_kind_t;

typedef enum {
    HK_MAPPING_NONE = 0,
    HK_MAPPING_IMAGE_TEXT,
    HK_MAPPING_IMAGE_DATA,
    HK_MAPPING_SHARED_CACHE,
    HK_MAPPING_STATIC_HOOKKIT_SECTION,
    HK_MAPPING_ANONYMOUS,
    HK_MAPPING_PROVIDER_OWNED,
    HK_MAPPING_UNKNOWN,
} hk_mapping_kind_t;
```

## Continuation information

```c
typedef struct {
    uint32_t struct_size;
    uint32_t struct_version;

    hk_continuation_kind_t kind;

    uintptr_t address;
    uintptr_t jump_back_destination;

    hk_id_t mapping_id;
    hk_mapping_kind_t mapping_kind;
    uintptr_t mapping_base;
    size_t mapping_size;
    uint32_t mapping_protection;

    bool executable_memory_allocated;
    uint32_t relocated_instruction_count;

    bool readable;
    bool mechanically_reversible;
    bool safe_to_reverse_after_activation;
    bool fully_inspected;
} hk_continuation_info_t;
```

Every prepared or active function-entry hook returns this. See
`TERMINAL_INLINE.md` (pending, Milestone 7) for the strict-terminal zeroed
form this struct must take.

## Reachability capability vector

```c
typedef uint64_t hk_reachability_t;

enum {
    HK_REACH_EXISTING_IMPORTS      = 1ull << 0,
    HK_REACH_FUTURE_IMPORTS        = 1ull << 1,
    HK_REACH_ENTRYPOINT            = 1ull << 2,
    HK_REACH_DLSYM_POINTERS        = 1ull << 3,
    HK_REACH_SAVED_POINTERS        = 1ull << 4,
    HK_REACH_EXACT_IMAGE_SCOPE     = 1ull << 5,
    HK_REACH_PRIVATE_ADDRESS       = 1ull << 6,
    HK_REACH_CURRENT_IMAGES        = 1ull << 7,
    HK_REACH_FUTURE_IMAGES         = 1ull << 8,
    HK_REACH_OBJC_DISPATCH         = 1ull << 9,
    HK_REACH_SWIFT_VTABLE_DISPATCH = 1ull << 10,
    HK_REACH_EXACT_MEMORY          = 1ull << 11,
};
```

Each request supplies `required_reach` and `preferred_reach`; each result
reports `achieved_reach` and `unmet_preferred_reach`. All required bits must
be achieved — no exceptions, no near-enough. Full honesty rules (what may
never be claimed) are in `REACHABILITY_VECTOR.md` (pending); the short
version: never claim entrypoint or saved-pointer reach from import
rebinding, never claim future-image reach without an active future-image
mechanism, never claim exact-image-scope when the engine actually mutates a
broader set.

## Process effects and request constraints

```c
typedef uint64_t hk_effects_t;

enum {
    HK_EFFECT_TARGET_TEXT_MUTATION       = 1ull << 0,
    HK_EFFECT_IMPORT_MUTATION            = 1ull << 1,
    HK_EFFECT_OBJC_METADATA_MUTATION     = 1ull << 2,
    HK_EFFECT_SWIFT_VTABLE_MUTATION      = 1ull << 3,
    HK_EFFECT_MEMORY_MUTATION            = 1ull << 4,

    HK_EFFECT_EXECUTABLE_ALLOCATION      = 1ull << 5,
    HK_EFFECT_STATIC_CONTINUATION_USE    = 1ull << 6,
    HK_EFFECT_PROVIDER_ACTIVATION        = 1ull << 7,
    HK_EFFECT_PROVIDER_IMAGE_LOAD        = 1ull << 8,
    HK_EFFECT_IMAGE_LOAD                 = 1ull << 9,
    HK_EFFECT_CALLBACK_REGISTRATION      = 1ull << 10,
    HK_EFFECT_THREAD_CREATION            = 1ull << 11,
    HK_EFFECT_PRIVATE_SYMBOL_SCAN        = 1ull << 12,
    HK_EFFECT_FILE_MAPPING               = 1ull << 13,
    HK_EFFECT_MEMORY_PROTECTION_CHANGE   = 1ull << 14,

    HK_EFFECT_UNKNOWN_PROCESS_MUTATION   = 1ull << 63,
};
```

Every operation reports four masks: `declared_prepare_effects`,
`observed_prepare_effects`, `declared_commit_effects`,
`observed_commit_effects`. An observed effect outside the declared upper
bound is an engine contract violation and fails the operation — this is
what keeps §4.1/§4.2 (analysis/preparation purity) enforceable rather than
just documented.

```c
typedef uint64_t hk_constraints_t;

enum {
    HK_FORBID_TARGET_TEXT_PATCH           = 1ull << 0,
    HK_FORBID_IMPORT_SLOT_PATCH           = 1ull << 1,
    HK_FORBID_OBJC_METADATA_CHANGE        = 1ull << 2,
    HK_FORBID_SWIFT_VTABLE_CHANGE         = 1ull << 3,

    HK_FORBID_DYNAMIC_EXECUTABLE_MEMORY   = 1ull << 4,
    HK_FORBID_STATIC_CONTINUATION         = 1ull << 5,
    HK_FORBID_PRIVATE_SYMBOL_SCAN         = 1ull << 6,

    HK_FORBID_PROVIDER_ACTIVATION         = 1ull << 7,
    HK_FORBID_PROVIDER_IMAGE_LOAD         = 1ull << 8,
    HK_FORBID_CALLBACK_REGISTRATION       = 1ull << 9,
    HK_FORBID_LATE_IMAGE_CALLBACK         = 1ull << 10,
    HK_FORBID_THREAD_CREATION             = 1ull << 11,

    HK_FORBID_UNKNOWN_PREPARATION_EFFECTS = 1ull << 12,
    HK_FORBID_UNKNOWN_COMMIT_EFFECTS      = 1ull << 13,
};
```

Continuation policy and generic constraints are both enforced, independently:
`HK_CONTINUATION_FORBIDDEN` rejects continuation machinery even inside
prelinked image text; `HK_FORBID_DYNAMIC_EXECUTABLE_MEMORY` rejects every
dynamic executable mapping, including branch islands and page-remap
fallbacks.

## Target kinds and specs

```c
typedef enum {
    HK_TARGET_FUNCTION_SYMBOL = 0,
    HK_TARGET_FUNCTION_ADDRESS,
    HK_TARGET_OBJC_METHOD,
    HK_TARGET_MEMORY_PATCH,
    HK_TARGET_SWIFT_VTABLE,
} hk_target_kind_t;

typedef enum {
    HK_IMAGE_ANY_LOADED = 0,
    HK_IMAGE_MAIN_EXECUTABLE,
    HK_IMAGE_EXACT_PATH,
    HK_IMAGE_EXACT_UUID,
    HK_IMAGE_EXACT_HEADER,
    HK_IMAGE_EXPLICIT_SET,
} hk_image_selector_kind_t;
```

Three separate scopes, never conflated: the defining-image selector, the
caller/import-image scope, and the excluded-image set. If an engine excludes
the replacement-defining image to prevent recursion, `achieved_reach` must
reflect that exclusion — the exclusion is never silent.

**Symbol target**: symbol string; name convention (C source name, exact
Mach-O name, C++ mangled, Swift mangled); exported/private visibility
requirement; defining-image selector; caller/import-image scope; alias
policy; whether an interior address is permitted. C lookup tries the supplied
spelling, then its linker-prefixed form; use `HK_SYMBOL_NAME_MACHO_EXACT` for
one exact Mach-O spelling.

**Address target**: target address; expected image identity; optional
expected image UUID; optional expected initial bytes; whether address
normalization may strip PAC or Thumb state. An address resolving outside an
explicitly expected image identity is rejected, never silently accepted.

**Objective-C target**:

```c
typedef enum {
    HK_OBJC_INSTANCE_METHOD = 0,
    HK_OBJC_CLASS_METHOD,
} hk_objc_method_kind_t;

typedef enum {
    HK_OBJC_LOCAL_METHOD_ONLY = 0,
    HK_OBJC_ALLOW_INHERITED_OVERRIDE,
} hk_objc_inheritance_policy_t;
```

`Class` pointer or class name; selector or selector name; method kind;
inheritance policy; required-now or deferred availability. Class-method-ness
is always stated explicitly by the request, never inferred by probing the
instance class first.

**Memory target**: target address or image-relative location; replacement
bytes; expected bytes; expected-byte mask; size; expected image identity
where applicable; whether the target is code or data. Expected bytes are
required so commit can revalidate before writing.

**Swift target** (`HookKitSwift.h`): class/metadata pointer; class name
where resolvable; exact mangled method name; demangled substring lookup;
declaration-order slot index; required uniqueness; documented scope
limitations.

## Availability and operation role

```c
typedef enum {
    HK_AVAILABILITY_REQUIRED_NOW = 0,
    HK_AVAILABILITY_OPTIONAL_IF_PRESENT,
    HK_AVAILABILITY_DEFER_UNTIL_AVAILABLE,
} hk_availability_t;

typedef enum {
    HK_OPERATION_OPTIONAL = 0,
    HK_OPERATION_MANDATORY,
} hk_operation_role_t;
```

## Domain specification

```c
typedef enum {
    HK_COMPENSATION_NONE = 0,
    HK_COMPENSATION_BEST_EFFORT_REVERSIBLE_MEMBERS,
    HK_COMPENSATION_REQUIRE_REVERSIBLE_PREFIX,
} hk_compensation_policy_t;

typedef struct {
    uint32_t struct_size;
    uint32_t struct_version;

    const char *stable_domain_id;
    uint32_t domain_order;

    bool require_all_mandatory_prepared;
    bool prefer_reversible_before_irreversible;

    hk_compensation_policy_t compensation_policy;
} hk_domain_spec_t;
```

Full domain/commit-ordering semantics: `COMMIT_DOMAINS.md` (pending,
Milestone 4).

## Hook specification

```c
typedef struct {
    uint32_t struct_size;
    uint32_t struct_version;

    const char *stable_hook_id;

    hk_target_kind_t target_kind;
    hk_target_spec_t target;

    void *replacement;

    hk_reachability_t required_reach;
    hk_reachability_t preferred_reach;

    hk_original_requirement_t original_requirement;
    hk_continuation_policy_t continuation_policy;

    hk_constraints_t constraints;
    hk_availability_t availability;

    hk_operation_role_t role;
    hk_domain_t *domain;

    uint32_t commit_order;

    const hk_hook_t *const *commit_after;
    size_t commit_after_count;
} hk_hook_spec_t;
```

All request-owned data is deep-copied when the hook is added to the plan.
Stable IDs and target identities are each validated unique within the plan at
add time. To chain a compatible target, install it through separately
committed plans: a successor prepared before its predecessor commits fails
safely rather than overwriting a stale target state.

## Plan state

```c
typedef enum {
    HK_PLAN_DRAFT = 0,
    HK_PLAN_ANALYZED,
    HK_PLAN_PREPARING,
    HK_PLAN_PREPARED,
    HK_PLAN_COMMITTING,
    HK_PLAN_COMMITTED,
    HK_PLAN_PARTIAL,
    HK_PLAN_FAILED,
    HK_PLAN_INVALIDATED,
    HK_PLAN_DISCARDED,
} hk_plan_state_t;
```

Only `HK_PLAN_DRAFT` accepts new domains or hooks. Changing a request
invalidates prior analysis/preparation — request objects are meant to be
treated as immutable rather than mutated in place through setters.

## Hook outcome and mutation state

```c
typedef enum {
    HK_OUTCOME_UNANALYZED = 0,
    HK_OUTCOME_NO_ROUTE,
    HK_OUTCOME_ANALYZED,
    HK_OUTCOME_PREPARED,
    HK_OUTCOME_PENDING,
    HK_OUTCOME_ACTIVE,
    HK_OUTCOME_ALREADY_ACTIVE,
    HK_OUTCOME_STALE_PLAN,
    HK_OUTCOME_CONFLICT,
    HK_OUTCOME_FAILED_SAFE,
    HK_OUTCOME_FAILED_PARTIAL,
    HK_OUTCOME_FAILED_UNKNOWN,
    HK_OUTCOME_COMPENSATED,
    HK_OUTCOME_INVALIDATED,
} hk_outcome_t;

typedef enum {
    HK_MUTATION_NONE = 0,
    HK_MUTATION_COMPLETE,
    HK_MUTATION_PARTIAL,
    HK_MUTATION_UNKNOWN,
} hk_mutation_state_t;
```

## Hook result

```c
typedef struct {
    uint32_t struct_size;
    uint32_t struct_version;

    hk_id_t runtime_owner_id;
    hk_id_t plan_id;
    hk_id_t request_id;
    hk_id_t installed_id;

    uint64_t image_generation;
    uint64_t installed_generation;

    hk_outcome_t outcome;
    hk_mutation_state_t mutation;

    hk_reachability_t achieved_reach;
    hk_reachability_t unmet_preferred_reach;

    hk_effects_t declared_prepare_effects;
    hk_effects_t observed_prepare_effects;
    hk_effects_t declared_commit_effects;
    hk_effects_t observed_commit_effects;

    hk_continuation_info_t continuation;

    bool original_available;
    bool verified;
    bool retryable;
    bool currently_valid;

    uint32_t matched_locations;
    uint32_t modified_locations;

    hk_string_view_t diagnostic_engine_id;
    hk_string_view_t error_domain;
    int64_t error_code;
    hk_string_view_t error_message;

    size_t artifact_count;
} hk_hook_result_t;
```

Never an aggregate-only result — every hook in a plan gets its own
`hk_hook_result_t`, even inside a batch.

## Runtime configuration and threading

The default runtime creates no thread. An external serial executor may be
supplied:

```c
typedef void (*hk_task_fn)(void *task_context);

typedef bool (*hk_executor_submit_fn)(
    void *executor_context,
    hk_task_fn task,
    void *task_context);

typedef struct {
    uint32_t struct_size;
    uint32_t struct_version;

    hk_executor_submit_fn submit;
    void *executor_context;

    hk_diagnostic_callback_fn diagnostic_callback;
    void *diagnostic_context;

    hk_install_context_t install_context;
} hk_runtime_config_t;
```

The current deferred ObjC lifecycle is caller-driven: once its host observes
the relevant class/load event, it calls `hk_runtime_drain_pending()`. No
current engine claims future-image reach or registers an automatic callback,
so `submit` is not used for automatic installation yet.

```c
HK_INSTALL_CONTEXT_EARLY_PROCESS
HK_INSTALL_CONTEXT_MAIN_THREAD_SERIALIZED
HK_INSTALL_CONTEXT_RUNTIME_SERIALIZED
HK_INSTALL_CONTEXT_ARBITRARY_RUNTIME
```

Engines declare which install contexts they support; see
`THREADING_AND_INSTALL_CONTEXT.md` (pending).

## Core functions

```c
hk_status_t hk_runtime_create(const hk_runtime_config_t *config, hk_runtime_t **out_runtime);
void hk_runtime_shutdown(hk_runtime_t *runtime);
void hk_runtime_release(hk_runtime_t *runtime);
hk_id_t hk_runtime_owner_id(const hk_runtime_t *runtime);
hk_status_t hk_runtime_drain_pending(hk_runtime_t *runtime, hk_report_t **out_report);

hk_status_t hk_plan_create(hk_runtime_t *runtime, const hk_plan_config_t *config, hk_plan_t **out_plan);
void hk_plan_release(hk_plan_t *plan);
hk_status_t hk_plan_define_domain(hk_plan_t *plan, const hk_domain_spec_t *spec, hk_domain_t **out_domain);
hk_status_t hk_plan_add_hook(hk_plan_t *plan, const hk_hook_spec_t *spec, hk_hook_t **out_hook);
hk_status_t hk_plan_analyze(hk_plan_t *plan, hk_report_t **out_report);
hk_status_t hk_plan_prepare(hk_plan_t *plan, hk_report_t **out_report);
hk_status_t hk_plan_commit(hk_plan_t *plan, hk_report_t **out_report);
hk_plan_state_t hk_plan_state(const hk_plan_t *plan);

hk_status_t hk_hook_copy_result(const hk_hook_t *hook, hk_hook_result_t *out_result);
const hk_original_slot_t *hk_hook_original_slot(const hk_hook_t *hook);
void *hk_original_slot_load(const hk_original_slot_t *slot);
const hk_installed_hook_t *hk_hook_installed_handle(const hk_hook_t *hook);
hk_status_t hk_installed_hook_copy_result(const hk_installed_hook_t *installed, hk_hook_result_t *out_result);

void hk_report_release(hk_report_t *report);
```

Active installation data and original slots that live replacements still
use must survive runtime wrapper release — process-lifetime internal
installation records are retained as needed. Releasing a runtime does not
generically unhook active targets; that is documented behavior, not an
oversight.

## Artifacts and structs not yet drafted here

`hk_artifact_kind_t`, `hk_artifact_state_t`, the artifact record layout, and
the snapshot functions (`hk_report_copy_artifacts`,
`hk_runtime_copy_artifacts`, `hk_copy_process_artifacts`, ...) are specified
in `ARTIFACT_MANIFEST.md` (pending) and
`metadata/schemas/hookkit-artifact.schema.json` rather than duplicated here.
The schema is the serialized artifact contract; the public C declaration lives
in `include/HookKit/HookKitArtifacts.h`.
