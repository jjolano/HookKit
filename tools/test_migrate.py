#!/usr/bin/env python3
"""Run with python3 tools/test_migrate.py."""

import os
from pathlib import Path
from tempfile import TemporaryDirectory

from migrate import collect_all


def test_collect_all():
    previous = Path.cwd()
    with TemporaryDirectory() as directory:
        try:
            os.chdir(directory)
            expected = set()
            for parent in (".", "nested", ".theos", "vendor", ".git"):
                Path(parent).mkdir(exist_ok=True)
                for name in ("Tweak.x", "Tweak.xm", "Code.m", "Code.mm", "Makefile", "rules.mk"):
                    path = Path(parent) / name
                    path.touch()
                    if parent in (".", "nested"):
                        expected.add(path)
            Path("ignored.txt").touch()
            Path("directory.x").mkdir()
            actual = collect_all()
            assert set(actual) == expected, actual
            assert len(actual) == len(expected), actual
        finally:
            os.chdir(previous)


if __name__ == "__main__":
    test_collect_all()
    print("migration discovery: PASS")
