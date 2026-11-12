#!/usr/bin/env python3
import json
import os
import subprocess
import sys
from pathlib import Path

def main():
    repo_root = Path(__file__).resolve().parent.parent.parent
    build_dir = repo_root / "build" / "woozy-stdlib-native"
    woozyc = build_dir / "woozyc"
    workspace_root = repo_root.parent
    libxml2 = workspace_root / "toolchains" / "libxml2-legacy-2.13.9" / "usr" / "lib"

    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = f"{libxml2}:{env.get('LD_LIBRARY_PATH', '')}"

    cases_file = repo_root / "tests" / "async" / "cases.json"
    with open(cases_file) as f:
        catalog = json.load(f)

    out_base = Path("/tmp/woozy-async-cases")
    out_base.mkdir(parents=True, exist_ok=True)

    passed = 0
    total = len(catalog.get("positive", [])) + len(catalog.get("reject", []))

    print(f"Running {total} async test cases...")

    # Positive cases in both O0 and O2
    for opt in ("0", "2"):
        for case in catalog.get("positive", []):
            name = case["name"]
            toml_path = repo_root / "tests" / "async" / name / "lang.toml"
            out_dir = out_base / f"{name}_O{opt}"
            out_dir.mkdir(parents=True, exist_ok=True)

            # Build
            build_cmd = [str(woozyc), "build", "--project", str(toml_path), "--backend", "native", "--opt", opt, "--out-dir", str(out_dir)]
            res = subprocess.run(build_cmd, env=env, capture_output=True, text=True)
            if res.returncode != 0:
                print(f"FAILED (build O{opt}): positive case '{name}' exited {res.returncode}")
                print("STDOUT:", res.stdout)
                print("STDERR:", res.stderr)
                sys.exit(1)

            # Execute
            prog = out_dir / "program"
            run_res = subprocess.run([str(prog)], capture_output=True, text=True, timeout=10)
            expected_exit = case.get("exit", 0)
            expected_stdout = case.get("stdout")

            if run_res.returncode != expected_exit:
                print(f"FAILED (exit O{opt}): positive case '{name}' got exit {run_res.returncode}, expected {expected_exit}")
                sys.exit(1)
            if expected_stdout is not None and run_res.stdout != expected_stdout:
                print(f"FAILED (stdout O{opt}): positive case '{name}'\nEXPECTED:\n{expected_stdout}\nGOT:\n{run_res.stdout}")
                sys.exit(1)

            print(f"  [PASS] positive (O{opt}): {name}")
            passed += 1

    # Reject cases
    for case in catalog.get("reject", []):
        name = case["name"]
        code = case["code"]
        toml_path = repo_root / "tests" / "async" / "reject" / name / "lang.toml"
        out_dir = out_base / "reject" / name
        out_dir.mkdir(parents=True, exist_ok=True)

        build_cmd = [str(woozyc), "build", "--project", str(toml_path), "--backend", "native", "--opt", "0", "--out-dir", str(out_dir)]
        res = subprocess.run(build_cmd, env=env, capture_output=True, text=True)

        if res.returncode == 0:
            print(f"FAILED (expected failure): reject case '{name}' built successfully, expected code '{code}'")
            sys.exit(1)

        all_output = res.stdout + res.stderr
        if code not in all_output:
            print(f"FAILED (error code): reject case '{name}' output did not contain '{code}'")
            print("OUTPUT:", all_output)
            sys.exit(1)

        print(f"  [PASS] reject: {name} (expected {code})")
        passed += 1

    print(f"\nALL {passed}/{total} ASYNC CASES PASSED!")

if __name__ == "__main__":
    main()
