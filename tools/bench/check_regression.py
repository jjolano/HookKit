#!/usr/bin/env python3
# check_regression.py — compare current bench JSON lines against baselines.
# ponytail: stdlib only, threshold 10% default.
import sys, json, pathlib

THRESHOLD = 1.10  # 10% regression

def load_json_lines(path):
    entries = {}
    try:
        with open(path) as f:
            for line in f:
                line=line.strip()
                if not line.startswith("{"): continue
                try:
                    j=json.loads(line)
                    if "bench" in j and "mean" in j:
                        entries[j["bench"]] = j
                except: pass
    except FileNotFoundError:
        return None
    return entries

def load_maybe(path):
    if not pathlib.Path(path).exists():
        print(f"skip {path}: not found (run bench first)")
        return None
    # try json lines then single json
    entries = load_json_lines(path)
    if entries and len(entries)>0:
        return entries
    try:
        with open(path) as f:
            data=json.load(f)
            if isinstance(data, dict):
                return data
    except: pass
    return entries

def check(baseline_path):
    base = load_maybe(baseline_path)
    if base is None:
        return True
    # Current is expected under .theos/bench; only baseline presence is checked for now.
    print(f"baseline {baseline_path}: {len(base)} entries")
    for k,v in sorted(base.items()):
        print(f"  {k}: mean {v.get('mean',0):.0f} {v.get('unit','')}")
    return True

ok=True
for p in sys.argv[1:]:
    if not check(p):
        ok=False
if ok:
    print("check_regression: OK (threshold check TODO when current vs baseline both present)")
sys.exit(0 if ok else 1)
