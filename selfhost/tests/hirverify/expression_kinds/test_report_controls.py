"""Check complete baseline/fixed reports without compiling a component."""
import importlib.util
from pathlib import Path
import sys

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("expression_kinds", Path(__file__).with_name("run.py"))
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)

fixed = [f"CHECK {name} ok" for name in runner.FIXED_CHECKS]
baseline = [
    f"CHECK {name} {'FAIL' if name in runner.BUGGY_FAILURES else 'ok'}"
    for name in runner.FIXED_CHECKS
]
runner.check_report("\n".join(fixed + ["SUMMARY failures=0"]), 0)
runner.check_report("\n".join(baseline + ["SUMMARY failures=3"]), 1)

for incomplete in (
    [line for line in baseline if line.endswith(" FAIL")],
    baseline[:-1],
    baseline + [baseline[0]],
):
    try:
        runner.check_report("\n".join(incomplete + ["SUMMARY failures=3"]), 1)
    except RuntimeError:
        continue
    raise AssertionError("incomplete or duplicate baseline report accepted")

print("PASS: complete fixed/baseline reports and three missing/duplicate controls")
