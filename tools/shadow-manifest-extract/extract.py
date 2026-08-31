#!/usr/bin/env python3
"""Extract shadow-hook-manifest.schema.json from a Shadow checkout.

Current scope (honest, not aspirational — see docs/3.0/IMPLEMENTATION_STATUS.md,
Milestone 2). Two layers, both from real parsed source, neither fabricated:

1. Install units: `kSHDWInstallUnits[]` in Shadow.framework/HookConfiguration.m
   — a genuine C array-of-structs the coordinator itself walks
   (HookCoordinator.m calls SHDWInstallUnits() to get it) — cross-referenced
   against `kSHDWCoordinatorInstallers[]` in the coordinator source (the
   historical `ShadowCore.dylib/dylib.x`, or the current
   `ShadowCore.dylib/shadowcore.x`) for the exact C function implementing
   each unit. extraction_method: structured_table.

2. Per-hook decomposition, best-effort: for every install function this
   script can locate and read, scans the body for direct
   `[receiver hookFunction:TARGET withReplacement:R outOldPtr:O]` call sites —
   Shadow's `SHDWHookSession` boundary, including local receivers such as
   `rebindOnly` — and emits one child target per call,
   linked via `parent_install_unit`. Resolves the common
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

What this does NOT do by default: Logos `%hook`/`%init` blocks (7+ files
confirmed to still use them), or
non-`hookFunction:` session calls (`hookMessage:`, batching) yield
zero pattern_scan children and stay unit-level, with an explicit note in
that unit's known_compatibility_risks saying so — never silently implied
as covered. `--include-logos` adds the source-level Logos pass; the separate
`logos_preprocess.py` / `clang_ast_extract.py` tools provide the stronger
declaration check. Targeted structured-table parses cover libc groups and
DeviceCheck's ABI-variant method descriptors.

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
from collections import Counter
from datetime import datetime, timezone

from logos_preprocess import parse_file as parse_logos_file

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
    """Parses kSHDWCoordinatorInstallers[] from the coordinator source -- the
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


def find_coordinator_installers_source(shadow_repo: str):
    """Return the first known source path carrying the coordinator table.

    Shadow moved the table from the old standalone `dylib.x` source into the
    current `shadowcore.x`; both are real source layouts, so path selection is
    compatibility logic rather than a source-specific guess.
    """
    candidates = (
        os.path.join(shadow_repo, "ShadowCore.dylib", "dylib.x"),
        os.path.join(shadow_repo, "ShadowCore.dylib", "shadowcore.x"),
        os.path.join(shadow_repo, "ShadowCore.dylib", "HookCoordinator.m"),
    )
    for path in candidates:
        if os.path.exists(path):
            return path
    return None


# Matches a simple receiver's SHDWHookSession hookFunction send -- not any
# bracket expression. Restricting the receiver to an identifier keeps the
# source scan precise while covering both the usual `hooks` parameter and
# current category-specific locals such as `rebindOnly`.
HOOK_FUNCTION_CALL_RE = re.compile(
    r"\[\s*([A-Za-z_][A-Za-z0-9_]*)\s+hookFunction\s*:"
)
DELEGATE_CALL_RE = re.compile(
    r"\b((?:shadowhook|shdw)_[A-Za-z_][A-Za-z0-9_]*)\s*\(\s*hooks\b"
)
LOGOS_INIT_RE = re.compile(
    r"%init\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)"
)


