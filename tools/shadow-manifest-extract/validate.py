#!/usr/bin/env python3
"""Validate a manifest against metadata/schemas/shadow-hook-manifest.schema.json.

Also runs the cross-checks the schema alone can't express (spec section
18.2's validate.py requirement): every stable_hook_id unique, every
manual_override entry in manual_overrides.yaml actually corresponds to a
target in the manifest (catches an override rotting after a source rename).
"""

import argparse
import json
import os
import sys


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("manifest", nargs="?",
                     default=None,
                     help="manifest JSON to validate (default: metadata/manifests/shadow-current-manifest.json)")
    args = ap.parse_args()

    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    manifest_path = args.manifest or os.path.join(repo_root, "metadata", "manifests", "shadow-current-manifest.json")
    schema_path = os.path.join(repo_root, "metadata", "schemas", "shadow-hook-manifest.schema.json")

    try:
        import jsonschema
    except ImportError:
        print("error: pip install jsonschema", file=sys.stderr)
        return 1

    with open(schema_path) as f:
        schema = json.load(f)
    with open(manifest_path) as f:
        manifest = json.load(f)

    validator_cls = jsonschema.validators.validator_for(schema)
    validator_cls.check_schema(schema)
    validator = validator_cls(schema)

    errors = list(validator.iter_errors(manifest))
    for e in errors:
        path = "/".join(str(p) for p in e.absolute_path)
        print(f"SCHEMA ERROR at {path or '<root>'}: {e.message}", file=sys.stderr)

    seen_ids = {}
    for idx, t in enumerate(manifest.get("targets", [])):
        hid = t.get("stable_hook_id")
        if hid in seen_ids:
            errors.append(f"duplicate stable_hook_id {hid!r} at targets[{idx}] and targets[{seen_ids[hid]}]")
            print(f"DUPLICATE ID: {hid!r} at targets[{idx}] and targets[{seen_ids[hid]}]", file=sys.stderr)
        else:
            seen_ids[hid] = idx

    overrides_path = os.path.join(os.path.dirname(__file__), "manual_overrides.yaml")
    if os.path.exists(overrides_path):
        try:
            import yaml
            with open(overrides_path) as f:
                overrides = (yaml.safe_load(f) or {}).get("overrides", {}) or {}
            for oid in overrides:
                if oid not in seen_ids:
                    print(f"STALE OVERRIDE: manual_overrides.yaml has {oid!r}, "
                          f"no such stable_hook_id in the manifest", file=sys.stderr)
                    errors.append(f"stale override {oid!r}")
        except ImportError:
            print("note: PyYAML not installed, skipped stale-override check", file=sys.stderr)

    if errors:
        print(f"FAIL: {len(errors)} problem(s) in {manifest_path}", file=sys.stderr)
        return 1

    print(f"OK: {manifest_path} — {len(manifest.get('targets', []))} targets, "
          f"schema-valid, no duplicate/stale IDs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
