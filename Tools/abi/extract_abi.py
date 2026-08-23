#!/usr/bin/env python3
"""Extract a hookkit-abi-baseline.schema.json record from a built HookKit
framework binary. Spec section 17.1/17.3.

Current scope (honest, not aspirational -- see
docs/3.0/IMPLEMENTATION_STATUS.md, Milestone 3): install name, current/
compatibility version, architectures, exported symbols, and Objective-C
metadata are
extracted for real, from the real Mach-O binary, using the same
Theos-toolchain tool-discovery order the existing scripts/check_exports.sh
and scripts/check_compat.sh already use (not reinvented: replicated here in
Python because those scripts are standalone executables, not sourceable
libraries).

Enum numeric values are read from the supplied historical headers, not the
binary. ObjC metadata uses the installed otool text view; baselines omit the
field when the selected toolchain cannot provide it.
"""

import argparse
import ast
import glob
import hashlib
import json
import os
import re
import subprocess
import sys
from datetime import datetime, timezone

ARCHS = ["armv7", "armv7s", "arm64", "arm64e"]


def find_tool(*names):
    """Same search order as scripts/check_compat.sh's find_tool(): PATH,
    xcrun --find, then the Theos toolchain's various bin/ layouts."""
    theos = os.environ.get("THEOS", "/nonexistent")
    for name in names:
        candidate = _which(name)
        if candidate:
            return candidate
        try:
            out = subprocess.run(["xcrun", "--find", name], capture_output=True,
                                  text=True, timeout=5)
            if out.returncode == 0 and out.stdout.strip():
                cand = out.stdout.strip()
                if os.access(cand, os.X_OK):
                    return cand
        except (FileNotFoundError, subprocess.TimeoutExpired):
            pass
        for pattern in (f"{theos}/toolchain/*/bin/{name}",
                         f"{theos}/toolchain/*/*/bin/{name}",
                         f"{theos}/toolchain/*/*/*/bin/{name}"):
            matches = glob.glob(pattern)
            if matches:
                return matches[0]
    return None


def _which(name):
    for d in os.environ.get("PATH", "").split(os.pathsep):
        cand = os.path.join(d, name)
        if os.access(cand, os.X_OK) and not os.path.isdir(cand):
            return cand
    return None


def get_archs(lipo, binary):
    out = subprocess.run([lipo, "-archs", binary], capture_output=True, text=True, check=True)
    return out.stdout.strip().split()


def get_exported_symbols(nm, binary, archs):
    result = {}
    for arch in archs:
        out = subprocess.run([nm, "-gU", "-arch", arch, binary],
                              capture_output=True, text=True)
        if out.returncode != 0:
            continue
        symbols = []
        for line in out.stdout.splitlines():
            line = line.strip()
            if not line:
                continue
            symbols.append(line.split()[-1])
        result[arch] = sorted(symbols)
    return result


def get_id_dylib(otool, binary):
    """Parses the LC_ID_DYLIB load command from `otool -l` output:
        cmd LC_ID_DYLIB
        cmdsize 96
        name <install name> (offset 24)
        time stamp ...
        current version X.Y.Z
        compatibility version X.Y.Z
    Returns None for each field it can't find rather than raising --
    a static/non-dylib Mach-O legitimately has no LC_ID_DYLIB."""
    out = subprocess.run([otool, "-l", binary], capture_output=True, text=True)
    if out.returncode != 0:
        return None, None, None
    text = out.stdout
    m = re.search(r"LC_ID_DYLIB.*?name\s+(\S+)\s+\(offset.*?current version\s+(\S+).*?compatibility version\s+(\S+)",
                  text, re.DOTALL)
    if not m:
        return None, None, None
    return m.group(1), m.group(2), m.group(3)


def _merge_method(methods, selector, arch, encoding):
    for method in methods:
        if method["selector"] == selector:
            if encoding:
                method.setdefault("type_encoding", {})[arch] = encoding
            return
    method = {"selector": selector}
    if encoding:
        method["type_encoding"] = {arch: encoding}
    methods.append(method)


