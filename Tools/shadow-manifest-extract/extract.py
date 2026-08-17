#!/usr/bin/env python3
"""Extract shadow-hook-manifest.schema.json from a Shadow checkout.

Current scope (honest, not aspirational — see docs/3.0/IMPLEMENTATION_STATUS.md,
Milestone 2). Two layers, both from real parsed source, neither fabricated:

1. Install units: `kSHDWInstallUnits[]` in Shadow.framework/HookConfiguration.m
   — a genuine C array-of-structs the coordinator itself walks
   (HookCoordinator.m calls SHDWInstallUnits() to get it) — cross-referenced
   against `kSHDWCoordinatorInstallers[]` in ShadowCore.dylib/dylib.x (spec
   section 18.2's "Coordinator installer table") for the exact C function
   implementing each unit. extraction_method: structured_table.

2. Per-hook decomposition, best-effort: for every install function this
   script can locate and read, scans the body for direct
   `[hooks hookFunction:TARGET withReplacement:R outOldPtr:O]` call sites —
   HKSubstitutor's real function-hook API — and emits one child target per
   call, linked via `parent_install_unit`. Resolves the common
   dlsym-then-hook pattern (`sym = [hooks findSymbolInImage:...
   symbolName:@"..."]; if(sym) [hooks hookFunction:sym ...]`) back to the
   real symbol name, using the NEAREST preceding assignment to the variable,
   not just any assignment in the function — files like mach.x reuse one
   `sym` variable across 7 consecutive resolve-then-hook steps, and the
   naive "first assignment anywhere" version of this produced duplicate
   stable_hook_ids that validate.py caught for real; see
   test_reused_variable_resolves_to_nearest_preceding_assignment.
   extraction_method: pattern_scan (real and mechanical, weaker guarantee
   than a compiler-grade parse — see the schema's own description of the
   confidence ordering between structured_table/static_ast/pattern_scan).

What this does NOT do yet: units that delegate to `shdw_libc_install_group`
(4 files sharing one big descriptor/switch in libc.x, not yet parsed),
Logos `%hook`/`%init` blocks (7+ files confirmed to still use them), or
non-`hookFunction:` HKSubstitutor calls (`hookMessage:`, batching) yield
zero pattern_scan children and stay unit-level, with an explicit note in
that unit's known_compatibility_risks saying so — never silently implied
as covered. Needs logos_preprocess.py / clang_ast_extract.py (not built) or
a targeted structured-table parse of shdw_libc_install_group's own table.

required_reach / original_requirement / commit_domain for install-unit rows
are reasoned defaults derived from the real capability/phase enums (see the
mapping tables below, each with its citation), not verified per-hook facts
— deliberately conservative, meant to be tightened as decomposition deepens.
Child (pattern_scan) rows are closer to verified: required_reach reflects
whether the symbol was resolved via runtime dlsym vs. a bare import, and
original_requirement reflects whether the real call actually passed
outOldPtr: or not. manual_overrides.yaml corrects either kind without
re-deriving this script's output.
"""

import argparse
import json
import os
import re
import sys
from datetime import datetime, timezone

MANIFEST_VERSION = "0.2.0-milestone2-partial-decomposition"

# Phase -> commit_domain. Freeform string in the schema; using the real
# enum name (HookConfiguration.h SHDWLifecyclePhase) rather than inventing
# new domain names not backed by source.
PHASE_DOMAIN = {
    "SHDWPhaseAlways": "phase_always",
    "SHDWPhaseTier1": "phase_tier1",
    "SHDWPhaseTier2": "phase_tier2",
    "SHDWPhaseUIKit": "phase_uikit",
    "SHDWPhaseEscalation": "phase_escalation",
}

# Capability -> (target_kind guess, required_reach). Cited from
# HookConfiguration.h's SHDWCapabilityKind doc comments:
#   None    = "identity group, no backend requirement"
#   Message = "ObjC-method swizzle (subMain)"
#   Function = "C-function rebind (subCFunc)"
#   Symlookup = "inline-first, rebind fallback (dlsym/dladdr pair)"
#   PrivateSym = "private-symbol-capable backend (dlopen_internal)"
CAPABILITY_MAP = {
    "SHDWCapabilityNone": (None, []),
    "SHDWCapabilityMessage": ("objc_method", ["objc_dispatch"]),
    "SHDWCapabilityFunction": ("function_symbol", ["existing_imports"]),
    "SHDWCapabilitySymlookup": ("function_symbol", ["existing_imports", "private_address"]),
    "SHDWCapabilityPrivateSym": ("function_symbol", ["private_address"]),
}

