# Shadow Route Feasibility — Initial (Modeled) Pass

Generated 2026-08-22T16:43:46Z from manifest `0.2.0-milestone2-partial-decomposition` (511 targets).

**Historical model, not current routing evidence**: this retained M2 baseline classifies each target against HookKit 2.x (see `docs/3.0/ENGINE_CONTRACT.md`), not current HK3 descriptors or a real router decision. Reported under a single lane-agnostic `"all"` entry (schema `shadow-route-report.schema.json`) rather than 4 duplicated copies of the same data — no real per-lane provider-availability data exists yet (Milestone 10) to make rootful-legacy/rootful-modern/rootless/roothide actually differ.

## Disposition

| Disposition | Count |
|---|---|
| needs_platform_decision | 114 |
| routable | 397 |

## Routable targets, by modeled engine

| Engine | Count |
|---|---|
| import-rebind | 81 |
| objc-message | 316 |

## Summary

- `dobby_required`: **False** — nothing extracted so far demonstrates a need only Dobby satisfies (see the classifier's own reasoning for private-address targets).
- `gum_required`: **False** — same reasoning, no evidence yet either way.
- `unclassified_mandatory_targets`: **113** — every one of these is a `needs_platform_decision` target resolved via runtime private-symbol lookup, not a gap in the classifier's coverage of target *shapes*. See each target's `blocking_reason`.

## Caveats (read before treating this as an ABI-freeze input)

- Concrete children are associated with 21/22 Shadow install-unit records in this manifest; the aggregate unit rows are retained as provenance alongside their source-extracted children.
- "Routable" here means "a HookKit 2.x engine already does this today", not "a certified HK3 engine has been proven to do this". Do not cite this retained initial report alone as satisfying the ABI-freeze gate in spec section 18.5.
