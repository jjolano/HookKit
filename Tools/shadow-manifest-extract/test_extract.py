#!/usr/bin/env python3
"""Self-check for extract.py's parser. Run directly: python3 test_extract.py

Covers the failure mode this session actually hit once already: a naive
text match finding directive-looking substrings inside comments. Also
covers NULL vs identifier prefKey, line-number accuracy, and the
malformed-row-count error path.
"""

from extract import parse_install_units_from_text, unit_to_manifest_target

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
