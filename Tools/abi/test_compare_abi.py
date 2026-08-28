#!/usr/bin/env python3
"""Self-check for compare_abi.py."""

from compare_abi import compare


def test_additive_surface_passes():
    old = {
        "install_name": "@rpath/HookKit.framework/HookKit",
        "compatibility_version": "3.0.0",
        "architectures": ["arm64"],
        "exported_symbols": {"arm64": ["_old"]},
        "header_checksums": {"Headers/HookKit.h": "old"},
        "objc": {"classes": [{"name": "HKFixture", "instance_methods": [
            {"selector": "init", "type_encoding": {"arm64": "@16@0:8"}}
        ]}]},
        "enum_values": {"HK_OK": 0},
    }
    new = {
        "install_name": "@rpath/HookKit.framework/HookKit",
        "compatibility_version": "3.1.0",
        "architectures": ["arm64", "arm64e"],
        "exported_symbols": {"arm64": ["_old", "_new"]},
        "header_checksums": {"Headers/HookKit.h": "new"},
        "objc": {"classes": [{"name": "HKFixture", "instance_methods": [
            {"selector": "init", "type_encoding": {"arm64": "@16@0:8"}}
        ]}]},
        "enum_values": {"HK_OK": 0, "HK_ERR": 1},
    }
    assert compare(old, new) == []


def test_removed_abi_is_reported():
    old = {
        "install_name": "HookKit",
        "architectures": ["arm64"],
        "exported_symbols": {"arm64": ["_old"]},
        "header_checksums": {},
    }
    new = {
        "install_name": "HookKit",
        "architectures": ["arm64"],
        "exported_symbols": {"arm64": []},
        "header_checksums": {},
    }
    assert any("removed exported symbol _old" in error for error in compare(old, new))


def test_expected_install_name_replaces_historical_name_check():
    old = {
        "install_name": "@rpath/HookKit.framework/HookKit",
        "architectures": [],
        "exported_symbols": {},
        "header_checksums": {},
    }
    new = {
        "install_name": "@loader_path/.jbroot/Library/Frameworks/HookKit.framework/HookKit",
        "architectures": [],
        "exported_symbols": {},
        "header_checksums": {},
    }
    assert compare(old, new, expected_install_name=new["install_name"]) == []


def test_struct_fields_behind_pointer_do_not_break_abi():
    old = {
        "install_name": "HookKit",
        "architectures": ["arm64"],
        "exported_symbols": {"arm64": []},
        "header_checksums": {},
        "objc": {"classes": [{"name": "HKFixture", "instance_methods": [
            {"selector": "closeImage:", "type_encoding": {
                "arm64": "v24@0:8^{HKImage=}16"
            }}
        ]}]},
    }
    new = {
        "install_name": "HookKit",
        "architectures": ["arm64"],
        "exported_symbols": {"arm64": []},
        "header_checksums": {},
        "objc": {"classes": [{"name": "HKFixture", "instance_methods": [
            {"selector": "closeImage:", "type_encoding": {
                "arm64": "v24@0:8^{HKImage=Ii^v}16"
            }}
        ]}]},
    }
    assert compare(old, new) == []


if __name__ == "__main__":
    for name, test in sorted(globals().items()):
        if name.startswith("test_"):
            test()
            print(f"PASS {name}")
