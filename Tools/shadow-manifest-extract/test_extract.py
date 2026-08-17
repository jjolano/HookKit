#!/usr/bin/env python3
"""Self-check for extract.py's parser. Run directly: python3 test_extract.py

Covers the failure mode this session actually hit once already: a naive
text match finding directive-looking substrings inside comments. Also
covers NULL vs identifier prefKey, line-number accuracy, and the
malformed-row-count error path.
"""

from extract import (
    parse_install_units_from_text,
    unit_to_manifest_target,
    strip_comments_preserve_lines,
    find_hook_function_calls,
    resolve_symbol_variable,
    hook_call_to_manifest_target,
    parse_group_mask_expr,
    parse_libc_descriptor_table,
    libc_row_to_manifest_target,
)

FIXTURE = '''#import <Shadow/HookConfiguration.h>

// A fake %hook block mention right here, and a fake kSHDWInstallUnits[]
// mention too -- if the comment-stripper is broken this line alone would
// corrupt the parse.
static const SHDWInstallUnit kSHDWInstallUnits[] = {
    // unconditional
    { "dyld",              NULL,                 SHDWPhaseAlways, SHDWCapabilityFunction, 1, 1 },
    /* pref-gated, multi
       line comment */
    { "Hook_Filesystem@c", SHDWHookIDFilesystem, SHDWPhaseTier1,  SHDWCapabilityFunction, 1, 0 },
};

const SHDWInstallUnit* SHDWInstallUnits(NSUInteger* outCount) {
    return kSHDWInstallUnits;
}
'''


def test_basic_parse():
    units = parse_install_units_from_text(FIXTURE, source_label="fixture")
    assert len(units) == 2, units

    assert units[0]["unit_id"] == "dyld"
    assert units[0]["pref_key"] is None
    assert units[0]["phase"] == "SHDWPhaseAlways"
    assert units[0]["capability"] == "SHDWCapabilityFunction"
    assert units[0]["ctor_install"] is True
    assert units[0]["verify"] is True
    # Line 8 in FIXTURE (1-indexed) is the "dyld" row (line 7 is the
    # "// unconditional" comment above it).
    assert units[0]["source_line"] == 8, units[0]["source_line"]

    assert units[1]["unit_id"] == "Hook_Filesystem@c"
    assert units[1]["pref_key"] == "SHDWHookIDFilesystem"
    assert units[1]["verify"] is False
    # Row after a 2-line block comment -- line tracking must survive it.
    assert units[1]["source_line"] == 11, units[1]["source_line"]


def test_comment_does_not_leak_into_parse():
    # The fixture's leading comments mention "kSHDWInstallUnits[]" and
    # "%hook" specifically to catch a comment-blind matcher -- exactly the
    # bug this session's grep -l hit for real against DeviceCheck.x.
    units = parse_install_units_from_text(FIXTURE, source_label="fixture")
    assert len(units) == 2  # not 0, not confused by the comment mentions


def test_target_mapping_reasoned_not_fabricated():
    units = parse_install_units_from_text(FIXTURE, source_label="fixture")
    t0 = unit_to_manifest_target(units[0], "fixture.m")
    assert t0["role"] == "mandatory"  # prefKey is NULL
    assert t0["commit_domain"] == "phase_always"
    assert t0["target_kind"] == "function_symbol"
    assert "verification_method" in t0  # verify bit was set

    t1 = unit_to_manifest_target(units[1], "fixture.m")
    assert t1["role"] == "optional"  # prefKey present
    assert "verification_method" not in t1  # verify bit was clear
    assert any("SHDWHookIDFilesystem" in r for r in t1["known_compatibility_risks"])


REUSED_VAR_FIXTURE = '''
void shadowhook_mach(HKSubstitutor* hooks) {
    void* sym = [hooks findSymbolInImage:NULL symbolName:@"_bootstrap_check_in2"];
    if(sym) [hooks hookFunction:sym withReplacement:replaced_a outOldPtr:(void **) &original_a];

    sym = [hooks findSymbolInImage:NULL symbolName:@"_bootstrap_check_in3"];
    if(sym) [hooks hookFunction:sym withReplacement:replaced_b outOldPtr:(void **) &original_b];

    sym = [hooks findSymbolInImage:NULL symbolName:@"_pid_for_task"];
    if(sym) [hooks hookFunction:sym withReplacement:replaced_c outOldPtr:(void **) &original_c];
}
'''


