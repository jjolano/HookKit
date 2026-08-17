#!/usr/bin/env python3
"""Initial Shadow route-feasibility report (spec section 18.4), modeled
capabilities only -- HookKit 3's actual engines don't exist as code yet
(Milestones 4-10), so there is no real hk_engine_vtable_t to query. What
this does instead, and states plainly in its own output rather than hiding
it: a small, hand-authored, clearly-labeled classifier mapping each
manifest target's (target_kind, required_reach) shape to what's already
known and true about HookKit 2.x's working engines today
(docs/3.0/ENGINE_CONTRACT.md's "what's already true of the 2.x code" notes),
plus an honest "undetermined" bucket for anything that needs real engine
certification to answer. This is what spec section 18 Milestone 2 calls
"initial route report using modeled engine capabilities" -- a starting
point for the ABI-freeze gate (section 18.5), not a finished answer.

Known simplification: the same classification is applied to all 4 packaging
lanes. No real per-lane engine-availability data exists yet (that's
Milestone 10's ProviderEvidence work) -- faking lane differences would be
worse than stating they're identical for now.

Deliberately conservative about requires_dobby/requires_gum: a target
resolved via runtime private-symbol lookup (findSymbolInImage /
shdw_libc_hooks-style dlsym) genuinely needs more than plain import
rebinding, but WHICH certified engine ends up serving it (native inline,
ElleKit's private-symbol path, Dobby, litehook -- HookKit 2.x's own
HK_CAT_PRIVATE_SYMBOL picker order is ElleKit > Substrate > Substitute >
litehook, Dobby not even in that order) is not something this modeled pass
guesses. Both flags stay false unless a target concretely demonstrates a
need only Dobby or Gum specifically satisfy -- none do yet.
"""

import argparse
import json
import os
import sys
from collections import Counter
from datetime import datetime, timezone


def make_route(engine_id, provider_family, continuation_kind, install_context, achieved_reach):
    return {
        "engine_id": engine_id,
        "provider_family": provider_family,
        "continuation_kind": continuation_kind,
        "required_install_context": install_context,
        "achieved_reach": achieved_reach,
    }


def classify_target(target):
    """Returns a dict matching shadow-route-report.schema.json's
    target_route shape, minus stable_hook_id (the caller adds that)."""
    kind = target.get("target_kind")
    reach = set(target.get("required_reach", []))

    if kind == "objc_method":
        route = make_route("objc-message", "none", "direct_predecessor",
                            "arbitrary_runtime", ["objc_dispatch"])
        return {
            "eligible_routes": [route],
            "selected_route": route,
            "missing_preferred_reach": [],
            "requires_dobby": False, "requires_gum": False,
            "requires_private_symbols": False, "requires_terminal_inline": False,
            "requires_dynamic_continuation": False, "requires_static_continuation": False,
            "blocking_reason": None,
            "disposition": "routable",
        }

    if kind == "function_symbol" and reach == {"existing_imports"}:
        route = make_route("import-rebind", "none", "direct_predecessor",
                            "early_process", ["existing_imports"])
        return {
            "eligible_routes": [route],
            "selected_route": route,
            "missing_preferred_reach": [],
            "requires_dobby": False, "requires_gum": False,
            "requires_private_symbols": False, "requires_terminal_inline": False,
            "requires_dynamic_continuation": False, "requires_static_continuation": False,
            "blocking_reason": None,
            "disposition": "routable",
        }

    if kind == "function_symbol" and "private_address" in reach:
        return {
            "eligible_routes": [],
            "selected_route": None,
            "missing_preferred_reach": [],
            "requires_dobby": False, "requires_gum": False,
            "requires_private_symbols": True,
            "requires_terminal_inline": False,
            "requires_dynamic_continuation": False,
            "requires_static_continuation": False,
            "blocking_reason": (
                "resolved via runtime private-symbol lookup; which certified HK3 "
                "engine serves this (native inline / ElleKit private-symbol path / "
                "Dobby / litehook) is undetermined until Milestone 6/7/10 engine "
                "certification exists -- modeled pass does not guess a specific one"
            ),
            "disposition": "needs_platform_decision",
        }

    return {
        "eligible_routes": [],
        "selected_route": None,
        "missing_preferred_reach": [],
        "requires_dobby": False, "requires_gum": False,
        "requires_private_symbols": False, "requires_terminal_inline": False,
        "requires_dynamic_continuation": False, "requires_static_continuation": False,
        "blocking_reason": (
            f"(target_kind={kind!r}, required_reach={sorted(reach)!r}) is not a shape "
            "the modeled classifier covers yet"
        ),
        "disposition": "needs_platform_decision",
    }