ROW_RE = re.compile(r"\{([^{}]*)\}")


def strip_comments_preserve_lines(text: str) -> str:
    """Blank out comments and string contents' comment-like substrings are a
    non-issue here (C identifiers/strings in this file don't contain // or
    /* ), but real strings ARE preserved verbatim -- only genuine // and
    /* */ comments are blanked, with newlines kept so line numbers of
    everything after a comment stay correct. Learned the hard way earlier
    this session: a naive regex over raw source matched "%hook" inside a
    plain comment sentence. Do it properly here instead."""
    out = []
    i = 0
    n = len(text)
    in_string = False
    while i < n:
        c = text[i]
        if in_string:
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(text[i + 1])
                i += 2
                continue
            if c == '"':
                in_string = False
            i += 1
            continue
        if c == '"':
            in_string = True
            out.append(c)
            i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                out.append(" ")
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            out.append("  ")
            i += 2
            while i < n and not (text[i] == "*" and i + 1 < n and text[i + 1] == "/"):
                out.append(" " if text[i] != "\n" else "\n")
                i += 1
            out.append("  ")
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def parse_row_fields(row_body: str):
    """Split 'a, b, c' at top-level commas (none of these rows nest commas
    inside parens/braces further, but stay defensive rather than assume)."""
    fields, depth, cur = [], 0, []
    for c in row_body:
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        if c == "," and depth == 0:
            fields.append("".join(cur).strip())
            cur = []
        else:
            cur.append(c)
    if cur:
        fields.append("".join(cur).strip())
    return fields


def parse_string_literal(field: str):
    field = field.strip()
    m = re.match(r'^"((?:[^"\\]|\\.)*)"$', field)
    if not m:
        raise ValueError(f"expected a quoted string literal, got: {field!r}")
    return m.group(1)


def parse_pref_key(field: str):
    """prefKey is `NULL` or a bare NSString* const identifier (e.g.
    SHDWHookIDFilesystem, defined elsewhere as an extern constant) -- not a
    string literal in the table itself. Returns the identifier name, not its
    (unresolved) string value; see the module docstring's honesty notes."""
    field = field.strip()
    if field in ("NULL", "nil"):
        return None
    if re.match(r'^"((?:[^"\\]|\\.)*)"$', field):
        return parse_string_literal(field)
    if re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", field):
        return field
    raise ValueError(f"expected NULL, a string literal, or an identifier, got: {field!r}")


def find_balanced_braces(clean_text: str, open_paren_idx: int):
    """Given the index of an opening '{' (already matched), return the index
    just past its matching '}'. Depth-aware, comment/string already stripped
    by the caller so this only has to count braces."""
    depth = 1
    i = open_paren_idx + 1
    n = len(clean_text)
    while depth > 0 and i < n:
        if clean_text[i] == "{":
            depth += 1
        elif clean_text[i] == "}":
            depth -= 1
        i += 1
    return i


def find_c_array_body(clean_text: str, array_name: str, source_label: str):
    """Locate `<array_name>[...] = { ... };` in already comment-stripped
    text and return (body_text, body_start_line). Shared by every table this
    script parses -- kSHDWInstallUnits, kSHDWCoordinatorInstallers, and any
    future one, rather than re-deriving the same depth-count/line-tracking
    logic per table."""
    m = re.search(rf"\b{re.escape(array_name)}\s*\[\s*\]\s*=\s*\{{", clean_text)
    if not m:
        raise RuntimeError(
            f"{array_name}[] not found in {source_label} -- table was "
            "moved/renamed, this extractor needs updating, not the "
            "manifest silently going stale"
        )
    open_idx = m.end() - 1
    end_idx = find_balanced_braces(clean_text, open_idx)
    body = clean_text[open_idx + 1:end_idx - 1]
    body_start_line = clean_text.count("\n", 0, open_idx + 1) + 1
    return body, body_start_line


def extract_install_units(config_m_path: str):
    with open(config_m_path, "r", encoding="utf-8") as f:
        raw = f.read()
    return parse_install_units_from_text(raw, source_label=config_m_path)


