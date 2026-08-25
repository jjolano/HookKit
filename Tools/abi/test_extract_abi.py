#!/usr/bin/env python3
"""Self-check for extract_abi.py. Run directly: python3 test_extract_abi.py"""

import re
import struct

from extract_abi import (_chained_rebase_map, _complete_objc_arches,
                         _decode_chained_entry, _macho_segments_and_fixups,
                         _parse_objc_arch, _read_cstring,
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


# A method whose imp line ALSO has no -[Class sel] annotation, matching what
# real macOS CI otool prints for an arm64e selref needing chained-fixups
# decoding (see _resolve_chained_selectors) -- neither in-parser fallback
# can resolve this one, so it must survive filtering unresolved, carrying
# its slot vmaddr for the caller to attempt next.
OBJC_OTOOL_UNRESOLVABLE_SAMPLE = """0000000000001000 0x100 _OBJC_CLASS_$_HKSubstitutor
    data 0x200
        name 0x20 HKSubstitutor
        baseMethods 0x30 __OBJC_$_INSTANCE_METHODS_HKSubstitutor
            name 0x40 (0x244f8)
            types 0x60 v16@0:8
            imp 0x70 (0x9d9c)
"""


def test_objc_parser_keeps_unresolved_vmaddr_when_no_fallback_resolves_it():
    parsed = _parse_objc_arch(OBJC_OTOOL_UNRESOLVABLE_SAMPLE, "arm64e", {"HKSubstitutor"})
    methods = parsed["HKSubstitutor"]["instance_methods"]
    assert methods == [
        {"selector": None, "type_encoding": {"arm64e": "v16@0:8"},
         "_unresolved_vmaddr": 0x244f8}
    ]


def test_chained_pointer_decode_round_trips_known_bit_layouts():
    # Bit positions mirror Sources/Resolvers/HKChainedFixups.c's
    # decode_pointer(), validated there by test-chained-fixups. Encoding
    # with the same shifts the function under test uses to decode, then
    # asserting the round-trip, proves the extraction is self-consistent
    # with that already-proven layout without re-deriving magic numbers.
    DYLD_CHAINED_PTR_ARM64E = 1
    DYLD_CHAINED_PTR_64_OFFSET = 6

    # arm64e non-auth rebase: target:43, high8:8, next:11, bind:1=0, auth:1=0.
    raw = 0x2ABCDE | (7 << 51)
    is_bind, next_delta, target = _decode_chained_entry(raw, DYLD_CHAINED_PTR_ARM64E)
    assert (is_bind, next_delta, target) == (False, 7, 0x2ABCDE)

    # arm64e auth rebase: target:32, diversity:16, addrDiv:1, key:2, next:11,
    # bind:1=0, auth:1=1 -- only the low 32 bits count as target.
    raw = 0xABCD1234 | (3 << 51) | (1 << 63)
    is_bind, next_delta, target = _decode_chained_entry(raw, DYLD_CHAINED_PTR_ARM64E)
    assert (is_bind, next_delta, target) == (False, 3, 0xABCD1234)

    # arm64e bind: bind bit set -- no target extracted.
    raw = (1 << 62) | (5 << 51) | 0x1234
    is_bind, next_delta, target = _decode_chained_entry(raw, DYLD_CHAINED_PTR_ARM64E)
    assert (is_bind, next_delta, target) == (True, 5, None)

    # DYLD_CHAINED_PTR_64_OFFSET rebase: target:36, high8:8, reserved:7,
    # next:12, bind:1=0.
    raw = 0x7FEDCBA | (11 << 51)
    is_bind, next_delta, target = _decode_chained_entry(raw, DYLD_CHAINED_PTR_64_OFFSET)
    assert (is_bind, next_delta, target) == (False, 11, 0x7FEDCBA)


def _pack_segment_64(vmaddr, vmsize, fileoff, filesize):
    return (struct.pack("<II", 0x19, 72) + b"__DATA\x00".ljust(16, b"\x00") +
            struct.pack("<QQQQIIII", vmaddr, vmsize, fileoff, filesize, 3, 3, 0, 0))


def test_chained_fixups_end_to_end_resolves_a_synthetic_selref():
    # One segment, vmaddr==fileoff==0x1000 to keep the address arithmetic
    # readable, one page with one rebase chain entry (arm64e, target = the
    # offset from image base where the string lives), no bind/second entry.
    image_base = 0x1000
    string_vmaddr = image_base + 0x10
    slot_fileoff = image_base  # the chain entry itself, at page offset 0

    chained_fixups_header = struct.pack("<IIIIIII", 0, 28, 0, 0, 0, 0, 0)  # starts_offset=28
    starts_in_image = struct.pack("<II", 1, 8)  # seg_count=1, seg_info_offset=8 (relative)
    starts_in_segment = (
        struct.pack("<IHHQIH", 24, 0x1000, 1, 0, 0, 1) +  # size,page_size,ptr_fmt=ARM64E,seg_off=0,max_valid=0,page_count=1
        struct.pack("<H", 0)  # page_start[0] = 0 (chain starts at page offset 0)
    )
    fixups_blob = chained_fixups_header + starts_in_image + starts_in_segment
    fixups_dataoff = 200

    data = bytearray(image_base + 0x100)
    struct.pack_into("<IIIIIIII", data, 0, 0xFEEDFACF, 0x0100000C, 0, 6, 2, 88, 0, 0)
    data[32:32 + 72] = _pack_segment_64(image_base, 0x1000, image_base, 0x1000)
    data[104:104 + 16] = struct.pack("<IIII", 0x80000034, 16, fixups_dataoff, len(fixups_blob))
    data[fixups_dataoff:fixups_dataoff + len(fixups_blob)] = fixups_blob
    struct.pack_into("<Q", data, slot_fileoff, 0x10)  # arm64e rebase, target=0x10, next=0
    data[string_vmaddr:string_vmaddr + len(b"hello\x00")] = b"hello\x00"  # fileoff == vmaddr here

    segments, fixups = _macho_segments_and_fixups(bytes(data))
    assert segments == [(image_base, 0x1000, image_base, 0x1000)]
    assert fixups == (fixups_dataoff, len(fixups_blob))

    rebase_map = _chained_rebase_map(bytes(data), segments, fixups)
    assert rebase_map == {image_base: string_vmaddr}
    assert _read_cstring(bytes(data), segments, string_vmaddr) == "hello"


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