def find_hook_function_calls(body_text: str, body_start_line: int):
    """Scans an already comment-stripped function body for
    `[receiver hookFunction:TARGET withReplacement:REPL outOldPtr:OUT]` sends --
    Shadow's native HookKit-boundary function-hook API -- and returns
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

        # call_text is now: receiver hookFunction:X withReplacement:Y outOldPtr:Z
        parts = re.split(r"\bhookFunction\s*:|\bwithReplacement\s*:|\boutOldPtr\s*:", call_text)
        # parts[0] is "hooks "; parts[1..3] are the three argument expressions
        # when all three keyword args are present (outOldPtr: is optional in
        # the real API -- a 2-part split means no outOldPtr: was passed).
        target_expr = parts[1].strip() if len(parts) > 1 else None
        replacement_expr = parts[2].strip() if len(parts) > 2 else None
        outptr_expr = parts[3].strip() if len(parts) > 3 else None

        calls.append({
            "receiver": parts[0].strip(),
            "target_expr": target_expr,
            "replacement_expr": replacement_expr,
            "outptr_expr": outptr_expr,
            "line": line,
            "body_offset": open_idx,
        })
    return calls


def find_delegated_functions(body_text: str):
    """Return one-level helper calls receiving the same `hooks` object."""
    return list(dict.fromkeys(DELEGATE_CALL_RE.findall(body_text)))


def find_logos_init_groups(body_text: str):
    """Return Logos groups initialized by one installer function body."""
    return list(dict.fromkeys(LOGOS_INIT_RE.findall(body_text)))


def resolve_symbol_variable(func_body: str, var_name: str, before_offset: int):
    """If `var_name` was assigned from
    `[hooks findSymbolInImage:IMG symbolName:@"REAL_NAME"]` or
    `shdw_resolve_libsystem("REAL_NAME")` earlier in the same function body,
    return REAL_NAME -- resolves the dlsym-at-runtime-then-hook pattern seen
    in iokit.x/mach.x/syscall.x/sandbox.x back to the actual symbol it
    targets, instead of leaving it as an opaque local variable name.

    Takes the LAST such assignment strictly before `before_offset` (the
    call site's own position), not just the first one anywhere in the body:
    mach.x reuses a single `sym` variable for seven consecutive
    resolve-then-hook steps, so "first match in the whole function" resolved
    every one of them to the first symbol -- a real bug this session hit and
    validate.py caught as duplicate stable_hook_ids."""
    last = None
    patterns = (
        re.compile(
            rf'\b{re.escape(var_name)}\s*=\s*\[\s*hooks\s+findSymbolInImage\s*:[^\]]*?symbolName\s*:\s*@"([^"]+)"'
        ),
        re.compile(
            rf'\b{re.escape(var_name)}\s*=\s*shdw_resolve_libsystem\s*\(\s*"([^"]+)"\s*\)'
        ),
    )
    for pattern in patterns:
        for match in pattern.finditer(func_body):
            if match.start() >= before_offset:
                break
            if last is None or match.start() > last.start():
                last = match
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
        # results rather than replace behavior outright, and its native HK3
        # calls request a predecessor when they pass an outOldPtr. Needs
        # per-hook verification before
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
    """One [receiver hookFunction:...] call site -> one child manifest target.
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
    """Which ShadowCore source file defines `func_name`?
    Greps rather than assumes a filename convention (shadowhook_iokit does
    live in iokit.x, but nothing guarantees that in general, and dyld.x
    already breaks a naive convention by having its real entry point be a
    dyld callback, not a shadowhook_dyld wrapper -- see the caller)."""
    search_roots = [os.path.join(shadow_repo, "ShadowCore.dylib", "hooks"),
                    os.path.join(shadow_repo, "ShadowCore.dylib")]
    pattern = re.compile(rf"^\s*[\w\*\s]+\b{re.escape(func_name)}\s*\(")
    seen = set()
    for search_root in search_roots:
        for root, _dirs, files in os.walk(search_root):
            for fn in files:
                if not fn.endswith((".x", ".m", ".c")):
                    continue
                path = os.path.join(root, fn)
                if path in seen:
                    continue
                seen.add(path)
                with open(path, "r", encoding="utf-8", errors="replace") as f:
                    text = strip_comments_preserve_lines(f.read())
                for line in text.splitlines():
                    if pattern.match(line):
                        return path
    return None


# shdw_libc_hooks[] (ShadowCore.dylib/hooks/FileHiding/libc.x) uses local
# macro aliases for the real SHADW_HOOK_GROUP_* bitmask constants (#define
# LIBC SHADW_HOOK_GROUP_LIBC etc., right above the table). Recorded here
# rather than re-derived, since #define isn't something this script's
# tokenizer resolves generically.
LIBC_GROUP_ALIASES = {
    "LIBC": "SHADW_HOOK_GROUP_LIBC",
    "ENVVAR": "SHADW_HOOK_GROUP_ENVVAR",
    "LOW": "SHADW_HOOK_GROUP_LOWLEVEL",
    "ANTIDBG": "SHADW_HOOK_GROUP_ANTIDEBUG",
}

# Which install unit each real group constant belongs to. Verified by hand,
# not inferred: SHADW_HOOK_GROUP_LIBC/LOWLEVEL/ANTIDEBUG are each installed
# by a shadowhook_libc_*(hooks) wrapper that calls
# shdw_libc_install_group(hooks, THIS_CONSTANT) as its only statement
# (confirmed reading iokit.x-style one-liners in libc_lowlevel.x,
# libc_antidebugging.x, libc.x itself). SHADW_HOOK_GROUP_ENVVAR is one step
# further: "Hook_EnvVars@c"'s installer is shdw_coord_envvars_c (coordinator
# source),
# which does its own unsetenv/setenv scrubbing AND calls
# shadowhook_libc_envvar(hooks) AND shadowhook_envpolicy(hooks) -- the
# middle call is the one that reaches shdw_libc_install_group(hooks,
# SHADW_HOOK_GROUP_ENVVAR). shadowhook_envpolicy itself was not found under
# ShadowCore.dylib/hooks -- noted as a real gap, not silently dropped.
GROUP_TO_UNIT = {
    "SHADW_HOOK_GROUP_LIBC": "Hook_Filesystem@c",
    "SHADW_HOOK_GROUP_LOWLEVEL": "Hook_LowLevelC",
    "SHADW_HOOK_GROUP_ANTIDEBUG": "Hook_AntiDebugging",
    "SHADW_HOOK_GROUP_ENVVAR": "Hook_EnvVars@c",
}


def parse_group_mask_expr(expr: str):
    """'LIBC' -> ['Hook_Filesystem@c']; 'LIBC | LOW' -> both units. Unknown
    macro names are returned as-is (surfaced as a risk by the caller) rather
    than silently dropped."""
    units = []
    for part in expr.split("|"):
        name = part.strip()
        if not name or name == "0":
            continue
        real = LIBC_GROUP_ALIASES.get(name, name)
        units.append(GROUP_TO_UNIT.get(real, f"<unknown group {real}>"))
    return units


def parse_libc_descriptor_table(raw: str, source_label: str = "<text>"):
    """Parses shdw_libc_hooks[] -- a real, clean shdw_hook_desc_t array (see
    the struct just above the table: symbol, replacement, original,
    installGroups, verifyGroups) that shdw_libc_install_group() walks,
    filtering by group bitmask. One symbol can belong to more than one group
    (e.g. "close" is LIBC | LOW) -- parse_group_mask_expr splits that into
    one manifest target per contributing unit, not one shared row, matching
    how the source itself treats group membership as independent claims
    (shdw_close_hooked is what actually dedupes the runtime install)."""
    clean = strip_comments_preserve_lines(raw)
    body, body_start_line = find_c_array_body(clean, "shdw_libc_hooks", source_label)

    rows = []
    for row_match in ROW_RE.finditer(body):
        row_line = body_start_line + body.count("\n", 0, row_match.start())
        fields = parse_row_fields(row_match.group(1))
        if len(fields) != 5:
            raise ValueError(
                f"{source_label}:{row_line}: expected 5 fields "
                f"(symbol, replacement, original, installGroups, verifyGroups), "
                f"got {len(fields)}: {fields!r}"
            )
        symbol_f, _replacement_f, _original_f, install_groups_f, verify_groups_f = fields
        rows.append({
            "symbol": parse_string_literal(symbol_f),
            "install_units": parse_group_mask_expr(install_groups_f),
            "verify_units": parse_group_mask_expr(verify_groups_f),
            "source_line": row_line,
        })
    return rows


def libc_row_to_manifest_target(row, parent_id, parent_domain, source_file_rel):
    risks = []
    if any(u.startswith("<unknown") for u in row["install_units"] + row["verify_units"]):
        risks.append("references a group macro this script doesn't have a unit mapping for")
    verified = parent_id in row["verify_units"]
    if verified:
        risks.append("required: shdw_libc_verify_group treats a NULL original here as a failure")
    else:
        risks.append("optional/best-effort: alias-validated via dladdr at install time, not verify-checked")

    target = {
        "stable_hook_id": f"{parent_id}::{row['symbol']}",
        "parent_install_unit": parent_id,
        "source": {"file": source_file_rel, "line": row["source_line"]},
        "target_kind": "function_symbol",
        "target_symbol": row["symbol"],
        "role": "mandatory",
        "commit_domain": parent_domain,
        # dlsym(RTLD_DEFAULT, ...) resolution, same reach class as the
        # findSymbolInImage-resolved pattern_scan children.
        "required_reach": ["existing_imports", "private_address"],
        "original_requirement": "direct_predecessor",  # outOldPtr: is always d->original
        "continuation_policy": "any",
        "availability": "required_now",
        "extraction_method": "structured_table",
        "known_compatibility_risks": risks,
    }
    if verified:
        target["verification_method"] = "shdw_libc_verify_group (see libc.x)"
    return target


def parse_devicecheck_descriptor_table(raw: str, source_label: str = "<text>"):
    """Parse DeviceCheck's DCHDescriptor table without guessing from Logos.

    Two rows may describe the same method with mutually exclusive runtime
    encodings (`B`/`@`); callers collapse those variants into one manifest
    target because Shadow installs at most one replacement for that method.
    """
    clean = strip_comments_preserve_lines(raw)
    body, body_start_line = find_c_array_body(
        clean, "shdw_devicecheck_descriptors", source_label)
    rows = []
    for row_match in ROW_RE.finditer(body):
        row_line = body_start_line + body.count("\n", 0, row_match.start())
        fields = parse_row_fields(row_match.group(1))
        if len(fields) != 6:
            raise ValueError(
                f"{source_label}:{row_line}: expected 6 DeviceCheck descriptor "
                f"fields, got {len(fields)}: {fields!r}")
        class_f, selector_f, kind_f, encoding_f, _arg_count_f, _policy_f = fields
        if class_f.strip() in ("NULL", "nil"):
            continue
        if kind_f.strip() not in ("DCHMethodInstance", "DCHMethodClass"):
            raise ValueError(f"{source_label}:{row_line}: unknown method kind {kind_f!r}")
        encoding = encoding_f.strip()
        if not re.fullmatch(r"'(?:[^'\\]|\\.)'", encoding):
            raise ValueError(f"{source_label}:{row_line}: expected char encoding, got {encoding!r}")
        rows.append({
            "target_class": parse_string_literal(class_f),
            "target_selector": parse_string_literal(selector_f),
            "method_kind": "instance" if kind_f.strip() == "DCHMethodInstance" else "class",
            "encoding": encoding[1:-1],
            "source_line": row_line,
        })
    return rows


def devicecheck_rows_to_manifest_targets(rows, parent_id, parent_target,
                                         source_file_rel):
    """Collapse DeviceCheck's encoding variants into concrete ObjC methods."""
    grouped = {}
    for row in rows:
        key = (row["target_class"], row["target_selector"], row["method_kind"])
        grouped.setdefault(key, []).append(row)

    targets = []
    for (target_class, target_selector, method_kind), variants in grouped.items():
        encodings = ", ".join(sorted({row["encoding"] for row in variants}))
        targets.append({
            "stable_hook_id": (
                f"{parent_id}::{target_class}::{method_kind}:{target_selector}"),
            "parent_install_unit": parent_id,
            "source": {"file": source_file_rel,
                       "line": variants[0]["source_line"]},
            "target_kind": "objc_method",
            "target_class": target_class,
            "target_selector": target_selector,
            "role": parent_target["role"],
            "commit_domain": parent_target["commit_domain"],
            "required_reach": ["objc_dispatch"],
            # DeviceCheck passes outOldPtr:NULL intentionally.
            "original_requirement": "none",
            "continuation_policy": "any",
            # Missing classes/methods or a non-matching runtime encoding are
            # skipped by shdw_devicecheck_install_hooks.
            "availability": "optional_if_present",
            "extraction_method": "structured_table",
            "current_implementation": "shdw_devicecheck_install_hooks",
            "known_compatibility_risks": [
                f"runtime return encoding must match one of {encodings}; "
                "otherwise Shadow skips this method"
            ],
        })
    return targets


def extract_logos_targets(shadow_repo: str, group_parent_units=None,
                          unit_targets=None):
    """Extract direct Objective-C targets from Logos `%hook` blocks.

    Logos expands these blocks before Clang sees them, so this pass records
    source-level targets with the weaker `logos_preprocess` confidence. The
    companion AST tool is available when compiler-grade declaration checks are
    needed.
    """
    targets = []
    group_parent_units = group_parent_units or {}
    unit_targets = unit_targets or {}
    root = os.path.join(shadow_repo, "ShadowCore.dylib")
    if not os.path.isdir(root):
        return targets
    for source_root, _dirs, files in os.walk(root):
        for filename in sorted(files):
            if not filename.endswith((".x", ".xm")):
                continue
            path = os.path.join(source_root, filename)
            metadata = parse_logos_file(path)
            relative = os.path.relpath(path, shadow_repo)
            for hook in metadata["hooks"]:
                cls = hook.get("class")
                if not cls:
                    continue
                group = hook.get("group")
                parent_ids = sorted(group_parent_units.get(group, ()))
                parent_target = (
                    unit_targets.get(parent_ids[0]) if len(parent_ids) == 1
                    else None
                )
                parent = parent_ids[0] if parent_target else f"logos:{relative}:{cls}"
                domain = (parent_target["commit_domain"] if parent_target
                          else f"logos_{os.path.splitext(filename)[0]}")
                risks = [
                    "Logos source was normalized without compiler macro expansion; "
                    "verify %orig and conditional paths before migration"
                ]
                if not parent_target:
                    if parent_ids:
                        risks.append(
                            f"Logos group {group!r} maps to multiple install units "
                            f"({', '.join(parent_ids)}); parent needs review"
                        )
                    else:
                        risks.append(
                            f"could not map Logos group {group!r} to a coordinator "
                            "install unit; parent needs review"
                        )
                for method in hook["methods"]:
                    selector = method["selector"]
                    kind = method["kind"]
                    targets.append({
                        "stable_hook_id": f"{parent}::{cls}::{kind}:{selector}",
                        "parent_install_unit": parent,
                        "source": {"file": relative, "line": method["line"]},
                        "target_kind": "objc_method",
                        "target_class": cls,
                        "target_selector": selector,
                        "role": parent_target["role"] if parent_target else "mandatory",
                        "commit_domain": domain,
                        "required_reach": ["objc_dispatch"],
                        "original_requirement": "direct_predecessor",
                        "continuation_policy": "any",
                        "availability": (parent_target.get("availability", "required_now")
                                         if parent_target else "required_now"),
                        "extraction_method": "logos_preprocess",
                        "current_implementation": f"{relative}:{method['line']}",
                        "known_compatibility_risks": risks,
                    })
    return targets


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


def resolve_shadow_commit(shadow_repo, explicit_commit=None):
    """Return an explicit provenance value or the checkout's HEAD.

    git archive snapshots intentionally have no .git directory.  They must
    either carry their known source commit explicitly or omit the optional
    schema field; `null` is not valid manifest provenance.
    """
    if explicit_commit:
        return explicit_commit
    if not os.path.exists(os.path.join(shadow_repo, ".git", "HEAD")):
        return None
    try:
        import subprocess
        return subprocess.check_output(
            ["git", "-C", shadow_repo, "rev-parse", "HEAD"],
            text=True).strip()
    except Exception:
        return None


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--shadow-repo", default=os.environ.get("SHADOW_REPO"),
                     help="path to a shadow checkout (default: $SHADOW_REPO, "
                          "then ../../shadow relative to this script)")
    ap.add_argument("--out", default=None,
                     help="output path (default: metadata/manifests/shadow-current-manifest.json "
                          "under this script's HookKit repo root)")
    ap.add_argument("--include-logos", action="store_true",
                    help="also emit per-method targets from Logos %hook blocks")
    ap.add_argument("--shadow-commit", default=None,
                    help="provenance for an immutable snapshot without .git")
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

    # Cross-reference the coordinator installer table -- the OTHER real
    # structured table named in spec section 18.2 ("Coordinator installer
    # table"). Gives the exact C function per unit mechanically, instead of
    # guessing from a filename convention, and doubles as a cross-check: the
    # source's own comment says the two tables "must stay in
    # SHDWInstallUnits() order," so a mismatch here is a real finding.
    coordinator_source = find_coordinator_installers_source(shadow_repo)
    installers = {}
    if coordinator_source:
        with open(coordinator_source, "r", encoding="utf-8") as f:
            installers = parse_coordinator_installers_from_text(
                f.read(), source_label=coordinator_source)
    else:
        print("warning: no known coordinator source found -- current_implementation left "
              "unset, no per-hook decomposition attempted", file=sys.stderr)

    missing_installer = [uid for uid in target_by_id if uid not in installers]
    missing_unit = [uid for uid in installers if uid not in target_by_id]
    if missing_installer:
        print(f"warning: {len(missing_installer)} install unit(s) have no "
              f"matching coordinator installer row: {missing_installer}", file=sys.stderr)
    if missing_unit:
        print(f"warning: coordinator installer table has {len(missing_unit)} "
              f"unit(s) not in kSHDWInstallUnits: {missing_unit}", file=sys.stderr)

    # Per-hook decomposition, best-effort: direct calls first, then one
    # bounded helper hop for coordinator wrappers that pass the same hooks
    # object onward. This is intentionally not a call-graph walker.
    child_targets = []
    decomposed = []
    logos_group_units = {}
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
        for group in find_logos_init_groups(body):
            logos_group_units.setdefault(group, set()).add(unit_id)

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
            delegated_calls = []
            delegated_names = find_delegated_functions(body)
            for delegated_name in delegated_names:
                delegated_path = find_function_definition_file(shadow_repo, delegated_name)
                if not delegated_path:
                    continue
                with open(delegated_path, "r", encoding="utf-8") as f:
                    delegated_clean = strip_comments_preserve_lines(f.read())
                delegated_found = find_function_body(delegated_clean, delegated_name)
                if not delegated_found:
                    continue
                delegated_body, delegated_start_line = delegated_found
                for group in find_logos_init_groups(delegated_body):
                    logos_group_units.setdefault(group, set()).add(unit_id)
                delegated_calls.extend((call, delegated_path, delegated_body)
                                       for call in find_hook_function_calls(
                                           delegated_body, delegated_start_line))

            if delegated_calls:
                for call, delegated_path, delegated_body in delegated_calls:
                    child_targets.append(hook_call_to_manifest_target(
                        call, unit_id, t["commit_domain"],
                        os.path.relpath(delegated_path, shadow_repo), delegated_body))
                decomposed.append(unit_id)
                t["known_compatibility_risks"].append(
                    f"decomposed into {len(delegated_calls)} pattern_scan child target(s) "
                    f"through one helper hop from {installer['install_fn']}()"
                )
            else:
                t["known_compatibility_risks"].append(
                    f"no [hooks hookFunction:...] call sites found in "
                    f"{installer['install_fn']}() ({rel}) -- delegates {delegated_names or 'nothing visible'}; "
                    "may use a descriptor table, shdw_libc_install_group, or a "
                    "Logos %hook/%init block; needs a follow-up pass to classify"
                )

    # shdw_libc_hooks[] (libc.x): the other real descriptor table found this
    # iteration, shared by 4 units (Hook_Filesystem@c, Hook_LowLevelC,
    # Hook_AntiDebugging, Hook_EnvVars@c) via a group bitmask -- see
    # GROUP_TO_UNIT's citation for how each was verified against real source.
    libc_x = os.path.join(shadow_repo, "ShadowCore.dylib", "hooks", "FileHiding", "libc.x")
    if os.path.exists(libc_x):
        with open(libc_x, "r", encoding="utf-8") as f:
            libc_rows = parse_libc_descriptor_table(f.read(), source_label=libc_x)
        libc_rel = os.path.relpath(libc_x, shadow_repo)
        decomposed_by_libc = set()
        for row in libc_rows:
            for unit_id in row["install_units"]:
                t = target_by_id.get(unit_id)
                if not t:
                    continue
                child_targets.append(libc_row_to_manifest_target(
                    row, unit_id, t["commit_domain"], libc_rel))
                decomposed_by_libc.add(unit_id)
        for unit_id in decomposed_by_libc:
            t = target_by_id[unit_id]
            t["known_compatibility_risks"] = [
                r for r in t["known_compatibility_risks"]
                if not (r.startswith("unit-level only") or r.startswith("no [hooks hookFunction:...] call sites"))
            ]
            n = sum(1 for row in libc_rows if unit_id in row["install_units"])
            t["known_compatibility_risks"].append(
                f"decomposed into {n} structured_table child target(s) from "
                f"shdw_libc_hooks[] in {libc_rel}"
            )
            decomposed.append(unit_id)

    # DeviceCheck's message hooks are a separate real descriptor table. Its
    # rows are ABI variants, so parse them directly rather than claiming an
    # opaque unit-level message route or mistaking both variants for hooks.
    devicecheck_m = os.path.join(
        shadow_repo, "ShadowCore.dylib", "hooks", "Environment",
        "DeviceCheckHooks.m")
    devicecheck_parent = target_by_id.get("Hook_DeviceCheck")
    if devicecheck_parent and os.path.exists(devicecheck_m):
        with open(devicecheck_m, "r", encoding="utf-8") as f:
            devicecheck_rows = parse_devicecheck_descriptor_table(
                f.read(), source_label=devicecheck_m)
        devicecheck_rel = os.path.relpath(devicecheck_m, shadow_repo)
        devicecheck_targets = devicecheck_rows_to_manifest_targets(
            devicecheck_rows, "Hook_DeviceCheck", devicecheck_parent,
            devicecheck_rel)
        child_targets.extend(devicecheck_targets)
        devicecheck_parent["known_compatibility_risks"] = [
            risk for risk in devicecheck_parent["known_compatibility_risks"]
            if not risk.startswith("no [hooks hookFunction:...] call sites")
        ]
        devicecheck_parent["known_compatibility_risks"].append(
            f"decomposed into {len(devicecheck_targets)} structured_table "
            "child target(s) from shdw_devicecheck_descriptors[]"
        )
        decomposed.append("Hook_DeviceCheck")

    logos_targets = (extract_logos_targets(
        shadow_repo, logos_group_units, target_by_id)
        if args.include_logos else [])
    if logos_targets:
        child_targets.extend(logos_targets)
        logos_counts = Counter(
            target["parent_install_unit"] for target in logos_targets
            if target["parent_install_unit"] in target_by_id
        )
        for unit_id, count in logos_counts.items():
            t = target_by_id[unit_id]
            t["known_compatibility_risks"] = [
                risk for risk in t["known_compatibility_risks"]
                if not risk.startswith("no [hooks hookFunction:...] call sites")
            ]
            t["known_compatibility_risks"].append(
                f"decomposed into {count} logos_preprocess child target(s)"
            )
            decomposed.append(unit_id)

    targets.extend(child_targets)

    overrides_path = os.path.join(os.path.dirname(__file__), "manual_overrides.yaml")
    overrides = load_manual_overrides(overrides_path)
    applied = apply_overrides(targets, overrides)

    shadow_commit = resolve_shadow_commit(shadow_repo, args.shadow_commit)

    manifest = {
        "manifest_version": MANIFEST_VERSION,
        "generated_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "targets": targets,
    }
    if shadow_commit:
        manifest["shadow_commit"] = shadow_commit

    out_path = args.out or os.path.join(repo_root, "metadata", "manifests", "shadow-current-manifest.json")
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")

    method_counts = Counter(t["extraction_method"] for t in targets)
    print(f"wrote {len(targets)} targets to {out_path}")
    print(f"  {len(units)} install units, {len(child_targets)} child targets "
          f"from {len(set(decomposed))} decomposed unit(s)")
    print(f"  by extraction_method: {dict(method_counts)}")
    print(f"  ({len(applied)} overridden by manual_overrides.yaml: {applied})" if applied
          else "  (no manual overrides applied)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
