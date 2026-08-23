#!/usr/bin/env python3
"""Extract Logos Objective-C targets through Clang's JSON AST."""

import argparse
import json
import os
import subprocess
import sys
import tempfile

from logos_preprocess import normalized_source, parse_file


def walk(node, parent=None):
    yield node, parent
    for child in node.get("inner", []):
        yield from walk(child, node)


def extract_file(path, clang="clang"):
    metadata = parse_file(path)
    source = normalized_source(metadata)
    with tempfile.NamedTemporaryFile("w", suffix=".m", delete=False) as stream:
        stream.write(source)
        normalized = stream.name
    try:
        command = [clang, "-fsyntax-only", "-x", "objective-c",
                   "-Xclang", "-ast-dump=json", normalized]
        result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, check=False)
        if result.returncode != 0:
            raise RuntimeError(result.stderr.strip() or "clang AST dump failed")
        tree = json.loads(result.stdout)
    finally:
        os.unlink(normalized)

    targets = []
    for node, parent in walk(tree):
        if node.get("kind") != "ObjCMethodDecl" or not parent:
            continue
        selector = node.get("name")
        if not selector:
            continue
        loc = node.get("loc", {})
        targets.append({
            "target_kind": "objc_method",
            "target_class": parent.get("name"),
            "target_selector": selector,
            "method_kind": "instance" if node.get("instance") else "class",
            "source": {"file": path, "line": loc.get("line", 1)},
            "extraction_method": "static_ast",
        })
    return targets


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("source")
    parser.add_argument("--clang", default="clang")
    args = parser.parse_args(argv)
    json.dump({"targets": extract_file(args.source, args.clang)}, sys.stdout, indent=2)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
