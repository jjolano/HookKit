#!/usr/bin/env python3

import os
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
LOGOS = ROOT / "tools/logos-hookkit/logos.pl"


def test_objc_original_is_prepared_before_commit():
    theos = Path(os.environ.get("THEOS", "/home/coder/theos"))
    env = os.environ.copy()
    env["PERL5LIB"] = str(theos / "vendor/logos/bin/lib")
    source = "%hook Demo\n- (id)value { return %orig; }\n%end\n"

    with tempfile.NamedTemporaryFile("w", suffix=".x", delete=False) as fixture:
        fixture.write(source)
        fixture_path = fixture.name
    try:
        generated = subprocess.check_output(
            ["perl", str(LOGOS), "-c", "generator=hookkit", fixture_path],
            env=env,
            text=True,
        )
    finally:
        Path(fixture_path).unlink()

    requirement = "_spec.original_requirement = _old ? HK_ORIGINAL_DIRECT_PREDECESSOR : HK_ORIGINAL_NONE;"
    missing = "if (!_prepared.continuation.address)"
    prepared = "*_old=(IMP)_prepared.continuation.address;"
    commit = "hk_plan_commit(_plan, NULL)"
    assert requirement in generated
    assert missing in generated
    assert prepared in generated
    assert "*_old=NULL" not in generated
    assert "else if (_o != (void*)_prepared.continuation.address) *_old=(IMP)_o;" in generated
    assert generated.index(requirement) < generated.index("hk_plan_prepare(_plan, NULL)")
    assert generated.index(missing) < generated.index(commit)
    assert generated.index(prepared) < generated.index(commit)


if __name__ == "__main__":
    test_objc_original_is_prepared_before_commit()
    print("PASS test_objc_original_is_prepared_before_commit")