def test_reused_variable_resolves_to_nearest_preceding_assignment():
    # Regression test: mach.x reuses `sym` for 7 consecutive
    # resolve-then-hook steps. resolve_symbol_variable() used to take the
    # FIRST assignment anywhere in the function, which resolved every one
    # of those 7 calls to the same first symbol -- validate.py caught it as
    # duplicate stable_hook_ids against the real repo.
    clean = strip_comments_preserve_lines(REUSED_VAR_FIXTURE)
    calls = find_hook_function_calls(clean, 1)
    assert len(calls) == 3, calls

    resolved = [
        resolve_symbol_variable(clean, "sym", c["body_offset"])
        for c in calls
    ]
    assert resolved == ["_bootstrap_check_in2", "_bootstrap_check_in3", "_pid_for_task"], resolved

    targets = [hook_call_to_manifest_target(c, "Hook_MachBootstrap", "phase_tier1", "mach.x", clean)
               for c in calls]
    ids = [t["stable_hook_id"] for t in targets]
    assert len(ids) == len(set(ids)), f"duplicate stable_hook_ids: {ids}"
    assert ids == [
        "Hook_MachBootstrap::_bootstrap_check_in2",
        "Hook_MachBootstrap::_bootstrap_check_in3",
        "Hook_MachBootstrap::_pid_for_task",
    ], ids
    for t in targets:
        assert t["required_reach"] == ["existing_imports", "private_address"]
        assert t["original_requirement"] == "direct_predecessor"  # all three pass outOldPtr:


LIBC_TABLE_FIXTURE = '''
static const shdw_hook_desc_t shdw_libc_hooks[] = {
    { "access",  (void*)&replaced_access, (void**)&original_access, LIBC,       LIBC },
    { "close",   (void*)&replaced_close,  (void**)&original_close,  LIBC | LOW, LOW  },
};
'''


def test_group_mask_split():
    # "close" is claimed by two groups in the real table -- must produce two
    # units, not a comma-joined string or just the first one.
    assert parse_group_mask_expr("LIBC") == ["Hook_Filesystem@c"]
    assert parse_group_mask_expr("LIBC | LOW") == ["Hook_Filesystem@c", "Hook_LowLevelC"]
    assert parse_group_mask_expr("0") == []


def test_libc_descriptor_table_dual_group_row():
    rows = parse_libc_descriptor_table(LIBC_TABLE_FIXTURE, source_label="fixture")
    assert len(rows) == 2
    access, close = rows
    assert access["symbol"] == "access"
    assert access["install_units"] == ["Hook_Filesystem@c"]
    assert access["verify_units"] == ["Hook_Filesystem@c"]

    assert close["symbol"] == "close"
    assert close["install_units"] == ["Hook_Filesystem@c", "Hook_LowLevelC"]
    assert close["verify_units"] == ["Hook_LowLevelC"]  # LOW only, per the real table

    # Two manifest targets for "close", correctly reflecting that only the
    # LowLevelC parent actually verify-checks it (matches the real
    # shdw_libc_verify_group semantics read from libc.x).
    t_fs = libc_row_to_manifest_target(close, "Hook_Filesystem@c", "phase_tier1", "libc.x")
    t_low = libc_row_to_manifest_target(close, "Hook_LowLevelC", "phase_tier1", "libc.x")
    assert t_fs["stable_hook_id"] == "Hook_Filesystem@c::close"
    assert t_low["stable_hook_id"] == "Hook_LowLevelC::close"
    assert "verification_method" not in t_fs
    assert "verification_method" in t_low


def test_malformed_row_rejected():
    bad = '''
static const SHDWInstallUnit kSHDWInstallUnits[] = {
    { "onlyThreeFields", NULL, SHDWPhaseAlways },
};
'''
    try:
        parse_install_units_from_text(bad, source_label="bad-fixture")
    except ValueError as e:
        assert "expected 6 fields" in str(e)
    else:
        raise AssertionError("expected ValueError for a malformed row, got none")


def test_missing_table_rejected():
    try:
        parse_install_units_from_text("static const int x = 1;", source_label="empty")
    except RuntimeError as e:
        assert "kSHDWInstallUnits" in str(e)
    else:
        raise AssertionError("expected RuntimeError when the table isn't present")


if __name__ == "__main__":
    tests = [v for k, v in list(globals().items()) if k.startswith("test_")]
    for t in tests:
        t()
        print(f"PASS {t.__name__}")
    print(f"all {len(tests)} tests passed")
