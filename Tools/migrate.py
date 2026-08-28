#!/usr/bin/env python3
"""
migrate.py — migrate a generic Logos/Substrate tweak to HookKit.

Substrate / substitute / libhooker / ElleKit raw calls → HookKit via HookKitCompat.h.
%hook / %orig → HookKit via hookkit Logos generator (no source change, just Makefile).

Idempotent. Dry-run by default.

Usage:
  python3 Tools/migrate.py --check Tweak.x Makefile
  python3 Tools/migrate.py --apply Tweak.x Makefile
  python3 Tools/migrate.py --revert Tweak.x Makefile
  python3 Tools/migrate.py --check --all   # finds *.x *.xm Makefile* in cwd
"""

import argparse
import re
import sys
from pathlib import Path

COMPAT_HEADER = "HookKit/HookKitCompat.h"
COMPAT_IMPORT = f"#import <{COMPAT_HEADER}>"
MAKE_FRAMEWORK = "Tweak_EXTRA_FRAMEWORKS += HookKit"
# Theos expects per-instance variant: <instance>_EXTRA_FRAMEWORKS
MAKE_FRAMEWORK_RE = re.compile(r'^\s*\w+_EXTRA_FRAMEWORKS\s*\+=.*HookKit', re.M)
MAKE_LOGOS_RE = re.compile(r'generator\s*=\s*hookkit')
MAKE_LOGOS = "Tweak_LOGOSFLAGS += -c generator=hookkit"

PATTERNS = {
    "%hook": re.compile(r'^\s*%hook\b', re.M),
    "%orig": re.compile(r'%orig\b'),
    "MSHookFunction": re.compile(r'\bMSHookFunction\b'),
    "MSHookMessageEx": re.compile(r'\bMSHookMessageEx\b'),
    "MSHookMemory": re.compile(r'\bMSHookMemory\b'),
    "substitute_hook": re.compile(r'\bsubstitute_hook_functions\b|\bsubstitute_hook_objc_message\b'),
    "LHHookFunctions": re.compile(r'\bLHHookFunctions\b'),
    "LBHookMessage": re.compile(r'\bLBHookMessage\b'),
    "LHPatchMemory": re.compile(r'\bLHPatchMemory\b'),
    "substrate.h": re.compile(r'#\s*(import|include)\s*[<"]substrate\.h[">]'),
    "substitute.h": re.compile(r'#\s*(import|include)\s*[<"]substitute\.h[">]'),
    "libhooker.h": re.compile(r'#\s*(import|include)\s*[<"]libhooker'),
}

def scan_file(p: Path):
    try:
        t = p.read_text()
    except Exception:
        return {}
    return {k: len(rx.findall(t)) for k, rx in PATTERNS.items() if rx.search(t)}

def has_compat_import(p: Path):
    try:
        return COMPAT_HEADER in p.read_text()
    except Exception:
        return False

def has_makefile_hookkit(p: Path):
    try:
        t = p.read_text()
        return bool(MAKE_FRAMEWORK_RE.search(t)), bool(MAKE_LOGOS_RE.search(t))
    except Exception:
        return False, False

def check(paths):
    overall = {}
    for p in paths:
        if not p.exists():
            continue
        if p.is_dir():
            continue
        counts = scan_file(p)
        if counts:
            overall[str(p)] = counts
            print(f"{p}:")
            for k, c in counts.items():
                print(f"  {k}: {c}")
        # makefile specifics
        if p.name.startswith("Makefile") or p.suffix == ".mk":
            has_fw, has_logos = has_makefile_hookkit(p)
            print(f"  HookKit framework in Makefile: {has_fw}")
            print(f"  hookkit generator in Makefile: {has_logos}")
        else:
            # source files
            if any(k in counts for k in ("MSHookFunction","MSHookMessageEx","MSHookMemory","substitute_hook","LHHookFunctions","LBHookMessage","LHPatchMemory")):
                print(f"  → needs HookKitCompat.h: {has_compat_import(p)}")
            if "%hook" in counts:
                print(f"  → needs generator=hookkit")
    if not overall:
        print("No Logos/Substrate patterns found.")
    return 0

