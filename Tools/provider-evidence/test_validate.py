#!/usr/bin/env python3
"""Small self-check for provider evidence validation and routing safety."""

from validate import automatic_route_eligible, validate_record


def record(**overrides):
    value = {
        "provider_family": "dobby",
        "version": "test",
        "sha256": "0" * 64,
        "architecture": "arm64",
        "os_lane": "rootless",
        "continuation_kind": "dynamic",
        "continuation_inspectable": True,
        "original_publication_order": "before_activation",
        "mutation_failure_semantics": "may_partially_mutate",
        "certification_status": "device_certified",
    }
    value.update(overrides)
    return value


def test_valid_record():
    assert validate_record(record()) == []
    assert automatic_route_eligible(record())


def test_uncertified_record_cannot_route():
    assert not automatic_route_eligible(record(certification_status="uncertified"))


def test_unknown_continuation_cannot_route():
    value = record(continuation_kind="unknown", continuation_inspectable=False)
    assert validate_record(value) == []
    assert not automatic_route_eligible(value)


def test_bad_hash_is_rejected():
    assert any("sha256" in error for error in validate_record(record(sha256="bad")))


if __name__ == "__main__":
    for name, test in sorted(globals().items()):
        if name.startswith("test_"):
            test()
            print(f"PASS {name}")