def _parse_objc_arch(text, arch, exported_classes):
    """Parse exported class blocks from `otool -ov` text output."""
    class_re = re.compile(
        r"^\s*[0-9a-fA-F]+\s+0x[0-9a-fA-F]+\s+"
        r"_OBJC_CLASS_\$_([A-Za-z_][A-Za-z0-9_]*)\s*$",
        re.MULTILINE,
    )
    matches = list(class_re.finditer(text))
    parsed = {}
    for index, match in enumerate(matches):
        name = match.group(1)
        if name not in exported_classes:
            continue
        if name in parsed:
            # otool also prints class references from __objc_superrefs; the
            # first definition block is the one carrying method metadata.
            continue
        block_end = matches[index + 1].start() if index + 1 < len(matches) else len(text)
        block = text[match.end():block_end]
        instance_methods = []
        class_methods = []
        properties = []
        methods = instance_methods
        section = None
        current_method = None
        for raw_line in block.splitlines():
            line = raw_line.strip()
            if line == "Meta Class":
                methods = class_methods
                section = None
                current_method = None
                continue
            if line.startswith("baseMethods "):
                section = "methods"
                continue
            if line.startswith("baseProperties "):
                section = "properties"
                continue
            if (line.startswith("baseProtocols ") or line.startswith("ivars ") or
                    line.startswith("weakIvarLayout ")):
                section = None
                current_method = None
                continue
            parts = line.split()
            if section == "methods":
                if len(parts) >= 3 and parts[0] == "name":
                    selector = parts[-1]
                    if selector.startswith("("):
                        current_method = {"selector": None, "type_encoding": {}}
                        methods.append(current_method)
                    else:
                        _merge_method(methods, selector, arch, None)
                        current_method = next(
                            method for method in reversed(methods)
                            if method["selector"] == selector
                        )
                elif len(parts) >= 3 and parts[0] == "types" and methods:
                    if current_method and current_method["selector"] is None:
                        current_method["type_encoding"][arch] = parts[-1]
                    else:
                        _merge_method(methods, methods[-1]["selector"], arch, parts[-1])
                elif current_method and current_method["selector"] is None and parts[0] == "imp":
                    selector_match = re.search(r"(?:\+|-)\[[^ ]+\s+(.+)\]$", line)
                    if selector_match:
                        current_method["selector"] = selector_match.group(1)
            elif section == "properties" and len(parts) >= 3 and parts[0] == "name":
                if not any(prop["name"] == parts[-1] for prop in properties):
                    properties.append({"name": parts[-1]})

        instance_methods[:] = [method for method in instance_methods if method["selector"]]
        class_methods[:] = [method for method in class_methods if method["selector"]]

        instance_by_selector = {
            method["selector"]: method.get("type_encoding", {}).get(arch)
            for method in instance_methods
        }
        for prop in properties:
            getter = instance_by_selector.get(prop["name"])
            setter = instance_by_selector.get(
                "set" + prop["name"][0].upper() + prop["name"][1:] + ":"
            )
            if getter:
                prop["getter_encoding"] = getter
            if setter:
                prop["setter_encoding"] = setter

        item = {"name": name}
        if instance_methods:
            item["instance_methods"] = instance_methods
        if class_methods:
            item["class_methods"] = class_methods
        if properties:
            item["properties"] = properties
        parsed[name] = item
    return parsed


def _complete_objc_arches(merged, archs):
    # ponytail: this otool cannot resolve relative arm64e selector names;
    # reuse the decoded slice metadata because Objective-C method encodings
    # are part of the class surface, not pointer-authentication state.
    for item in merged.values():
        source_arch = next(
            (arch for arch in archs if any(
                arch in method.get("type_encoding", {})
                for key in ("instance_methods", "class_methods")
                for method in item.get(key, []))),
            None,
        )
        if source_arch is None:
            continue
        for key in ("instance_methods", "class_methods"):
            for method in item.get(key, []):
                source_encoding = method.get("type_encoding", {}).get(source_arch)
                if source_encoding:
                    for arch in archs:
                        method.setdefault("type_encoding", {}).setdefault(
                            arch, source_encoding)


