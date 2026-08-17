# Shadow Route Feasibility — Initial (Modeled) Pass

Generated 2026-08-17T07:29:28Z from manifest `0.2.0-milestone2-partial-decomposition` (186 targets).

**Modeled, not measured**: HookKit 3's engines don't exist as code yet. This classifies each target against what's already known and working in HookKit 2.x (see `docs/3.0/ENGINE_CONTRACT.md`), not a real router decision. Reported under a single lane-agnostic `"all"` entry (schema `shadow-route-report.schema.json`) rather than 4 duplicated copies of the same data — no real per-lane provider-availability data exists yet (Milestone 10) to make rootful-legacy/rootful-modern/rootless/roothide actually differ.

## Disposition

| Disposition | Count |
|---|---|
| needs_platform_decision | 114 |
| routable | 72 |

## Routable targets, by modeled engine

| Engine | Count |
|---|---|
| import-rebind | 63 |
| objc-message | 9 |

## Summary

- `dobby_required`: **False** — nothing extracted so far demonstrates a need only Dobby satisfies (see the classifier's own reasoning for private-address targets).
- `gum_required`: **False** — same reasoning, no evidence yet either way.
- `unclassified_mandatory_targets`: **114** — every one of these is a `needs_platform_decision` target resolved via runtime private-symbol lookup, not a gap in the classifier's coverage of target *shapes*. See each target's `blocking_reason`.

## Caveats (read before treating this as an ABI-freeze input)

- Manifest coverage is partial: 13/22 Shadow install units are decomposed into individual targets as of this report; the rest are unit-level only (see `docs/3.0/IMPLEMENTATION_STATUS.md`, Milestone 2).
- "Routable" here means "a HookKit 2.x engine already does this today", not "a certified HK3 engine has been proven to do this" — that's Milestone 6+'s job. Do not cite this report alone as satisfying the ABI-freeze gate in spec section 18.5.
