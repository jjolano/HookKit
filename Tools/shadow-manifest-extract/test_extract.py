#!/usr/bin/env python3
"""Self-check for extract.py's parser. Run directly: python3 test_extract.py

Covers the failure mode this session actually hit once already: a naive
text match finding directive-looking substrings inside comments. Also
covers NULL vs identifier prefKey, line-number accuracy, and the
malformed-row-count error path.
"""

import os
import tempfile

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
    find_delegated_functions,
    find_coordinator_installers_source,
    resolve_shadow_commit,
    find_logos_init_groups,
    extract_logos_targets,
    parse_devicecheck_descriptor_table,
    devicecheck_rows_to_manifest_targets,
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


LIBSYSTEM_RESOLVER_FIXTURE = '''
void shadowhook_sandbox(HKSubstitutor* hooks) {
    void* sym_signal = shdw_resolve_libsystem("_signal");
    if(sym_signal) [hooks hookFunction:sym_signal withReplacement:replaced_signal outOldPtr:(void **) &original_signal];
    sym_signal = shdw_resolve_libsystem("_bsd_signal");
    if(sym_signal) [hooks hookFunction:sym_signal withReplacement:replaced_bsd_signal outOldPtr:(void **) &original_bsd_signal];
}
'''


def test_libsystem_resolver_uses_nearest_assignment():
    clean = strip_comments_preserve_lines(LIBSYSTEM_RESOLVER_FIXTURE)
    calls = find_hook_function_calls(clean, 1)
    assert [resolve_symbol_variable(clean, "sym_signal", c["body_offset"])
            for c in calls] == ["_signal", "_bsd_signal"]


def test_hook_function_scan_accepts_category_specific_receiver():
    clean = strip_comments_preserve_lines('''
void shadowhook_syscall(HKSubstitutor* hooks) {
    HKSubstitutor* rebindOnly = [HKSubstitutor substitutorWithCategory:HK_CAT_FUNCTION_REBIND];
    [rebindOnly hookFunction:syscall withReplacement:replaced_syscall outOldPtr:(void **) &original_syscall];
}
''')
    calls = find_hook_function_calls(clean, 1)
    assert len(calls) == 1
    assert calls[0]["receiver"] == "rebindOnly"
    target = hook_call_to_manifest_target(
        calls[0], "Hook_Syscall", "phase_tier1", "syscall.x", clean)
    assert target["stable_hook_id"] == "Hook_Syscall::syscall"


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


def test_one_level_delegate_scan_is_deduplicated_and_receiver_specific():
    body = """
    shadowhook_dyld_symlookup(hooks, one);
    shadowhook_dyld_symlookup(hooks, duplicate);
    shdw_coord_other(other, value);
    """
    assert find_delegated_functions(body) == [
        "shadowhook_dyld_symlookup"
    ]


def test_coordinator_source_layout_fallback():
    with tempfile.TemporaryDirectory() as root:
        core = os.path.join(root, "ShadowCore.dylib")
        os.makedirs(core)
        current = os.path.join(core, "shadowcore.x")
        with open(current, "w", encoding="utf-8") as f:
            f.write("// current coordinator source\n")

        assert find_coordinator_installers_source(root) == current

        historical = os.path.join(core, "dylib.x")
        with open(historical, "w", encoding="utf-8") as f:
            f.write("// historical coordinator source\n")
        assert find_coordinator_installers_source(root) == historical


def test_snapshot_provenance_is_explicit_or_omitted():
    with tempfile.TemporaryDirectory() as root:
        # A git archive has no .git directory. It must not turn into a JSON
        # null for the optional string-valued schema field.
        assert resolve_shadow_commit(root) is None
        assert resolve_shadow_commit(root, "6ad67ba") == "6ad67ba"


def test_logos_groups_inherit_their_coordinator_unit():
    with tempfile.TemporaryDirectory() as root:
        source_dir = os.path.join(root, "ShadowCore.dylib", "hooks")
        os.makedirs(source_dir)
        source = os.path.join(source_dir, "fixture.x")
        with open(source, "w", encoding="utf-8") as f:
            f.write("""%group shadowhook_Fixture
%hook Fixture
- (id)value { return %orig; }
%end
%end
""")

        unit = {
            "role": "optional",
            "commit_domain": "phase_tier1",
            "availability": "defer_until_available",
        }
        targets = extract_logos_targets(
            root, {"shadowhook_Fixture": {"Hook_Fixture"}},
            {"Hook_Fixture": unit})
        assert len(targets) == 1
        target = targets[0]
        assert target["parent_install_unit"] == "Hook_Fixture"
        assert target["stable_hook_id"] == "Hook_Fixture::Fixture::instance:value"
        assert target["role"] == "optional"
        assert target["commit_domain"] == "phase_tier1"
        assert target["availability"] == "defer_until_available"


def test_logos_init_groups_are_deduplicated():
    body = "%init(shadowhook_A); %init(shadowhook_A); %init(shadowhook_B);"
    assert find_logos_init_groups(body) == ["shadowhook_A", "shadowhook_B"]


DEVICECHECK_TABLE_FIXTURE = '''
const DCHDescriptor shdw_devicecheck_descriptors[] = {
    { "Probe", "isRooted", DCHMethodInstance, 'B', 0, DCHPolicyFalse },
    { "Probe", "isRooted", DCHMethodInstance, '@', 0, DCHPolicyFalse },
    { "Probe", "isTrusted", DCHMethodClass, 'B', 0, DCHPolicyTrue },
    { NULL, NULL, 0, 0, 0, 0 }
};
'''


def test_devicecheck_descriptor_table_collapses_encoding_variants():
    rows = parse_devicecheck_descriptor_table(DEVICECHECK_TABLE_FIXTURE, "fixture")
    assert len(rows) == 3
    parent = {"role": "optional", "commit_domain": "phase_tier1"}
    targets = devicecheck_rows_to_manifest_targets(
        rows, "Hook_DeviceCheck", parent, "DeviceCheckHooks.m")
    assert len(targets) == 2
    rooted = next(t for t in targets if t["target_selector"] == "isRooted")
    assert rooted["stable_hook_id"] == "Hook_DeviceCheck::Probe::instance:isRooted"
    assert rooted["original_requirement"] == "none"
    assert rooted["availability"] == "optional_if_present"
    assert "@, B" in rooted["known_compatibility_risks"][0]


if __name__ == "__main__":
    tests = [v for k, v in list(globals().items()) if k.startswith("test_")]
    for t in tests:
        t()
        print(f"PASS {t.__name__}")
    print(f"all {len(tests)} tests passed")