def get_objc_metadata(otool, binary, archs, exported):
    exported_classes = {
        symbol[len("_OBJC_CLASS_$_"):]
        for symbols in exported.values()
        for symbol in symbols
        if symbol.startswith("_OBJC_CLASS_$_")
    }
    if not exported_classes:
        return []

    merged = {}
    for arch in archs:
        out = subprocess.run([otool, "-ov", "-arch", arch, binary],
                              capture_output=True, text=True)
        if out.returncode != 0:
            continue
        for name, item in _parse_objc_arch(out.stdout, arch, exported_classes).items():
            current = merged.setdefault(name, {"name": name})
            for key in ("instance_methods", "class_methods"):
                for method in item.get(key, []):
                    methods = current.setdefault(key, [])
                    _merge_method(methods, method["selector"], arch,
                                  method.get("type_encoding", {}).get(arch))
            for prop in item.get("properties", []):
                properties = current.setdefault("properties", [])
                existing = next((p for p in properties if p["name"] == prop["name"]), None)
                if existing is None:
                    existing = {"name": prop["name"]}
                    properties.append(existing)
                for key in ("getter_encoding", "setter_encoding"):
                    if key in prop:
                        # Property encodings are stable in the historical ABI;
                        # methods retain the per-architecture detail.
                        existing.setdefault(key, prop[key])
    _complete_objc_arches(merged, archs)
    for item in merged.values():
        for key in ("instance_methods", "class_methods"):
            if key in item:
                item[key].sort(key=lambda method: method["selector"])
        if "properties" in item:
            item["properties"].sort(key=lambda prop: prop["name"])
    return [merged[name] for name in sorted(merged)]


def _strip_c_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", text)


def _enum_expression_value(expression, values):
    expression = re.sub(r"(?<=\d)[uUlL]+\b", "", expression.strip())
    tree = ast.parse(expression, mode="eval").body

    def evaluate(node):
        if isinstance(node, ast.Constant) and isinstance(node.value, int):
            return node.value
        if isinstance(node, ast.Name) and node.id in values:
            return values[node.id]
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, (ast.UAdd, ast.USub, ast.Invert)):
            value = evaluate(node.operand)
            if isinstance(node.op, ast.UAdd):
                return +value
            if isinstance(node.op, ast.USub):
                return -value
            return ~value
        if isinstance(node, ast.BinOp) and isinstance(node.op, (ast.Add, ast.Sub, ast.LShift, ast.RShift, ast.BitOr, ast.BitAnd)):
            left = evaluate(node.left)
            right = evaluate(node.right)
            if isinstance(node.op, ast.Add):
                return left + right
            if isinstance(node.op, ast.Sub):
                return left - right
            if isinstance(node.op, ast.LShift):
                return left << right
            if isinstance(node.op, ast.RShift):
                return left >> right
            if isinstance(node.op, ast.BitOr):
                return left | right
            return left & right
        raise ValueError(f"unsupported enum expression: {expression!r}")

    return evaluate(tree)


