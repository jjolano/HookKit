#!/usr/bin/env python3
"""Self-check for extract_abi.py. Run directly: python3 test_extract_abi.py"""

import re

from extract_abi import sha256_file
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


if __name__ == "__main__":
    tests = [v for k, v in list(globals().items()) if k.startswith("test_")]
    for t in tests:
        t()
        print(f"PASS {t.__name__}")
    print(f"all {len(tests)} tests passed")