def parse_install_units_from_text(raw: str, source_label: str = "<text>"):
    clean = strip_comments_preserve_lines(raw)
    body, body_start_line = find_c_array_body(clean, "kSHDWInstallUnits", source_label)

    units = []
    for row_match in ROW_RE.finditer(body):
        row_line = body_start_line + body.count("\n", 0, row_match.start())
        fields = parse_row_fields(row_match.group(1))
        if len(fields) != 6:
            raise ValueError(
                f"{source_label}:{row_line}: expected 6 fields "
                f"(unitID, prefKey, phase, capability, ctorInstall, verify), "
                f"got {len(fields)}: {fields!r}"
            )
        unit_id, pref_key, phase, capability, ctor_install, verify = fields
        units.append({
            "unit_id": parse_string_literal(unit_id),
            "pref_key": parse_pref_key(pref_key),
            "phase": phase.strip(),
            "capability": capability.strip(),
            "ctor_install": ctor_install.strip() == "1",
            "verify": verify.strip() == "1",
            "source_line": row_line,
        })
    return units


def parse_coordinator_installers_from_text(raw: str, source_label: str = "<text>"):
    """Parses kSHDWCoordinatorInstallers[] (ShadowCore.dylib/dylib.x) -- the
    OTHER real structured table, explicitly named in spec section 18.2 as
    "Coordinator installer table". Its own source comment says it "must stay
    in SHDWInstallUnits() order" and its struct (SHDWHookInstaller,
    HookCoordinator.h) is `{ const char* unitID, installer_fn, verify_fn }` --
    giving the exact C function name that implements each install unit,
    mechanically, instead of guessing from a filename convention."""
    clean = strip_comments_preserve_lines(raw)
    body, body_start_line = find_c_array_body(clean, "kSHDWCoordinatorInstallers", source_label)

    installers = {}
    for row_match in ROW_RE.finditer(body):
        row_line = body_start_line + body.count("\n", 0, row_match.start())
        fields = parse_row_fields(row_match.group(1))
        if len(fields) != 3:
            raise ValueError(
                f"{source_label}:{row_line}: expected 3 fields "
                f"(unitID, installFn, verifyFn), got {len(fields)}: {fields!r}"
            )
        unit_id_f, install_fn, verify_fn = fields
        unit_id = parse_string_literal(unit_id_f)
        installers[unit_id] = {
            "install_fn": install_fn.strip(),
            "verify_fn": None if verify_fn.strip() in ("NULL", "nil") else verify_fn.strip(),
            "source_line": row_line,
        }
    return installers


# Matches "[hooks hookFunction:" specifically (not any bracket expression) --
# a deliberate precision-over-recall choice: every direct call this session
# found uses exactly this receiver name (the HKSubstitutor* hooks parameter),
# and matching only that avoids false positives on unrelated bracket sends.
# Extend if a real file turns up using a differently-named receiver.
HOOK_FUNCTION_CALL_RE = re.compile(r"\[\s*hooks\s+hookFunction\s*:")


def find_hook_function_calls(body_text: str, body_start_line: int):
    """Scans an already comment-stripped function body for
    `[hooks hookFunction:TARGET withReplacement:REPL outOldPtr:OUT]` sends --
    HKSubstitutor's real function-hook API (Headers/HookKit.h) -- and returns
    one dict per call. Depth-aware from the opening '[' to its matching ']',
    so multi-line calls and nested `(void **)&x` casts inside outOldPtr: are
    handled correctly, not just single-line ones."""
    calls = []
    for m in HOOK_FUNCTION_CALL_RE.finditer(body_text):
        open_idx = body_text.rfind("[", 0, m.start() + 1)
        depth = 1
        i = open_idx + 1
        n = len(body_text)
        while depth > 0 and i < n:
            if body_text[i] == "[":
                depth += 1
            elif body_text[i] == "]":
                depth -= 1
            i += 1
        call_text = body_text[open_idx + 1:i - 1]
        line = body_start_line + body_text.count("\n", 0, open_idx)

        # call_text is now: hooks hookFunction:X withReplacement:Y outOldPtr:Z
        parts = re.split(r"\bhookFunction\s*:|\bwithReplacement\s*:|\boutOldPtr\s*:", call_text)
        # parts[0] is "hooks "; parts[1..3] are the three argument expressions
        # when all three keyword args are present (outOldPtr: is optional in
        # the real API -- a 2-part split means no outOldPtr: was passed).
        target_expr = parts[1].strip() if len(parts) > 1 else None
        replacement_expr = parts[2].strip() if len(parts) > 2 else None
        outptr_expr = parts[3].strip() if len(parts) > 3 else None

        calls.append({
            "target_expr": target_expr,
            "replacement_expr": replacement_expr,
            "outptr_expr": outptr_expr,
            "line": line,
            "body_offset": open_idx,
        })
    return calls