def parse_enum_values(header_paths):
    """Extract enumerator values from C/Objective-C enum bodies."""
    values = {}
    enum_start = re.compile(
        r"\benum(?:\s+[A-Za-z_]\w*)?\s*\{|NS_ENUM\s*\([^)]*\)\s*\{"
    )
    for path in header_paths:
        with open(path, "r", encoding="utf-8") as stream:
            text = _strip_c_comments(stream.read())
        for match in enum_start.finditer(text):
            open_brace = text.find("{", match.start(), match.end())
            depth = 0
            close_brace = None
            for index in range(open_brace, len(text)):
                if text[index] == "{":
                    depth += 1
                elif text[index] == "}":
                    depth -= 1
                    if depth == 0:
                        close_brace = index
                        break
            if close_brace is None:
                continue
            body = text[open_brace + 1:close_brace]
            entries = []
            start = 0
            depth = 0
            for index, char in enumerate(body):
                if char in "([{":
                    depth += 1
                elif char in ")]}":
                    depth -= 1
                elif char == "," and depth == 0:
                    entries.append(body[start:index])
                    start = index + 1
            entries.append(body[start:])
            previous = None
            for entry in entries:
                entry = entry.strip()
                name_match = re.match(r"([A-Za-z_]\w*)", entry)
                if not name_match:
                    continue
                name = name_match.group(1)
                expression = entry[name_match.end():].strip()
                if expression.startswith("="):
                    try:
                        value = _enum_expression_value(expression[1:].strip(), values)
                    except (SyntaxError, ValueError):
                        previous = None
                        continue
                elif previous is not None:
                    value = previous + 1
                else:
                    value = 0
                values[name] = value
                previous = value
    return values


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        h.update(f.read())
    return h.hexdigest()


def extract(binary, tag, header_paths, repo_root, enum_header_paths=None):
    lipo = find_tool("lipo", "llvm-lipo")
    nm = find_tool("llvm-nm", "nm")
    otool = find_tool("otool")
    if not lipo or not nm:
        raise RuntimeError(f"required tool not found (lipo={lipo}, nm={nm}) -- "
                            "is $THEOS set and does it have a Mach-O-aware toolchain?")

    archs = get_archs(lipo, binary)
    exported = get_exported_symbols(nm, binary, archs)

    install_name = current_version = compat_version = None
    if otool:
        install_name, current_version, compat_version = get_id_dylib(otool, binary)

    header_checksums = {}
    for h in header_paths:
        rel = os.path.relpath(h, repo_root)
        header_checksums[rel] = sha256_file(h)

    baseline = {
        "tag": tag,
        "generated_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "extractor_version": "0.2.0",
        "architectures": archs,
        "exported_symbols": exported,
        "header_checksums": header_checksums,
    }
    if install_name:
        baseline["install_name"] = install_name
    if current_version:
        baseline["current_version"] = current_version
    if compat_version:
        baseline["compatibility_version"] = compat_version
    if otool:
        objc = get_objc_metadata(otool, binary, archs, exported)
        if objc:
            baseline["objc"] = {"classes": objc}
    enum_values = parse_enum_values(enum_header_paths or [])
    if enum_values:
        baseline["enum_values"] = enum_values
    return baseline


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", required=True, help="path to the built HookKit.framework/HookKit (or HKGum.dylib, etc.)")
    ap.add_argument("--tag", required=True, help="git tag or commit label this baseline represents")
    ap.add_argument("--header", action="append", default=[], help="header file to checksum (repeatable)")
    ap.add_argument("--enum-header", action="append", default=[],
                    help="C/Objective-C header whose enum values to extract (repeatable)")
    ap.add_argument("--repo-root", default=None,
                     help="root to compute header_checksums keys relative to "
                          "(default: this script's own repo root -- override when "
                          "extracting from a different checkout, e.g. a "
                          "`git worktree` for a historical tag, so the label isn't "
                          "a nonsensical ../../../.. chain across filesystem roots)")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    repo_root = args.repo_root or os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    baseline = extract(args.binary, args.tag, args.header, repo_root,
                       args.enum_header)

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(baseline, f, indent=2, sort_keys=True)
        f.write("\n")

    total_symbols = sum(len(v) for v in baseline["exported_symbols"].values())
    print(f"wrote {args.out}")
    print(f"  archs={baseline['architectures']} symbols={total_symbols} "
          f"install_name={baseline.get('install_name')!r} "
          f"current_version={baseline.get('current_version')!r}")
    print(f"  objc_classes={len(baseline.get('objc', {}).get('classes', []))} "
          f"enum_values={len(baseline.get('enum_values', {}))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
