#!/usr/bin/env python3
"""Self-check for extract_abi.py. Run directly: python3 test_extract_abi.py"""

import re

from extract_abi import (_complete_objc_arches, _parse_objc_arch,
                         parse_enum_values, sha256_file)
import hashlib
import tempfile
import os

# Real otool -l output captured against the actual built HookKit binary
# this session (roothide lane, before this test existed) -- not invented,
# so the regex is proven against real tool output, not a guess at its shape.
REAL_OTOOL_SAMPLE = """Load command 4
          cmd LC_ID_DYLIB
      cmdsize 96
         name @loader_path/.jbroot/Library/Frameworks/HookKit.framework/HookKit (offset 24)
   time stamp 1 Wed Dec 31 19:00:01 1969
      current version 2.5.0
compatibility version 2.5.0
Load command 5
          cmd LC_SEGMENT_64
"""

OBJC_OTOOL_SAMPLE = """0000000000001000 0x100 _OBJC_CLASS_$_HKSubstitutor
    data 0x200
        name 0x20 HKSubstitutor
        baseMethods 0x30 __OBJC_$_INSTANCE_METHODS_HKSubstitutor
            name 0x40 (0x50)
            types 0x60 v16@0:8
            imp 0x70 -[HKSubstitutor init]
        baseProperties 0x80 __OBJC_$_PROP_LIST_HKSubstitutor
            name 0x90 (0xa0)
            attributes 0xb0 T@,N,Vvalue
Meta Class
        baseMethods 0xc0 __OBJC_$_CLASS_METHODS_HKSubstitutor
            name 0xd0 (0xe0)
            types 0xf0 @16@0:8
            imp 0x100 +[HKSubstitutor shared]
0000000000001100 0x100 _OBJC_CLASS_$_HKSubstitutor
"""


def _parse(text):
    m = re.search(
        r"LC_ID_DYLIB.*?name\s+(\S+)\s+\(offset.*?current version\s+(\S+).*?compatibility version\s+(\S+)",
        text, re.DOTALL)
    return (m.group(1), m.group(2), m.group(3)) if m else (None, None, None)


def test_id_dylib_regex_against_real_otool_output():
    name, current, compat = _parse(REAL_OTOOL_SAMPLE)
    assert name == "@loader_path/.jbroot/Library/Frameworks/HookKit.framework/HookKit"
    assert current == "2.5.0"
    assert compat == "2.5.0"


def test_id_dylib_regex_stops_at_next_load_command():
    # Must not greedily match past a second LC_ID_DYLIB-shaped block --
    # DYLIB_ID and DYLIB_RPATH commands can appear more than once.
    doubled = REAL_OTOOL_SAMPLE + REAL_OTOOL_SAMPLE.replace("2.5.0", "9.9.9")
    name, current, compat = _parse(doubled)
    assert current == "2.5.0", f"regex matched too greedily: got {current!r}"


def test_id_dylib_regex_returns_none_when_absent():
    name, current, compat = _parse("Load command 0\n  cmd LC_SEGMENT_64\n")
    assert name is None and current is None and compat is None


def test_sha256_file_matches_hashlib_directly():
    content = b"hello world"
    with tempfile.NamedTemporaryFile(delete=False) as f:
        f.write(content)
        path = f.name
    try:
        assert sha256_file(path) == hashlib.sha256(content).hexdigest()
    finally:
        os.unlink(path)


def test_objc_parser_reads_address_only_selectors_and_deduplicates_refs():
    parsed = _parse_objc_arch(OBJC_OTOOL_SAMPLE, "arm64e", {"HKSubstitutor"})
    substitutor = parsed["HKSubstitutor"]
    assert substitutor["instance_methods"] == [
        {"selector": "init", "type_encoding": {"arm64e": "v16@0:8"}}
    ]
    assert substitutor["class_methods"] == [
        {"selector": "shared", "type_encoding": {"arm64e": "@16@0:8"}}
    ]
    assert substitutor["properties"] == [{"name": "(0xa0)"}]


def test_objc_metadata_fallback_can_fill_unresolved_slice():
    merged = {"HKSubstitutor": {"instance_methods": [{
        "selector": "init", "type_encoding": {"arm64": "v16@0:8"}
    }]}}
    _complete_objc_arches(merged, ["arm64", "arm64e"])
    assert merged["HKSubstitutor"]["instance_methods"][0]["type_encoding"] == {
        "arm64": "v16@0:8", "arm64e": "v16@0:8"
    }


def test_enum_parser_preserves_implicit_and_bit_values():
    with tempfile.NamedTemporaryFile(mode="w", suffix=".h", delete=False) as f:
        f.write("""
        typedef enum { A = 0, B = (1 << 2), C } Old;
        typedef NS_ENUM(NSUInteger, New) { D, E = B | 1, F };
        """)
        path = f.name
    try:
        assert parse_enum_values([path]) == {
            "A": 0, "B": 4, "C": 5, "D": 0, "E": 5, "F": 6
        }
    finally:
        os.unlink(path)


if __name__ == "__main__":
    tests = [v for k, v in list(globals().items()) if k.startswith("test_")]
    for t in tests:
        t()
        print(f"PASS {t.__name__}")
    print(f"all {len(tests)} tests passed")