def resolve_symbol_variable(func_body: str, var_name: str, before_offset: int):
    """If `var_name` was assigned from
    `[hooks findSymbolInImage:IMG symbolName:@"REAL_NAME"]` earlier in the
    same function body, return REAL_NAME -- resolves the
    dlsym-at-runtime-then-hook pattern seen in iokit.x/mach.x/syscall.x back
    to the actual symbol it targets, instead of leaving it as an opaque
    local variable name.

    Takes the LAST such assignment strictly before `before_offset` (the
    call site's own position), not just the first one anywhere in the body:
    mach.x reuses a single `sym` variable for seven consecutive
    resolve-then-hook steps, so "first match in the whole function" resolved
    every one of them to the first symbol -- a real bug this session hit and
    validate.py caught as duplicate stable_hook_ids."""
    pattern = re.compile(
        rf'\b{re.escape(var_name)}\s*=\s*\[\s*hooks\s+findSymbolInImage\s*:[^\]]*?symbolName\s*:\s*@"([^"]+)"'
    )
    last = None
    for m in pattern.finditer(func_body):
        if m.start() >= before_offset:
            break
        last = m
    return last.group(1) if last else None


def find_function_body(clean_text: str, func_name: str):
    """Locates `<func_name>(...) { ... }` (a definition, not just a
    declaration/prototype -- requires the `{`) and returns (body_text,
    body_start_line), or None if not found in this file."""
    m = re.search(rf"\b{re.escape(func_name)}\s*\([^;{{]*\)\s*\{{", clean_text)
    if not m:
        return None
    open_idx = m.end() - 1
    end_idx = find_balanced_braces(clean_text, open_idx)
    body = clean_text[open_idx + 1:end_idx - 1]
    body_start_line = clean_text.count("\n", 0, open_idx + 1) + 1
    return body, body_start_line


def unit_to_manifest_target(unit, source_file_rel):
    target_kind, required_reach = CAPABILITY_MAP.get(unit["capability"], (None, []))
    domain = PHASE_DOMAIN.get(unit["phase"], unit["phase"].lower())

    target = {
        "stable_hook_id": unit["unit_id"],
        "source": {"file": source_file_rel, "line": unit["source_line"]},
        "role": "mandatory" if unit["pref_key"] is None else "optional",
        "commit_domain": domain,
        "required_reach": required_reach,
        # Reasoned default, not verified: Shadow's hooks filter/modify
        # results rather than replace behavior outright, and the coordinator
        # builds its substitutors from HK_CAT_FUNCTION_REBIND/HK_CAT_MESSAGE
        # auto-cover (HookCoordinator.m) -- both direct-predecessor-oriented
        # lanes in HookKit's own model. Needs per-hook verification before
        # Milestone 3 freeze; override in manual_overrides.yaml if wrong for
        # a specific unit.
        "original_requirement": "direct_predecessor" if target_kind else "none",
        "continuation_policy": "any",
        "availability": "required_now" if unit["ctor_install"] else "defer_until_available",
        "extraction_method": "structured_table",
        "known_compatibility_risks": [
            "unit-level only: does not yet decompose into the individual "
            "function/selector targets this install unit installs"
        ],
    }
    if target_kind:
        target["target_kind"] = target_kind
    else:
        # SHDWCapabilityNone doesn't appear in the current table but is a
        # real enum value (HookConfiguration.h) -- represent it honestly
        # rather than guessing a target_kind that isn't backed by source.
        target["target_kind"] = "function_symbol"
        target["known_compatibility_risks"].append(
            "capability=SHDWCapabilityNone: no target_kind is implied by "
            "source, function_symbol used as a schema-valid placeholder"
        )
    if unit["pref_key"]:
        target["known_compatibility_risks"].append(f"pref-gated by {unit['pref_key']}")
    if unit["verify"]:
        target["verification_method"] = "shdw verify function (ctor-run, see HookConfiguration.h SHDWInstallUnit.verify)"
    return target


