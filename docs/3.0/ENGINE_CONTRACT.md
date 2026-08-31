# HookKit 3.0 — Engine Contract

The executable contract lives in `src/core/HKEngineInternal.h` and is
enforced by `src/core/HKPlan.c`.

- Analysis is read-only.
- Preparation may reserve resources but never mutates the requested target.
  Every successful preparation has exactly one `release_prepared` call.
- Commit reports the real mutation state: `HK_MUTATION_NONE`, `COMPLETE`,
  `PARTIAL`, or `UNKNOWN`.
- Another route may be tried only after `HK_MUTATION_NONE`. `PARTIAL` and
  `UNKNOWN` are terminal.
- Engines declare target kinds, reachability, installation context, effects,
  and certified architectures. The planner rejects a candidate that cannot
  satisfy the request.

Automatic routing selects one eligible engine during analysis and commits that
selection. A caller that implements higher-level retry policy must use the
per-hook mutation result, never a generic status code, as its safety gate.
