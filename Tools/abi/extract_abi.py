#!/usr/bin/env python3
"""Extract a hookkit-abi-baseline.schema.json record from a built HookKit
framework binary. Spec section 17.1/17.3.

Current scope (honest, not aspirational -- see
docs/3.0/IMPLEMENTATION_STATUS.md, Milestone 3): install name, current/
compatibility version, architectures, and exported symbols per arch are
extracted for real, from the real Mach-O binary, using the same
Theos-toolchain tool-discovery order the existing scripts/check_exports.sh
and scripts/check_compat.sh already use (not reinvented: replicated here in
Python because those scripts are standalone executables, not sourceable
libraries).

NOT implemented yet, and the schema leaves these fields absent (not
empty/null) rather than faked: Objective-C class/method/property metadata,
and historical enum numeric values (spec wants these read from headers, not
the binary). Real Mach-O ObjC metadata parsing (__objc_classlist,
__objc_data, method lists, type encodings) is a meaningfully bigger task
than the load-command/symbol-table reads this version does -- tracked as
open work, not silently skipped.
"""

import argparse
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


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        h.update(f.read())
    return h.hexdigest()


def extract(binary, tag, header_paths, repo_root):
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
        "extractor_version": "0.1.0",
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
    return baseline


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", required=True, help="path to the built HookKit.framework/HookKit (or HKGum.dylib, etc.)")
    ap.add_argument("--tag", required=True, help="git tag or commit label this baseline represents")
    ap.add_argument("--header", action="append", default=[], help="header file to checksum (repeatable)")
    ap.add_argument("--repo-root", default=None,
                     help="root to compute header_checksums keys relative to "
                          "(default: this script's own repo root -- override when "
                          "extracting from a different checkout, e.g. a "
                          "`git worktree` for a historical tag, so the label isn't "
                          "a nonsensical ../../../.. chain across filesystem roots)")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    repo_root = args.repo_root or os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    baseline = extract(args.binary, args.tag, args.header, repo_root)

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(baseline, f, indent=2, sort_keys=True)
        f.write("\n")

    total_symbols = sum(len(v) for v in baseline["exported_symbols"].values())
    print(f"wrote {args.out}")
    print(f"  archs={baseline['architectures']} symbols={total_symbols} "
          f"install_name={baseline.get('install_name')!r} "
          f"current_version={baseline.get('current_version')!r}")
    print("  NOT extracted (see module docstring): objc classes/methods, enum_values")
    return 0


if __name__ == "__main__":
    sys.exit(main())