IDENTIFIER_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def hook_call_to_manifest_target(call, parent_id, parent_domain, source_file_rel, func_body):
    """One [hooks hookFunction:...] call site -> one child manifest target.
    target_expr is usually a bare C identifier (a real exported symbol, e.g.
    `sandbox_check`) but is sometimes a local variable holding a
    dlsym-resolved pointer -- resolve_symbol_variable() traces those back to
    the real symbol name where the pattern is the one this session actually
    found (findSymbolInImage:...symbolName:@"...")."""
    target_expr = call["target_expr"]
    risks = []
    symbol = None
    reach = ["existing_imports"]

    if target_expr and IDENTIFIER_RE.match(target_expr):
        resolved = resolve_symbol_variable(func_body, target_expr, call["body_offset"])
        if resolved:
            symbol = resolved
            reach = ["existing_imports", "private_address"]
            risks.append(f"resolved at runtime via findSymbolInImage (local var {target_expr!r})")
        else:
            symbol = target_expr
    else:
        risks.append(f"target expression not a bare identifier, needs manual review: {target_expr!r}")

    stable_id = f"{parent_id}::{symbol or target_expr or '?'}"
    target = {
        "stable_hook_id": stable_id,
        "parent_install_unit": parent_id,
        "source": {"file": source_file_rel, "line": call["line"]},
        "target_kind": "function_symbol",
        "role": "mandatory",  # inherits the parent unit's own role; not independently pref-gated
        "commit_domain": parent_domain,  # installs alongside its parent unit
        "required_reach": reach,
        # outOldPtr: present in the call -> the hook keeps a callable original.
        "original_requirement": "direct_predecessor" if call["outptr_expr"] else "none",
        "continuation_policy": "any",
        "availability": "required_now",
        "extraction_method": "pattern_scan",
        "known_compatibility_risks": risks,
    }
    if symbol:
        target["target_symbol"] = symbol
    return target


def find_function_definition_file(shadow_repo: str, func_name: str):
    """Which file under ShadowCore.dylib/hooks/**/* defines `func_name`?
    Greps rather than assumes a filename convention (shadowhook_iokit does
    live in iokit.x, but nothing guarantees that in general, and dyld.x
    already breaks a naive convention by having its real entry point be a
    dyld callback, not a shadowhook_dyld wrapper -- see the caller)."""
    hooks_dir = os.path.join(shadow_repo, "ShadowCore.dylib", "hooks")
    pattern = re.compile(rf"^\s*[\w\*\s]+\b{re.escape(func_name)}\s*\(")
    for root, _dirs, files in os.walk(hooks_dir):
        for fn in files:
            if not fn.endswith((".x", ".m", ".c")):
                continue
            path = os.path.join(root, fn)
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                text = f.read()
            for line in text.splitlines():
                if pattern.match(line):
                    return path
    return None


def load_manual_overrides(path):
    if not os.path.exists(path):
        return {}
    try:
        import yaml
    except ImportError:
        print(f"warning: {path} exists but PyYAML isn't installed -- "
              "overrides NOT applied", file=sys.stderr)
        return {}
    with open(path, "r", encoding="utf-8") as f:
        data = yaml.safe_load(f) or {}
    return data.get("overrides", {}) or {}