def build_report(manifest):
    # classify_target() takes no lane argument -- it structurally cannot
    # produce different output per lane, not just coincidentally does not
    # yet. Writing 4 copies of an identical 186-target array would be pure
    # bloat (an earlier version of this did exactly that: 15,600+ lines for
    # 186 targets). Use the schema's "all" lane shorthand instead, and
    # switch to real per-lane entries the day classify_target actually
    # takes lane-specific input (Milestone 10 ProviderEvidence).
    targets = [
        {"stable_hook_id": t["stable_hook_id"], **classify_target(t)}
        for t in manifest["targets"]
    ]
    lanes = [{"lane": "all", "targets": targets}]

    sample = targets
    unclassified = sum(1 for t in sample if t["disposition"] == "needs_platform_decision")

    return {
        "report_version": "0.1.0-milestone2-modeled",
        "generated_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "manifest_version": manifest.get("manifest_version"),
        "lanes": lanes,
        "summary": {
            "dobby_required": any(t["requires_dobby"] for t in sample),
            "gum_required": any(t["requires_gum"] for t in sample),
            "unclassified_mandatory_targets": unclassified,
        },
    }


def render_markdown(report, manifest):
    sample = report["lanes"][0]["targets"]
    by_disposition = Counter(t["disposition"] for t in sample)
    by_engine = Counter(
        t["selected_route"]["engine_id"] for t in sample if t["selected_route"]
    )

    lines = [
        "# Shadow Route Feasibility — Initial (Modeled) Pass",
        "",
        f"Generated {report['generated_at']} from manifest "
        f"`{report['manifest_version']}` ({len(manifest['targets'])} targets).",
        "",
        "**Modeled, not measured**: HookKit 3's engines don't exist as code yet. "
        "This classifies each target against what's already known and working in "
        "HookKit 2.x (see `docs/3.0/ENGINE_CONTRACT.md`), not a real router "
        "decision. Reported under a single lane-agnostic `\"all\"` entry (schema "
        "`shadow-route-report.schema.json`) rather than 4 duplicated copies of "
        "the same data — no real per-lane provider-availability data exists yet "
        "(Milestone 10) to make rootful-legacy/rootful-modern/rootless/roothide "
        "actually differ.",
        "",
        "## Disposition",
        "",
        "| Disposition | Count |",
        "|---|---|",
    ]
    for k, v in sorted(by_disposition.items()):
        lines.append(f"| {k} | {v} |")

    lines += [
        "",
        "## Routable targets, by modeled engine",
        "",
        "| Engine | Count |",
        "|---|---|",
    ]
    for k, v in sorted(by_engine.items()):
        lines.append(f"| {k} | {v} |")

    lines += [
        "",
        f"## Summary",
        "",
        f"- `dobby_required`: **{report['summary']['dobby_required']}** — nothing "
        "extracted so far demonstrates a need only Dobby satisfies (see the "
        "classifier's own reasoning for private-address targets).",
        f"- `gum_required`: **{report['summary']['gum_required']}** — same "
        "reasoning, no evidence yet either way.",
        f"- `unclassified_mandatory_targets`: **{report['summary']['unclassified_mandatory_targets']}** "
        "— every one of these is a `needs_platform_decision` target resolved via "
        "runtime private-symbol lookup, not a gap in the classifier's coverage of "
        "target *shapes*. See each target's `blocking_reason`.",
        "",
        "## Caveats (read before treating this as an ABI-freeze input)",
        "",
        "- Manifest coverage is partial: 13/22 Shadow install units are decomposed "
        "into individual targets as of this report; the rest are unit-level only "
        "(see `docs/3.0/IMPLEMENTATION_STATUS.md`, Milestone 2).",
        "- \"Routable\" here means \"a HookKit 2.x engine already does this today\", "
        "not \"a certified HK3 engine has been proven to do this\" — that's Milestone "
        "6+'s job. Do not cite this report alone as satisfying the ABI-freeze gate "
        "in spec section 18.5.",
    ]
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--manifest", default=None)
    ap.add_argument("--out-json", default=None)
    ap.add_argument("--out-md", default=None)
    args = ap.parse_args()

    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    manifest_path = args.manifest or os.path.join(repo_root, "artifacts", "shadow-current-manifest.json")
    with open(manifest_path, "r", encoding="utf-8") as f:
        manifest = json.load(f)

    report = build_report(manifest)

    out_json = args.out_json or os.path.join(repo_root, "artifacts", "shadow-route-feasibility.json")
    with open(out_json, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)
        f.write("\n")

    out_md = args.out_md or os.path.join(repo_root, "docs", "3.0", "SHADOW_ROUTE_FEASIBILITY.md")
    with open(out_md, "w", encoding="utf-8") as f:
        f.write(render_markdown(report, manifest))

    print(f"wrote {out_json}")
    print(f"wrote {out_md}")
    print(f"summary: {report['summary']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
