#!/usr/bin/env python3
"""Extract shadow-hook-manifest.schema.json from a Shadow checkout.

Current scope (honest, not aspirational — see docs/3.0/IMPLEMENTATION_STATUS.md,
Milestone 2): parses the ONE real, statically-parseable source of truth found
so far — the `kSHDWInstallUnits[]` array in Shadow.framework/HookConfiguration.m
— which is a genuine C array-of-structs the coordinator itself walks
(HookCoordinator.m calls SHDWInstallUnits() to get it), not a heuristic.

What this does NOT do yet: each row here is a coordinator-level "install
unit" (e.g. "Hook_Filesystem@c"), which the coordinator treats as one opaque
installable thing — it does NOT decompose into the individual C functions or
ObjC selectors hooked *inside* that unit (those live one level down, in files
like ShadowCore.dylib/hooks/**/*.x and per-group descriptor tables such as
DeviceCheckHooks.m's `shdw_devicecheck_descriptors[]`). That per-hook
decomposition is real future work — some groups have their own clean
structured tables (parseable the same way this script parses the install-unit
table), others still use free-form Logos %hook blocks that genuinely need
logos_preprocess.py + clang_ast_extract.py (not built yet). Don't read a
`structured_table`-tagged row here as "this hook target is fully specified" —
it means "this install unit's own existence and top-level metadata is
mechanically verified," which is what the manifest schema's `parent_install_unit`
field exists to let deeper extraction passes hang off of later.

required_reach / original_requirement / commit_domain below are reasoned
defaults derived from the real capability/phase enums (see the mapping
tables below, each with its citation), not verified per-hook facts. They are
deliberately conservative and are meant to be tightened once real per-hook
extraction exists. manual_overrides.yaml is the place to correct any of them
in the meantime without re-deriving this script's output.
"""

import argparse
import json
import os
import re
import sys
from datetime import datetime, timezone

MANIFEST_VERSION = "0.1.0-milestone2-installunits"

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


def extract_install_units(config_m_path: str):
    with open(config_m_path, "r", encoding="utf-8") as f:
        raw = f.read()
    return parse_install_units_from_text(raw, source_label=config_m_path)


def parse_install_units_from_text(raw: str, source_label: str = "<text>"):
    clean = strip_comments_preserve_lines(raw)

    m = re.search(r"kSHDWInstallUnits\s*\[\s*\]\s*=\s*\{", clean)
    if not m:
        raise RuntimeError(
            f"kSHDWInstallUnits[] not found in {source_label} -- "
            "table was moved/renamed, this extractor needs updating, not "
            "the manifest silently going stale"
        )

    # Find the matching closing brace for the *outer* array (depth-aware),
    # starting just after the opening '{' matched above.
    start = m.end()
    depth = 1
    i = start
    while depth > 0:
        if clean[i] == "{":
            depth += 1
        elif clean[i] == "}":
            depth -= 1
        i += 1
    body = clean[start:i - 1]
    body_start_line = clean.count("\n", 0, start) + 1

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

    print(f"wrote {len(targets)} install-unit targets to {out_path}")
    print(f"  ({len(applied)} overridden by manual_overrides.yaml: {applied})" if applied
          else "  (no manual overrides applied)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
