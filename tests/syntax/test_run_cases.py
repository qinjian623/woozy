import tempfile
from pathlib import Path
from run_cases import source_for_case

with tempfile.TemporaryDirectory(prefix="lang-fixture-guard-") as directory:
    root = Path(directory)
    fixture = root / "example.wz"
    fixture.write_bytes(b"original\r\n")
    case = {"path": "example.wz", "source": "original\r\n"}
    assert source_for_case(case, root) == b"original\r\n"
    assert source_for_case({"source": "中\0"}, root) == "中\0".encode()
    fixture.write_bytes(b"changed")
    for invalid in [case, {"path": "../outside", "source": ""},
                    {"path": str(fixture), "source": "changed"}]:
        try:
            source_for_case(invalid, root)
        except ValueError:
            pass
        else:
            raise AssertionError("invalid or changed fixture was accepted")
print("fixture content and path checks passed")
