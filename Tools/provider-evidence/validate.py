#!/usr/bin/env python3
"""Validate HookKit provider evidence records and their routing gate."""

from __future__ import annotations

import argparse
import datetime as _datetime
import hashlib
import json
import re
from pathlib import Path

FAMILIES = {"ellekit", "libhooker", "substrate", "substitute", "dobby", "gum"}
ARCHITECTURES = {"armv7", "armv7s", "arm64", "arm64e", "arm64e_old_abi"}
LANES = {"rootful-legacy", "rootful-modern", "rootless", "roothide"}
CONTINUATIONS = {"none", "static", "dynamic", "provider_internal", "unknown"}
CERTIFICATIONS = {"uncertified", "host_certified", "device_certified"}
SHA256 = re.compile(r"^[0-9a-fA-F]{64}$")


def validate_record(record: dict, source: str = "<record>") -> list[str]:
    errors = []
    required = {
        "provider_family",
        "version",
        "sha256",
        "architecture",
        "os_lane",
        "continuation_kind",
        "certification_status",
    }
    missing = sorted(required - record.keys())
    errors.extend(f"{source}: missing {key}" for key in missing)

    if record.get("provider_family") not in FAMILIES:
        errors.append(f"{source}: invalid provider_family")
    if not isinstance(record.get("version"), str) or not record.get("version"):
        errors.append(f"{source}: version must be a non-empty string")
    if not isinstance(record.get("sha256"), str) or not SHA256.fullmatch(record.get("sha256", "")):
        errors.append(f"{source}: sha256 must be 64 hexadecimal characters")
    if record.get("architecture") not in ARCHITECTURES:
        errors.append(f"{source}: invalid architecture")
    if record.get("os_lane") not in LANES:
        errors.append(f"{source}: invalid os_lane")
    if record.get("continuation_kind") not in CONTINUATIONS:
        errors.append(f"{source}: invalid continuation_kind")
    if record.get("certification_status") not in CERTIFICATIONS:
        errors.append(f"{source}: invalid certification_status")

    for key in ("commit", "load_path", "discovery_method"):
        if key in record and not isinstance(record[key], str):
            errors.append(f"{source}: {key} must be a string")

    activation = record.get("activation_behavior")
    if activation is not None:
        if not isinstance(activation, dict):
            errors.append(f"{source}: activation_behavior must be an object")
        else:
            enums = {
                "constructor_behavior": {"none", "bounded_and_known", "unbounded_or_unknown"},
                "thread_behavior": {"none", "creates_threads"},
                "callback_behavior": {"none", "registers_callbacks"},
                "executable_allocation_behavior": {"none", "static_only", "dynamic"},
            }
            for key, allowed in enums.items():
                if key in activation and activation[key] not in allowed:
                    errors.append(f"{source}: invalid activation_behavior.{key}")
            for key in ("requires_provider_activation", "requires_provider_image_load"):
                if key in activation and not isinstance(activation[key], bool):
                    errors.append(f"{source}: activation_behavior.{key} must be boolean")

    if "continuation_inspectable" in record and not isinstance(record["continuation_inspectable"], bool):
        errors.append(f"{source}: continuation_inspectable must be boolean")
    if record.get("continuation_kind") in {"provider_internal", "unknown"} and record.get("continuation_inspectable") is True:
        errors.append(f"{source}: opaque continuation cannot be inspectable")

    for result in record.get("device_test_results", []):
        if not isinstance(result, dict):
            errors.append(f"{source}: device_test_results entry must be an object")
            continue
        if result.get("verification_level") not in {"host_verified", "device_verified"}:
            errors.append(f"{source}: invalid device test verification_level")
        if result.get("result") not in {"pass", "fail", "not_run"}:
            errors.append(f"{source}: invalid device test result")
        if result.get("verification_level") == "device_verified" and result.get("result") != "pass":
            errors.append(f"{source}: device_verified requires a passing result")
        if "date" in result:
            try:
                _datetime.date.fromisoformat(result["date"])
            except (TypeError, ValueError):
                errors.append(f"{source}: invalid device test date")

    return errors


def automatic_route_eligible(record: dict) -> bool:
    """Return the conservative spec 8.7 provider eligibility decision."""
    return (
        record.get("certification_status") == "device_certified"
        and record.get("continuation_kind") not in {"provider_internal", "unknown"}
        and record.get("original_publication_order") == "before_activation"
        and record.get("mutation_failure_semantics") != "unknown"
    )


def load_records(root: Path) -> list[tuple[Path, dict]]:
    records = []
    evidence_root = root / "ProviderEvidence"
    for path in sorted(evidence_root.glob("*/*.json")):
        records.append((path, json.loads(path.read_text(encoding="utf-8"))))
    return records


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--require-device-certified", action="store_true")
    args = parser.parse_args(argv)

    records = load_records(args.root)
    if not records:
        print("provider evidence: no records")
        return 1

    errors = []
    eligible = 0
    for path, record in records:
        errors.extend(validate_record(record, str(path)))
        if automatic_route_eligible(record):
            eligible += 1

    if args.require_device_certified and eligible != len(records):
        errors.append("provider evidence: every record must be eligible for automatic routing")
    if errors:
        print("\n".join(errors))
        return 1

    print(f"provider evidence: {len(records)} valid record(s), {eligible} automatic-route eligible")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