def apply(paths):
    for p in paths:
        if not p.exists():
            print(f"skip {p} (not found)")
            continue
        if p.is_dir():
            continue
        text = p.read_text()
        orig = text

        # Makefile: add HookKit framework + generator
        if p.name.startswith("Makefile") or p.suffix == ".mk":
            if not MAKE_FRAMEWORK_RE.search(text):
                # append after first _FILES or at end
                text = text.rstrip() + f"\n{MAKE_FRAMEWORK}\n"
                print(f"{p}: added {MAKE_FRAMEWORK}")
            if not MAKE_LOGOS_RE.search(text):
                text = text.rstrip() + f"\n{MAKE_LOGOS}\n"
                print(f"{p}: added {MAKE_LOGOS}")
            if text != orig:
                p.write_text(text)
            continue

        # Source: add Compat import before first substrate/substitute/libhooker import, or after Foundation
        counts = scan_file(p)
        needs_compat = any(k in counts for k in ("MSHookFunction","MSHookMessageEx","MSHookMemory","substitute_hook","LHHookFunctions","LBHookMessage","LHPatchMemory"))
        if needs_compat and not has_compat_import(p):
            # Find insertion point
            inserted = False
            for rx in (PATTERNS["substrate.h"], PATTERNS["substitute.h"], PATTERNS["libhooker.h"]):
                m = rx.search(text)
                if m:
                    pos = m.start()
                    text = text[:pos] + COMPAT_IMPORT + "\n" + text[pos:]
                    print(f"{p}: inserted {COMPAT_IMPORT} before provider import")
                    inserted = True
                    break
            if not inserted:
                # fallback: after first #import <Foundation...>
                m = re.search(r'#\s*(import|include)\s*[<"]Foundation', text)
                if m:
                    end = text.find("\n", m.end()) + 1
                    text = text[:end] + COMPAT_IMPORT + "\n" + text[end:]
                    print(f"{p}: inserted {COMPAT_IMPORT} after Foundation")
                    inserted = True
            if not inserted:
                # top
                text = COMPAT_IMPORT + "\n" + text
                print(f"{p}: inserted {COMPAT_IMPORT} at top")
            p.write_text(text)
        elif needs_compat:
            print(f"{p}: already has {COMPAT_HEADER}")
        else:
            # pure %hook tweak — no source change needed
            if "%hook" in counts:
                print(f"{p}: pure Logos tweak — no source change, Makefile handles it")
    return 0

def revert(paths):
    for p in paths:
        if not p.exists():
            continue
        text = p.read_text()
        orig = text
        if p.name.startswith("Makefile") or p.suffix == ".mk":
            lines = [l for l in text.splitlines() if MAKE_FRAMEWORK not in l and "generator=hookkit" not in l]
            text = "\n".join(lines) + ("\n" if text.endswith("\n") else "")
            if text != orig:
                p.write_text(text)
                print(f"{p}: removed HookKit Makefile lines")
            continue
        if COMPAT_HEADER in text:
            # remove the import line we added
            text = re.sub(r'^.*HookKitCompat\.h.*\n?', '', text, flags=re.M)
            if text != orig:
                p.write_text(text)
                print(f"{p}: removed {COMPAT_HEADER}")
    return 0

def collect_all():
    out = []
    for pat in ("*.x", "*.xm", "*.m", "*.mm", "Makefile", "*.mk"):
        out.extend(Path(".").glob(pat))
        out.extend(Path(".").glob(f"**/{pat}"))
    # dedup, keep files
    seen = set()
    uniq = []
    for p in out:
        if p.is_file() and str(p) not in seen:
            seen.add(str(p))
            # skip .theos and vendor
            if ".theos" in str(p) or "vendor" in str(p) or ".git" in str(p):
                continue
            uniq.append(p)
    return uniq

def main():
    ap = argparse.ArgumentParser(description="Migrate tweak to HookKit")
    ap.add_argument("--check", action="store_true", help="report what would change")
    ap.add_argument("--apply", action="store_true", help="apply migration")
    ap.add_argument("--revert", action="store_true", help="revert migration")
    ap.add_argument("--all", action="store_true", help="scan cwd for Tweak files")
    ap.add_argument("paths", nargs="*", help="files to process")
    args = ap.parse_args()

    if not (args.check or args.apply or args.revert):
        args.check = True

    if sum((args.check, args.apply, args.revert)) > 1:
        ap.error("pick one of --check / --apply / --revert")

    paths = [Path(p) for p in args.paths] if args.paths else []
    if args.all or not paths:
        if args.all:
            paths = collect_all()
        elif not paths:
            ap.error("no paths given and --all not set")
    if args.check:
        return check(paths)
    if args.apply:
        return apply(paths)
    if args.revert:
        return revert(paths)

if __name__ == "__main__":
    sys.exit(main())
