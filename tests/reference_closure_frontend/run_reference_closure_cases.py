#!/usr/bin/env python3
"""Build and execute the independent reference closure/bound/function cases.

This is the runtime half of tests/reference_closure_frontend. It does not
contain a second .wz interpreter: all checking and code generation goes through
the real woozyc driver. Expected values are the independent checks inside the
source fixtures (main returns 0 only when every assertion holds).
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

POSITIVE = [
    "closure-copy-at-creation-sibling",
    "closure-creator-scope-escape",
    "closure-generic-two-instances",
    "closure-move-owning-and-scalar",
    "closure-nested-transitive",
    "closure-readonly-shared-mutation",
    "closure-two-capture-types",
    "fn-return-field-vec",
    "fn-throws-defer-capture",
    "bound-ordinary-repeat-copy",
    "bound-move-repeat-copy",
    "indirect-callee-arg-throw",
    "thread-capture-scalar-arg",
]
NEGATIVE = {
    "closure-move-owning-read-after": "E_OWNERSHIP_USE_AFTER_MOVE",
    "closure-readonly-assign": "E_TYPE_MISMATCH",
    "bound-inout-reject": "E_CALL_ARGUMENT",
}
PYTHON_NOT_RUN = {"thread-capture-scalar-arg"}


def run(argv, timeout=120, **kwargs):
    return subprocess.run(
        [str(a) for a in argv],
        capture_output=True,
        text=True,
        timeout=timeout,
        **kwargs,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--woozyc", required=True)
    parser.add_argument("--root", default=".")
    parser.add_argument("--python", default="python3.12")
    parser.add_argument("--opt", type=int, choices=(0, 2), default=0)
    args = parser.parse_args()
    root = Path(args.root).resolve()
    fixtures = root / "tests/reference_values/fixtures"
    results = {"passed": [], "not_run": [], "failed": []}
    with tempfile.TemporaryDirectory(prefix="woozy-closure-run-") as temp:
        for name in POSITIVE:
            manifest = fixtures / name / "lang.toml"
            # Native is the primary target; O0/O2 are selected explicitly.
            out = Path(temp) / f"native-{args.opt}-{name}"
            built = run([args.woozyc, "build", "--project", manifest,
                         "--backend", "native", "--opt", args.opt,
                         "--out-dir", out])
            if built.returncode != 0:
                results["failed"].append(
                    f"{name}: native O{args.opt} build failed: "
                    f"{built.stdout}{built.stderr}")
                continue
            executed = run([out / "program"])
            if executed.returncode != 0:
                results["failed"].append(
                    f"{name}: native O{args.opt} returned "
                    f"{executed.returncode}: {executed.stdout}{executed.stderr}")
            else:
                results["passed"].append(f"{name}: native O{args.opt}")
            # Python/64 is checked and executed separately.
            pyout = Path(temp) / f"python-{name}"
            built = run([args.woozyc, "build", "--project", manifest,
                         "--backend", "python", "--out-dir", pyout])
            if built.returncode != 0:
                if name in PYTHON_NOT_RUN and "E_CAPABILITY_UNAVAILABLE" in (
                        built.stdout + built.stderr):
                    results["not_run"].append(
                        f"{name}: python backend still rejects this call "
                        "signature")
                else:
                    results["failed"].append(
                        f"{name}: python build failed: "
                        f"{built.stdout}{built.stderr}")
                continue
            executed = run([args.python, "-s", pyout / "program.py"])
            if executed.returncode != 0:
                results["failed"].append(
                    f"{name}: python returned {executed.returncode}: "
                    f"{executed.stdout}{executed.stderr}")
            else:
                results["passed"].append(f"{name}: python")
        for name, code in NEGATIVE.items():
            checked = run([args.woozyc, "check", "--project",
                           fixtures / name / "lang.toml", "--format", "json"])
            try:
                payload = json.loads(checked.stdout)
                codes = [d.get("code") for d in payload.get("diagnostics", [])]
            except json.JSONDecodeError:
                codes = []
            if checked.returncode != 0 and code in codes:
                results["passed"].append(f"{name}: rejected {code}")
            else:
                results["failed"].append(
                    f"{name}: expected rejection {code}, got "
                    f"{checked.returncode} {codes}")
    print(json.dumps({"status": "ok" if not results["failed"] else "failed",
                      "opt": args.opt, "results": results}, indent=2))
    return 0 if not results["failed"] else 1


if __name__ == "__main__":
    sys.exit(main())