def apply_overrides(targets, overrides):
    applied = []
    for t in targets:
        ov = overrides.get(t["stable_hook_id"])
        if not ov:
            continue
        t.update(ov)
        t["extraction_method"] = "manual_override"
        applied.append(t["stable_hook_id"])
    return applied


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--shadow-repo", default=os.environ.get("SHADOW_REPO"),
                     help="path to a shadow checkout (default: $SHADOW_REPO, "
                          "then ../../shadow relative to this script)")
    ap.add_argument("--out", default=None,
                     help="output path (default: artifacts/shadow-current-manifest.json "
                          "under this script's HookKit repo root)")
    args = ap.parse_args()

    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    shadow_repo = args.shadow_repo or os.path.join(repo_root, "..", "shadow")
    shadow_repo = os.path.abspath(shadow_repo)
    config_m = os.path.join(shadow_repo, "Shadow.framework", "HookConfiguration.m")

    if not os.path.exists(config_m):
        print(f"error: {config_m} not found -- pass --shadow-repo or set SHADOW_REPO",
              file=sys.stderr)
        return 1

    units = extract_install_units(config_m)
    source_rel = os.path.join("Shadow.framework", "HookConfiguration.m")
    targets = [unit_to_manifest_target(u, source_rel) for u in units]
    target_by_id = {t["stable_hook_id"]: t for t in targets}

    # Cross-reference the coordinator installer table (dylib.x) -- the OTHER
    # real structured table named in spec section 18.2 ("Coordinator
    # installer table"). Gives the exact C function per unit mechanically,
    # instead of guessing from a filename convention, and doubles as a
    # cross-check: the source's own comment says the two tables "must stay
    # in SHDWInstallUnits() order," so a mismatch here is a real finding.
    dylib_x = os.path.join(shadow_repo, "ShadowCore.dylib", "dylib.x")
    installers = {}
    if os.path.exists(dylib_x):
        with open(dylib_x, "r", encoding="utf-8") as f:
            installers = parse_coordinator_installers_from_text(f.read(), source_label=dylib_x)
    else:
        print(f"warning: {dylib_x} not found -- current_implementation left "
              "unset, no per-hook decomposition attempted", file=sys.stderr)

    missing_installer = [uid for uid in target_by_id if uid not in installers]
    missing_unit = [uid for uid in installers if uid not in target_by_id]
    if missing_installer:
        print(f"warning: {len(missing_installer)} install unit(s) have no "
              f"matching coordinator installer row: {missing_installer}", file=sys.stderr)
    if missing_unit:
        print(f"warning: coordinator installer table has {len(missing_unit)} "
              f"unit(s) not in kSHDWInstallUnits: {missing_unit}", file=sys.stderr)

    # Per-hook decomposition, best-effort: for every unit whose install
    # function we can locate and read, scan its body for direct
    # [hooks hookFunction:...] call sites (pattern_scan, see the function
    # doc). Units that delegate elsewhere (shdw_libc_install_group, Logos
    # %init) or use a differently-shaped API (hookMessage:, etc.) yield zero
    # calls here and stay unit-level -- said explicitly in their risks list,
    # not left silently unexplained.
    child_targets = []
    decomposed = []
    for unit_id, installer in installers.items():
        t = target_by_id.get(unit_id)
        if not t:
            continue
        t["current_implementation"] = installer["install_fn"]

        def_path = find_function_definition_file(shadow_repo, installer["install_fn"])
        if not def_path:
            t["known_compatibility_risks"].append(
                f"could not locate a definition for {installer['install_fn']}() "
                "under ShadowCore.dylib/hooks -- may live outside that tree"
            )
            continue
        with open(def_path, "r", encoding="utf-8") as f:
            file_clean = strip_comments_preserve_lines(f.read())
        found = find_function_body(file_clean, installer["install_fn"])
        rel = os.path.relpath(def_path, shadow_repo)
        if not found:
            continue
        body, body_start_line = found
        calls = find_hook_function_calls(body, body_start_line)

        t["known_compatibility_risks"] = [
            r for r in t["known_compatibility_risks"] if not r.startswith("unit-level only")
        ]
        if calls:
            for call in calls:
                child_targets.append(hook_call_to_manifest_target(
                    call, unit_id, t["commit_domain"], rel, body))
            decomposed.append(unit_id)
            t["known_compatibility_risks"].append(
                f"decomposed into {len(calls)} pattern_scan child target(s) via "
                f"[hooks hookFunction:...] scan of {installer['install_fn']}() in {rel}"
            )
        else:
            t["known_compatibility_risks"].append(
                f"no [hooks hookFunction:...] call sites found in "
                f"{installer['install_fn']}() ({rel}) -- likely delegates to a "
                "descriptor table, shdw_libc_install_group, or a Logos %hook/"
                "%init block; needs a follow-up pass to classify, not yet done"
            )

    targets.extend(child_targets)

    overrides_path = os.path.join(os.path.dirname(__file__), "manual_overrides.yaml")
    overrides = load_manual_overrides(overrides_path)
    applied = apply_overrides(targets, overrides)

    shadow_commit = None
    head_path = os.path.join(shadow_repo, ".git", "HEAD")
    if os.path.exists(head_path):
        try:
            import subprocess
            shadow_commit = subprocess.check_output(
                ["git", "-C", shadow_repo, "rev-parse", "HEAD"],
                text=True).strip()
        except Exception:
            pass

    manifest = {
        "manifest_version": MANIFEST_VERSION,
        "generated_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "shadow_commit": shadow_commit,
        "targets": targets,
    }

    out_path = args.out or os.path.join(repo_root, "artifacts", "shadow-current-manifest.json")
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")

    print(f"wrote {len(targets)} targets ({len(units)} install units, "
          f"{len(child_targets)} pattern_scan child targets from "
          f"{len(decomposed)} decomposed unit(s)) to {out_path}")
    print(f"  ({len(applied)} overridden by manual_overrides.yaml: {applied})" if applied
          else "  (no manual overrides applied)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
