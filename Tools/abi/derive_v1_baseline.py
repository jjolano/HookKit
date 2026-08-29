#!/usr/bin/env python3
"""Derive the v1.0.1 ABI baseline: all seven classes.

No v1.0.1 binary exists on disk and vendor/Modulous.framework is unvendored, so
this is assembled rather than Mach-O-extracted.

Provenance of each field, all authoritative, nothing invented:
  * install_name / architectures / versions
        -> the v1.0.1 tag's own shipped HookKit.tbd
  * selector and property lists, for every class
        -> the v1.0.1 tag's public headers (Compat.h for HKSubstitutor,
           Core.h / Hook.h / Module.h / Module+Internal.h for the rest),
           transcribed below
  * HKSubstitutor type encodings
        -> copied verbatim from the real, binary-extracted v2.1.1.json
           (identical selector signatures => genuinely linker-observed)
  * module-class type encodings
        -> read from a built HookKit via --from-binary. v2 never shipped these
           six classes, so there is no older binary to copy from; clang encoding
           the v1.0.1 header signatures is the nearest authoritative source.
           Encodings are a deterministic function of those signatures, so this
           records what the v1 declarations mean, not what our build happens to
           emit -- and the selector lists below stay the real gate: derivation
           FAILS if the build is missing anything v1 declared.

extractor_version is intentionally omitted to mark this file as assembled
rather than Mach-O-extracted. See docs/3.0/IMPLEMENTATION_STATUS.md
("v1.0.1: subset baseline added").

Idempotent: re-running reuses an existing output file's generated_at, so a
clean tree re-derives byte-for-byte (a drift check). `git show` the tag to
re-verify the tbd/header facts.
"""
import argparse
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


def _accessors(props):
    """A v1 @property contributes a getter and a setter to the class ABI."""
    out = []
    for name in props:
        out += [name, "set" + name[0].upper() + name[1:] + ":"]
    return out


# v1.0.1 module surface, transcribed from the tag's Core.h / Hook.h / Module.h /
# Module+Internal.h. Every entry here must exist in the built framework or
# derivation fails -- this is what makes the baseline a completeness gate.
V1_MODULE_CLASSES = {
    # HookKitHook is a bare NSObject subclass: the class symbol is the ABI.
    "HookKitHook": {"class": [], "instance": [], "props": []},
    "HookKitClassHook": {
        "class": ["hook:selector:replacement:orig:"],
        "props": ["objcClass", "selector", "replacement", "orig"],
    },
    "HookKitFunctionHook": {
        "class": ["hook:replacement:orig:"],
        "props": ["function", "replacement", "orig"],
    },
    "HookKitMemoryHook": {
        "class": ["hook:data:size:"],
        "props": ["target", "data", "size"],
    },
    "HookKitModule": {
        "class": [],
        "instance": [
            "executeHook:", "executeHooks:", "openImageWithURL:",
            "openImageWithPath:", "closeImage:", "findSymbolName:",
            "findSymbolName:inImage:",
            # Module+Internal.h: v1's provider seam, part of the same ABI.
            "_hookClass:selector:replacement:orig:",
            "_hookFunction:replacement:orig:", "_hookFunctions:",
            "_hookRegion:data:size:", "_hookRegions:", "_openImage:",
            "_closeImage:", "_findSymbol:image:",
        ],
        "props": ["functionHookBatchingSupported", "memoryHookBatchingSupported",
                  "nullImageSearchSupported"],
    },
    "HookKitCore": {
        "class": ["sharedInstance"],
        "instance": ["defaultModule", "getModuleInfo",
                     "getModuleInfoWithIdentifier:", "getModuleWithIdentifier:",
                     "registerModule:withIdentifier:"],
        "props": [],
    },
}

ALL_CLASSES = ["HKSubstitutor"] + list(V1_MODULE_CLASSES)
EXPORTS = [f"_OBJC_{kind}$_{name}"
           for name in ALL_CLASSES for kind in ("CLASS_", "METACLASS_")]


def _pick(methods, allow):
    by = {m["selector"]: m for m in methods}
    missing = [s for s in allow if s not in by]
    if missing:
        raise SystemExit(f"FATAL: v2.1.1 lacks v1 selectors (cannot source "
                         f"encodings honestly): {missing}")
    return [by[s] for s in allow]


def _module_classes(binary_baseline):
    """Project the six v1 module classes out of a built HookKit's ABI dump."""
    by_name = {c["name"]: c for c in binary_baseline["objc"]["classes"]}
    out = []
    for name, spec in V1_MODULE_CLASSES.items():
        built = by_name.get(name)
        if built is None:
            raise SystemExit(f"FATAL: built HookKit is missing v1 class {name}")
        instance = list(spec.get("instance", [])) + _accessors(spec["props"])
        out.append({
            "name": name,
            "class_methods": _pick(built.get("class_methods", []), spec["class"]),
            "instance_methods": _pick(built.get("instance_methods", []), instance),
            "properties": [p for p in built.get("properties", [])
                           if p["name"] in spec["props"]],
        })
        missing = set(spec["props"]) - {p["name"] for p in out[-1]["properties"]}
        if missing:
            raise SystemExit(f"FATAL: {name} is missing v1 properties: "
                             f"{sorted(missing)}")
    return out


def build(from_binary=None):
    src = json.load(open(V211))
    hks = next(c for c in src["objc"]["classes"] if c["name"] == "HKSubstitutor")
    cls = {
        "name": "HKSubstitutor",
        "class_methods": _pick(hks.get("class_methods", []), V1_CLASS),
        "instance_methods": _pick(hks.get("instance_methods", []), V1_INSTANCE),
        "properties": [p for p in hks.get("properties", []) if p["name"] in V1_PROPS],
    }
    assert {p["name"] for p in cls["properties"]} == set(V1_PROPS)

    classes = [cls]
    if from_binary:
        classes += _module_classes(json.load(open(from_binary)))
    elif os.path.exists(OUT):
        # Re-derive without a build: keep the module classes already recorded
        # rather than silently narrowing the baseline back to the subset.
        prior = {c["name"]: c for c in json.load(open(OUT))["objc"]["classes"]}
        missing = [n for n in V1_MODULE_CLASSES if n not in prior]
        if missing:
            raise SystemExit(f"FATAL: --from-binary is required to add {missing}")
        classes += [prior[n] for n in V1_MODULE_CLASSES]
    else:
        raise SystemExit("FATAL: --from-binary is required for a first derive")

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
        "objc": {"classes": classes},
        "enum_values": {"HK_OK": 0, "HK_ERR": 1, "HK_ERR_NOT_SUPPORTED": 2,
                        "HK_LIB_NONE": 0},
        "header_checksums": {"Headers/HookKit.h": V1_UMBRELLA_SHA},
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--from-binary", metavar="ABI.json",
                    help="extract_abi.py output for a built HookKit; supplies "
                         "the six module classes' type encodings")
    args = ap.parse_args()
    baseline = build(args.from_binary)
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
