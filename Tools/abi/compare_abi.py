#!/usr/bin/env python3
"""Compare a historical ABI baseline with a candidate framework baseline."""

import argparse
import json
import re
import sys


def _methods(item, key):
    return {method["selector"]: method for method in item.get(key, [])}


def _abi_encoding(encoding):
    """Ignore fields added to a struct behind an already-stable pointer ABI."""
    if not encoding:
        return encoding
    return re.sub(r"\^\{([A-Za-z_][A-Za-z0-9_]*)=[^}]*\}", r"^{\1=}", encoding)


def compare(old, new, expected_install_name=None,
            expected_compatibility_version=None, required_headers=()):
    errors = []

    if expected_install_name:
        if new.get("install_name") != expected_install_name:
            errors.append(
                f"install name: expected {expected_install_name!r}, "
                f"got {new.get('install_name')!r}"
            )
    elif old.get("install_name") and new.get("install_name") != old["install_name"]:
        errors.append(
            f"install name changed: {old['install_name']!r} -> "
            f"{new.get('install_name')!r}"
        )
    if not new.get("install_name"):
        errors.append("candidate is missing install_name")

    if expected_compatibility_version and new.get("compatibility_version") != expected_compatibility_version:
        errors.append(
            f"compatibility version: expected {expected_compatibility_version!r}, "
            f"got {new.get('compatibility_version')!r}"
        )
    elif old.get("compatibility_version") and not new.get("compatibility_version"):
        errors.append("candidate is missing compatibility_version")

    for arch in old.get("architectures", []):
        if arch not in new.get("architectures", []):
            errors.append(f"missing architecture: {arch}")
        old_symbols = set(old.get("exported_symbols", {}).get(arch, []))
        new_symbols = set(new.get("exported_symbols", {}).get(arch, []))
        for symbol in sorted(old_symbols - new_symbols):
            errors.append(f"{arch}: removed exported symbol {symbol}")

    old_classes = {
        item["name"]: item for item in old.get("objc", {}).get("classes", [])
    }
    new_classes = {
        item["name"]: item for item in new.get("objc", {}).get("classes", [])
    }
    for name, old_class in sorted(old_classes.items()):
        new_class = new_classes.get(name)
        if new_class is None:
            errors.append(f"removed Objective-C class {name}")
            continue
        for key in ("instance_methods", "class_methods"):
            old_methods = _methods(old_class, key)
            new_methods = _methods(new_class, key)
            for selector, old_method in sorted(old_methods.items()):
                new_method = new_methods.get(selector)
                if new_method is None:
                    errors.append(f"{name}: removed {key[:-1]} {selector}")
                    continue
                for arch, encoding in old_method.get("type_encoding", {}).items():
                    actual = new_method.get("type_encoding", {}).get(arch)
                    if _abi_encoding(actual) != _abi_encoding(encoding):
                        errors.append(
                            f"{name} {selector} {arch}: type encoding changed "
                            f"{encoding!r} -> {actual!r}"
                        )
        old_properties = {prop["name"] for prop in old_class.get("properties", [])}
        new_properties = {prop["name"] for prop in new_class.get("properties", [])}
        for prop in sorted(old_properties - new_properties):
            errors.append(f"{name}: removed property {prop}")

    for name, value in sorted(old.get("enum_values", {}).items()):
        actual = new.get("enum_values", {}).get(name)
        if actual != value:
            errors.append(f"enum {name}: value changed {value} -> {actual}")

    candidate_headers = new.get("header_checksums", {})
    required = set(required_headers)
    required.update(old.get("header_checksums", {}).keys())
    for header in sorted(required):
        if header not in candidate_headers:
            errors.append(f"missing historical umbrella/header path {header}")

    return errors


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--old", required=True, help="historical baseline JSON")
    ap.add_argument("--new", required=True, help="candidate baseline JSON")
    ap.add_argument("--expected-install-name")
    ap.add_argument("--expected-compatibility-version")
    ap.add_argument("--required-header", action="append", default=[])
    args = ap.parse_args()

    with open(args.old, "r", encoding="utf-8") as stream:
        old = json.load(stream)
    with open(args.new, "r", encoding="utf-8") as stream:
        new = json.load(stream)
    errors = compare(old, new, args.expected_install_name,
                     args.expected_compatibility_version, args.required_header)
    if errors:
        for error in errors:
            print(f"FAIL {args.old}: {error}")
        return 1
    print(f"PASS {args.old}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
