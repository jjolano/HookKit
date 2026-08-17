#!/usr/bin/env python3
"""Self-check for hk_route_report.py. Run directly: python3 test_hk_route_report.py"""

from hk_route_report import classify_target, build_report


def test_objc_method_routable():
    r = classify_target({"target_kind": "objc_method", "required_reach": ["objc_dispatch"]})
    assert r["disposition"] == "routable"
    assert r["selected_route"]["engine_id"] == "objc-message"
    assert not r["requires_private_symbols"]


def test_plain_function_symbol_routable():
    r = classify_target({"target_kind": "function_symbol", "required_reach": ["existing_imports"]})
    assert r["disposition"] == "routable"
    assert r["selected_route"]["engine_id"] == "import-rebind"


def test_private_address_needs_decision_not_a_guess():
    r = classify_target({
        "target_kind": "function_symbol",
        "required_reach": ["existing_imports", "private_address"],
    })
    assert r["disposition"] == "needs_platform_decision"
    assert r["selected_route"] is None
    assert r["requires_private_symbols"] is True
    # The whole point: this must NOT claim Dobby/Gum without evidence.
    assert r["requires_dobby"] is False
    assert r["requires_gum"] is False
    assert r["blocking_reason"]


def test_unknown_shape_flagged_not_silently_routed():
    r = classify_target({"target_kind": "swift_vtable", "required_reach": ["swift_vtable_dispatch"]})
    assert r["disposition"] == "needs_platform_decision"
    assert "not a shape the modeled classifier covers" in r["blocking_reason"]


def test_lane_agnostic_pass_emits_one_all_lane_not_four_copies():
    # classify_target() takes no lane argument, so it cannot produce
    # per-lane differences yet -- the report should say so with one "all"
    # lane, not repeat identical data 4 times (an earlier version did
    # exactly that: 15,600+ lines for a 186-target manifest).
    manifest = {
        "manifest_version": "test",
        "targets": [
            {"stable_hook_id": "a", "target_kind": "objc_method", "required_reach": ["objc_dispatch"]},
            {"stable_hook_id": "b", "target_kind": "function_symbol", "required_reach": ["existing_imports"]},
        ],
    }
    report = build_report(manifest)
    assert len(report["lanes"]) == 1
    assert report["lanes"][0]["lane"] == "all"
    assert len(report["lanes"][0]["targets"]) == 2
    assert report["summary"]["unclassified_mandatory_targets"] == 0
    assert report["summary"]["dobby_required"] is False
    assert report["summary"]["gum_required"] is False


if __name__ == "__main__":
    tests = [v for k, v in list(globals().items()) if k.startswith("test_")]
    for t in tests:
        t()
        print(f"PASS {t.__name__}")
    print(f"all {len(tests)} tests passed")
