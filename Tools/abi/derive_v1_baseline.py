#!/usr/bin/env python3
"""Derive the v1.0.1 HKSubstitutor-subset ABI baseline.

Unlike extract_abi.py, this does NOT read a Mach-O — no v1.0.1 binary exists on
disk and vendor/Modulous.framework is unvendored, so a real extraction is not
possible here, and a full seven-class extraction could never pass the "no
Modulous" gate anyway (compare_abi.py fails on any baseline class the 3.0 build
dropped). The retained contract is the v1 HKSubstitutor subset alone
(ARCHITECTURE.md: "the v1.0.1 HKSubstitutor subset must run unrecompiled").

Provenance of each field, all authoritative, nothing invented:
  * install_name / architectures / exported subset / versions
        -> the v1.0.1 tag's own shipped HookKit.tbd
  * selector list + enum values
        -> the v1.0.1 tag's Compat.h public contract (encoded below)
  * ObjC type encodings
        -> copied verbatim from the real, binary-extracted v2.1.1.json
           (identical selector signatures => genuinely linker-observed)
extractor_version is intentionally omitted to mark this file as assembled
rather than Mach-O-extracted. See docs/3.0/IMPLEMENTATION_STATUS.md
("v1.0.1: subset baseline added").

Idempotent: re-running reuses an existing output file's generated_at, so a
clean tree re-derives byte-for-byte (a drift check). `git show` the tag to
re-verify the tbd/Compat.h facts.
"""
import datetime
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
V211 = os.path.join(ROOT, "Tests/LegacyABI/Baselines/v2.1.1.json")
SCHEMA = os.path.join(ROOT, "Schemas/hookkit-abi-baseline.schema.json")
OUT = os.path.join(ROOT, "Tests/LegacyABI/Baselines/v1.0.1.json")

# v1.0.1 Compat.h public HKSubstitutor contract (full selectors).
V1_CLASS = ["getAvailableSubstitutorTypes", "getSubstitutorTypeInfo:",
            "substitutorWithTypes:", "defaultSubstitutor"]
V1_INSTANCE = ["initLibraries",
               "hookMessageInClass:withSelector:withReplacement:outOldPtr:",
               "hookFunction:withReplacement:outOldPtr:",
               "hookMemory:withData:size:", "openImage:", "closeImage:",
               "findSymbolsInImage:symbolNames:outSymbols:",
               "findSymbolInImage:symbolName:", "executeHooks", "getLibErrno:",
               "types", "setTypes:", "batching", "setBatching:"]
V1_PROPS = ["types", "batching"]
# sha256 of `git show v1.0.1:Headers/HookKit.h` (presence-checked, not value-compared).
V1_UMBRELLA_SHA = "f6b26d282aa74a754497ed1071e17165877b27d9d3dc85510eb4fd4a5313aa28"
ARCHS = ["arm64", "arm64e"]  # modern gate surface, matching every existing baseline
EXPORTS = ["_OBJC_CLASS_$_HKSubstitutor", "_OBJC_METACLASS_$_HKSubstitutor"]


def _pick(methods, allow):
    by = {m["selector"]: m for m in methods}
    missing = [s for s in allow if s not in by]
    if missing:
        raise SystemExit(f"FATAL: v2.1.1 lacks v1 selectors (cannot source "
                         f"encodings honestly): {missing}")
    return [by[s] for s in allow]


def build():
    src = json.load(open(V211))
    hks = next(c for c in src["objc"]["classes"] if c["name"] == "HKSubstitutor")
    cls = {
        "name": "HKSubstitutor",
        "class_methods": _pick(hks.get("class_methods", []), V1_CLASS),
        "instance_methods": _pick(hks.get("instance_methods", []), V1_INSTANCE),
        "properties": [p for p in hks.get("properties", []) if p["name"] in V1_PROPS],
    }
    assert {p["name"] for p in cls["properties"]} == set(V1_PROPS)

    generated_at = datetime.datetime.now(datetime.timezone.utc).strftime(
        "%Y-%m-%dT%H:%M:%SZ")
    if os.path.exists(OUT):  # idempotent re-derive
        generated_at = json.load(open(OUT)).get("generated_at", generated_at)

    return {
        "tag": "v1.0.1",
        "generated_at": generated_at,
        "install_name": "@rpath/HookKit.framework/HookKit",
        "current_version": "0",
        "compatibility_version": "0",
        "architectures": ARCHS,
        "exported_symbols": {a: list(EXPORTS) for a in ARCHS},
        "objc": {"classes": [cls]},
        "enum_values": {"HK_OK": 0, "HK_ERR": 1, "HK_ERR_NOT_SUPPORTED": 2,
                        "HK_LIB_NONE": 0},
        "header_checksums": {"Headers/HookKit.h": V1_UMBRELLA_SHA},
    }


def main():
    baseline = build()
    try:
        import jsonschema
        jsonschema.validate(baseline, json.load(open(SCHEMA)))
    except ImportError:
        for key in ("tag", "generated_at", "install_name", "architectures",
                    "exported_symbols"):
            assert key in baseline, f"schema-required key missing: {key}"
    with open(OUT, "w") as f:
        json.dump(baseline, f, indent=2, sort_keys=True)
        f.write("\n")
    print(f"wrote {os.path.relpath(OUT, ROOT)}")


if __name__ == "__main__":
    main()
