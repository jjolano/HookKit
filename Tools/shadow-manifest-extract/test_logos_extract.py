#!/usr/bin/env python3

import json
import os
import subprocess
import tempfile

from clang_ast_extract import extract_file
from logos_preprocess import normalized_source, parse_text


FIXTURE = """// %hook FakeComment\n%group test\n%hook Demo\n- (id)value;\n- (void)setValue:(id)v other:(id)o { }\n+ (BOOL)ready { return YES; }\n%end\n%init(DemoHooks)\n"""


def test_logos_metadata_and_normalized_source():
    metadata = parse_text(FIXTURE)
    assert len(metadata["hooks"]) == 1
    hook = metadata["hooks"][0]
    assert hook["class"] == "Demo"
    assert [m["selector"] for m in hook["methods"]] == ["value", "setValue:other:", "ready"]
    assert metadata["initializers"][0]["initializer"] == "(DemoHooks)"
    assert "@interface Demo" in normalized_source(metadata)


def test_clang_ast_round_trip():
    with tempfile.NamedTemporaryFile("w", suffix=".x", delete=False) as stream:
        stream.write(FIXTURE)
        path = stream.name
    try:
        targets = extract_file(path)
    finally:
        os.unlink(path)
    assert {(t["target_class"], t["target_selector"], t["method_kind"]) for t in targets} == {
        ("Demo", "value", "instance"),
        ("Demo", "setValue:other:", "instance"),
        ("Demo", "ready", "class"),
    }


if __name__ == "__main__":
    tests = [test_logos_metadata_and_normalized_source, test_clang_ast_round_trip]
    for test in tests:
        test()
        print(f"PASS {test.__name__}")
    print(f"all {len(tests)} tests passed")
