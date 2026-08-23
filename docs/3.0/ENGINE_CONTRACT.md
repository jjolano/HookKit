# HookKit 3.0 — Internal Engine Contract (draft)

Status: **draft for Milestone 1**. Governs every engine under `Sources/Engines/`
(ObjC, Rebind, NativeInline, Memory, Swift, Providers) from Milestone 6
onward. This is an internal contract — none of it is public API; see
`PUBLIC_C_ABI.md` for what callers see.

## Why a versioned C vtable, not per-engine bespoke code

The router (`HKRouter.c`, Milestone 4) needs to rank and drive arbitrary
engines uniformly, and grouped operations need to stay possible so native
batching and one-pass image traversal aren't lost when the 2.x code
(`nativeBatch`, fishhook's one-image-walk rebind batching) gets rebuilt on
top of this contract. A versioned vtable is the minimum that gets both.

```c
typedef struct hk_engine_vtable {
    uint32_t abi_version;
    uint32_t struct_size;

    const char *engine_id;

    hk_engine_capabilities_t (*describe)(void);
    hk_engine_discovery_t (*discover)(hk_engine_context_t *context);

    hk_engine_analysis_t (*analyze_operation)(
        hk_engine_context_t *context,
        const hk_normalized_request_t *request);

    hk_engine_attempt_t (*prepare_group)(
        hk_engine_context_t *context,
        hk_prepared_operation_t *const *operations, size_t operation_count,
        hk_artifact_builder_t *artifacts);

    hk_engine_attempt_t (*revalidate_group)(
        hk_engine_context_t *context,
        hk_prepared_operation_t *const *operations, size_t operation_count);

    hk_engine_attempt_t (*commit_group)(
        hk_engine_context_t *context,
        hk_prepared_operation_t *const *operations, size_t operation_count,
        hk_artifact_builder_t *artifacts);

    hk_engine_attempt_t (*verify_group)(
        hk_engine_context_t *context,
        hk_prepared_operation_t *const *operations, size_t operation_count,
        hk_artifact_builder_t *artifacts);

    hk_engine_attempt_t (*compensate_group)(
        hk_engine_context_t *context,
        hk_prepared_operation_t *const *operations, size_t operation_count,
        hk_artifact_builder_t *artifacts);

    hk_engine_attempt_t (*inspect_continuation)(
        hk_engine_context_t *context,
        const hk_prepared_operation_t *operation,
        hk_continuation_info_t *out_info);
} hk_engine_vtable_t;
```

Every entry point takes a *group* (`operations`, `operation_count`), even
when the caller only has one hook — a single-element group is the common
case, not a special one. This is what preserves "one image walk per rebind
wave" as an invariant (spec section 13.2) instead of it degrading back to
`O(hooks × images)` once the code goes through a generic interface.

## Descriptor contents

`describe()` returns a static, side-effect-free capability declaration:
target kinds handled; required resolver capabilities; achieved reachability
vector; supported image scopes; architectures; minimum OS; supported install
contexts (`HK_INSTALL_CONTEXT_*`); supported original requirements;
possible continuation kinds; declared preparation/commit effect upper
bounds; whether effects are fully inspectable; whether provider activation
or image loading may occur; whether callback registration or thread
creation may occur; whether dynamic executable memory may occur; whether
native grouping/batching exists; whether commit supports conditional
mutation; revalidation method; verification method; mechanical
reversibility; safe compensation window; certification state; cost class.

The router (`HKRouter.c`) reads this once per candidate per route decision —
it is never allowed to call into engine state to answer these questions,
which is exactly what keeps `describe()` callable during analysis without
violating the side-effect-free rule in `ARCHITECTURE.md` §Non-negotiable
invariants #1.

Current implementation detail: `architectures` and
`certified_architectures` are bitmasks. A production route requires both to
contain the running slice; the private test registration is the only bypass.
The current iOS check uses the binary's deployment-target lower bound, which
is enough for the present iOS 15 package-floor claims and fails closed for a
future engine claiming a higher floor. Add a runtime OS probe only when that
future route is needed.

## Discovery

`discover()` may use compile-time built-in availability, `dlopen_preflight`,
checking an already-loaded image, or export-probing an already-loaded
provider. It may never `dlopen`, run a constructor, initialize a provider,
or register a callback. This is the same boundary Milestone 10's provider
evidence records (`hookkit-provider-evidence.schema.json`,
`discovery_method` field) exist to make auditable per provider version.

## Analysis

`analyze_operation()` returns: whether the route could satisfy the request;
required preparation effects; required commit effects; expected
continuation kind; achieved reach; missing preferred reach; required
install context; resolver assumptions; a deterministic route rank.
No persistent allocation outside the plan. This is the function the router
calls per candidate per hook during `hk_plan_analyze()` — see
`ARCHITECTURE.md` invariant #1 for what it may never do.

## Preparation

`prepare_group()` activates providers only when the request allows it,
captures exact preconditions, creates only request-permitted continuation
artifacts, reserves ownership, creates prepared artifacts, records actual
effects, and fails the operation if actual effects exceed what `describe()`
declared. It never mutates a requested target — see invariant #2.

## Commit

`commit_group()` revalidates every mandatory group member before the first
write, revalidates each operation immediately before its own mutating
primitive, publishes originals before activation, records the exact
`hk_mutation_state_t`, and never falls back to another route after
`HK_MUTATION_PARTIAL` or `HK_MUTATION_UNKNOWN` — only after
`HK_MUTATION_NONE`. Compensation (`compensate_group()`) is only attempted
inside a certified commit window, and only for members the engine can prove
mechanically reversible.

Note on the Objective-C engine specifically (spec section 13.1): the public
Objective-C runtime has no compare-and-exchange method replacement, so this
engine is certified only for early/serialized install contexts, and its
residual race is reported honestly rather than papered over — `describe()`
for this engine will declare a weaker revalidation method than, say, the
rebind engine's atomic slot CAS.

## Engine certification

Only certified engine modes participate in automatic routing (the private
test SPI can enable uncertified modes for testing, but that SPI never ships
in public production headers). Certification requires host conformance,
device conformance for every claimed architecture/lane, continuation
classification, effect classification, verification behavior, mutation-state
accuracy, ownership interaction tests, and — for provider-backed engines —
an evidence record per `hookkit-provider-evidence.schema.json` with
`certification_status: "device_certified"`.

**What's already true of the 2.x code this maps onto** (informs which
engines are closest to certifiable once ported):

- Rebind (fishhook-backed): already batches into one image walk
  (`72a855f`, "fishhook: batch rebinds in one image walk"), already handles
  chained fixups and GOT boundary pages (`98893a8`). Closest to a
  drop-in conformance pass.
- Native inline: as of `3e6bbdb`, already has per-operation atomicity
  (single 4-byte `B` via inbound thunk when in range), a process-wide write
  lock, and honest degradation to the non-atomic 16-byte form when out of
  range — i.e. it already reports what spec section 13.4/13.5 calls
  `mutation_state` accuracy, just not yet through this vtable shape.
- Swift vtable: already single-copy-atomic (`hk_native_patch_pointer`,
  also `3e6bbdb`), already does PAC pre-write self-check. Conceptually the
  simplest engine to certify — no code patching, no continuation, `describe()`
  reduces to a short, mostly-`false` capability declaration.
- Objective-C: works today but has the CAS gap noted above — certification
  will need to state that gap in the descriptor rather than resolve it,
  since the underlying runtime genuinely doesn't offer atomic IMP swap.
