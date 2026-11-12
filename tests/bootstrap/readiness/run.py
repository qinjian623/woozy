#!/usr/bin/env python3
"""Run the Woozy self-hosting readiness probe.

This driver is deliberately path-agnostic: the stage-0 compiler, Clang, LLD,
working directory and any tool environment (for example LD_LIBRARY_PATH for a
locally built LLD) are supplied on the command line.  It never searches for a
compiler on PATH.

The probe is built at O0 and O2 by the supplied stage-0 woozyc.  Each probe
binary then runs the restricted case suite, emits textual LLVM IR, invokes the
supplied Clang and LLD through Woozy's process interface, and executes the
generated static ELF.  Probe runs use a PATH containing only failing trap
wrappers, so a call to woozyc, lang_native_helper or a bare clang/ld.lld is
recorded and fails the run.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
CASES_DIR = HERE / "cases"
CASES_JSON = HERE / "cases.json"
PROBE_MANIFEST = HERE / "probe" / "lang.toml"
PROBE_SOURCE = HERE / "probe" / "src" / "main.wz"

TRAP_NAMES = [
    "woozyc",
    "lang_native_helper",
    "woozyc-helper",
    "langc",
    "clang",
    "ld.lld",
    "cc",
    "gcc",
    "as",
    "ld",
]


class CheckFailure(Exception):
    pass


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_env(values: list[str]) -> dict[str, str]:
    result: dict[str, str] = {}
    for item in values:
        if "=" not in item:
            raise CheckFailure(f"--env expects NAME=VALUE, got {item!r}")
        name, value = item.split("=", 1)
        if not name:
            raise CheckFailure(f"empty environment name in {item!r}")
        result[name] = value
    return result


def run(
    command: list[str],
    *,
    env: dict[str, str],
    timeout: int,
    cwd: Path,
) -> dict[str, Any]:
    started = time.monotonic()
    try:
        completed = subprocess.run(
            command,
            cwd=str(cwd),
            env=env,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
            check=False,
        )
        timed_out = False
        code = completed.returncode
        stdout = completed.stdout
        stderr = completed.stderr
    except subprocess.TimeoutExpired as expired:
        timed_out = True
        code = None
        stdout = expired.stdout or b""
        stderr = expired.stderr or b""
    return {
        "command": command,
        "exit": code,
        "timed_out": timed_out,
        "stdout": stdout,
        "stderr": stderr,
        "seconds": round(time.monotonic() - started, 4),
    }


def text(value: bytes) -> str:
    return value.decode("utf-8", errors="replace")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise CheckFailure(message)


def write_log(path: Path, result: dict[str, Any]) -> None:
    path.write_bytes(
        f"command: {result['command']}\nexit: {result['exit']}\n"
        f"timed_out: {result['timed_out']}\nseconds: {result['seconds']}\n"
        f"--- stdout ---\n{text(result['stdout'])}\n"
        f"--- stderr ---\n{text(result['stderr'])}\n".encode("utf-8")
    )


def build_probe(woozyc: Path, opt: int, work: Path, env: dict[str, str], timeout: int) -> dict[str, Any]:
    out_dir = work / f"probe-o{opt}"
    if out_dir.exists():
        shutil.rmtree(out_dir)
    command = [
        str(woozyc),
        "build",
        "--project",
        str(PROBE_MANIFEST),
        "--backend",
        "native",
        "--opt",
        str(opt),
        "--out-dir",
        str(out_dir),
        "--format",
        "json",
    ]
    result = run(command, env=env, timeout=timeout, cwd=work)
    write_log(work / f"build-o{opt}.log", result)
    require(not result["timed_out"], f"stage-0 build O{opt} timed out")
    require(result["exit"] == 0, f"stage-0 build O{opt} failed with exit {result['exit']}")
    try:
        document = json.loads(text(result["stdout"]))
    except json.JSONDecodeError as error:
        raise CheckFailure(f"stage-0 build O{opt} did not print JSON: {error}") from error
    require(document.get("status") == "ok", f"stage-0 build O{opt} status is not ok: {document}")
    program = out_dir / "program"
    object_file = out_dir / "program.o"
    require(program.is_file(), f"stage-0 build O{opt} did not produce {program}")
    require(object_file.is_file(), f"stage-0 build O{opt} did not produce {object_file}")
    binary = program.read_bytes()
    require(binary.startswith(b"\x7fELF"), f"probe O{opt} is not an ELF file")
    for forbidden in (b"lang_native_helper", b"woozyc"):
        require(forbidden not in binary, f"probe O{opt} embeds forbidden old-frontend marker {forbidden!r}")
    check_elf(program, relocatable=False)
    check_elf(object_file, relocatable=True)
    return {
        "opt": opt,
        "program": str(program),
        "program_sha256": sha256_file(program),
        "program_bytes": program.stat().st_size,
        "object_sha256": sha256_file(object_file),
        "build_log": str(work / f"build-o{opt}.log"),
    }


def create_traps(work: Path) -> Path:
    trap_dir = work / "traps"
    trap_dir.mkdir(parents=True, exist_ok=True)
    for name in TRAP_NAMES:
        path = trap_dir / name
        path.write_text(
            "#!/bin/sh\n"
            'if [ -n "$WZ_TRAP_LOG" ]; then\n'
            '  printf \'%s\\n\' "$0 $*" >> "$WZ_TRAP_LOG"\n'
            "fi\n"
            "exit 97\n",
            encoding="utf-8",
        )
        path.chmod(0o755)
    return trap_dir


def probe_environment(trap_dir: Path, trap_log: Path, extra_env: dict[str, str]) -> dict[str, str]:
    trap_log.write_bytes(b"")
    env = dict(extra_env)
    # Isolation wins even if --env also supplies PATH or the trap log.
    env["PATH"] = str(trap_dir)
    env["WZ_TRAP_LOG"] = str(trap_log)
    return env


def run_probe(
    program: Path,
    source: Path,
    output_dir: Path,
    *,
    probe_opt: int,
    ir_opt: int,
    clang: Path,
    lld: Path,
    trap_dir: Path,
    extra_env: dict[str, str],
    timeout: int,
) -> dict[str, Any]:
    if output_dir.exists():
        shutil.rmtree(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    ir = output_dir / "source.ll"
    obj = output_dir / "source.o"
    elf = output_dir / "program"
    trap_log = output_dir / "trap.log"
    env = probe_environment(trap_dir, trap_log, extra_env)
    command = [
        str(program),
        str(source),
        str(ir),
        str(obj),
        str(elf),
        str(clang),
        str(lld),
        str(ir_opt),
        str(max(1, timeout // 4) * 1000),
    ]
    result = run(command, env=env, timeout=timeout, cwd=output_dir)
    write_log(output_dir / "probe.log", result)
    result["trap_log"] = text(trap_log.read_bytes()) if trap_log.exists() else ""
    result["ir"] = ir
    result["object"] = obj
    result["elf"] = elf
    result["probe_opt"] = probe_opt
    result["ir_opt"] = ir_opt
    return result


def check_elf(path: Path, *, relocatable: bool) -> None:
    require(path.is_file(), f"missing ELF file {path}")
    data = path.read_bytes()
    require(len(data) >= 64, f"{path} is too small to be ELF")
    require(data[:4] == b"\x7fELF", f"{path} has no ELF magic")
    require(data[4] == 2, f"{path} is not ELF64")
    require(data[5] == 1, f"{path} is not little-endian")
    e_type = int.from_bytes(data[16:18], "little")
    e_machine = int.from_bytes(data[18:20], "little")
    expected_type = 1 if relocatable else 2
    require(e_type == expected_type, f"{path} e_type is {e_type}, expected {expected_type}")
    require(e_machine == 62, f"{path} e_machine is {e_machine}, expected x86_64 (62)")
    if relocatable:
        return
    e_phoff = int.from_bytes(data[32:40], "little")
    e_phentsize = int.from_bytes(data[54:56], "little")
    e_phnum = int.from_bytes(data[56:58], "little")
    require(e_phentsize >= 56, f"{path} has an invalid program-header size")
    p_types = []
    for index in range(e_phnum):
        offset = e_phoff + index * e_phentsize
        require(offset + 4 <= len(data), f"{path} program header is truncated")
        p_types.append(int.from_bytes(data[offset : offset + 4], "little"))
    require(3 not in p_types, f"{path} contains PT_INTERP; it is not static")
    require(2 not in p_types, f"{path} contains PT_DYNAMIC; it is not static")


def check_positive(case: dict[str, Any], result: dict[str, Any], *, timeout: int) -> dict[str, Any]:
    errors: list[str] = []
    stdout = text(result["stdout"])
    stderr = text(result["stderr"])
    if result["timed_out"]:
        errors.append("probe timed out")
    if result["exit"] != 0:
        errors.append(f"probe exit {result['exit']} != 0")
    if stderr:
        errors.append(f"probe wrote stderr: {stderr}")
    lines = set(stdout.splitlines())
    expected = case["expected_exit"]
    for required in (
        "probe: clang=ok",
        "probe: lld=ok",
        f"probe: elf-exit={expected}",
        f"probe: expected={expected}",
        "probe: pass",
    ):
        if required not in lines:
            errors.append(f"missing stdout line {required!r}")
    if result["trap_log"].strip():
        errors.append(f"old-frontend/tool trap was invoked: {result['trap_log']}")
    ir: Path = result["ir"]
    obj: Path = result["object"]
    elf: Path = result["elf"]
    if not ir.is_file():
        errors.append(f"missing IR file {ir}")
        ir_text = ""
    else:
        ir_text = ir.read_text(encoding="utf-8")
    for required in (
        'target triple = "x86_64-unknown-linux-gnu"',
        "define void @_start()",
        'asm sideeffect "syscall"',
        "and i64",
    ):
        if required not in ir_text:
            errors.append(f"IR is missing {required!r}")
    for required in case.get("ir_must_contain", []):
        if required not in ir_text:
            errors.append(f"IR is missing case-specific text {required!r}")
    for forbidden in ("nsw", "nuw", "declare "):
        if forbidden in ir_text:
            errors.append(f"IR contains forbidden text {forbidden!r}")
    try:
        check_elf(obj, relocatable=True)
    except CheckFailure as error:
        errors.append(str(error))
    try:
        check_elf(elf, relocatable=False)
    except CheckFailure as error:
        errors.append(str(error))
    if elf.is_file():
        elf_data = elf.read_bytes()
        for forbidden in (b"lang_native_helper", b"woozyc"):
            if forbidden in elf_data:
                errors.append(f"generated ELF embeds old-frontend marker {forbidden!r}")
    # Independently execute the generated static ELF; do not rely only on the
    # probe's own process wait report.
    driver_exit = None
    if elf.is_file():
        driver = run([str(elf)], env={}, timeout=timeout, cwd=elf.parent)
        write_log(elf.parent / "driver-exec.log", driver)
        if driver["timed_out"]:
            errors.append("driver execution of generated ELF timed out")
        else:
            driver_exit = driver["exit"]
            if driver_exit != expected:
                errors.append(f"driver ELF exit {driver_exit} != expected {expected}")
    if errors:
        raise CheckFailure("; ".join(errors))
    return {
        "case": case["name"],
        "probe_opt": result["probe_opt"],
        "ir_opt": result["ir_opt"],
        "status": "passed",
        "expected_exit": expected,
        "elf_exit": expected,
        "driver_elf_exit": driver_exit,
        "ir_sha256": hashlib.sha256(ir_text.encode("utf-8")).hexdigest(),
        "ir_bytes": ir.stat().st_size,
        "object_bytes": obj.stat().st_size,
        "elf_bytes": elf.stat().st_size,
        "seconds": result["seconds"],
    }


def check_negative(case: dict[str, Any], result: dict[str, Any]) -> dict[str, Any]:
    errors: list[str] = []
    stderr = text(result["stderr"])
    stdout = text(result["stdout"])
    if result["timed_out"]:
        errors.append("probe timed out")
    if result["exit"] != 1:
        errors.append(f"probe exit {result['exit']} != 1")
    if "probe: pass" in stdout:
        errors.append("negative case reported probe: pass")
    if "probe: clang=ok" in stdout or "probe: lld=ok" in stdout:
        errors.append("negative case invoked Clang or LLD")
    if result["trap_log"].strip():
        errors.append(f"old-frontend/tool trap was invoked: {result['trap_log']}")
    location = f":{case['line']}:{case['column']}: {case['code']}:"
    if location not in stderr:
        errors.append(f"stderr is missing diagnostic location {location!r}")
    span = f"bytes [{case['start']},{case['end']})"
    if span not in stderr:
        errors.append(f"stderr is missing byte span {span!r}")
    if case["message_contains"] not in stderr:
        errors.append(f"stderr is missing message text {case['message_contains']!r}")
    for key in ("ir", "object", "elf"):
        path: Path = result[key]
        if path.exists():
            errors.append(f"negative case wrote {key} at {path}")
    if errors:
        raise CheckFailure("; ".join(errors))
    return {
        "case": case["name"],
        "probe_opt": result["probe_opt"],
        "ir_opt": result["ir_opt"],
        "status": "passed",
        "diagnostic": case["code"],
        "line": case["line"],
        "column": case["column"],
        "bytes": [case["start"], case["end"]],
        "seconds": result["seconds"],
    }


def tool_versions(tools: dict[str, Path], env: dict[str, str], work: Path, timeout: int) -> dict[str, Any]:
    versions: dict[str, Any] = {}
    for name, path in tools.items():
        result = run([str(path), "--version"], env=env, timeout=timeout, cwd=work)
        write_log(work / f"version-{name}.log", result)
        require(not result["timed_out"], f"{name} --version timed out")
        require(result["exit"] == 0, f"{name} --version failed with exit {result['exit']}")
        output = text(result["stdout"]).strip().splitlines()
        versions[name] = {
            "path": str(path),
            "sha256": sha256_file(path),
            "version": output[0] if output else "",
        }
    return versions


def check_tool_failure(
    probe: Path,
    case: dict[str, Any],
    failing_tool: str,
    *,
    probe_opt: int,
    clang: Path,
    lld: Path,
    trap_dir: Path,
    extra_env: dict[str, str],
    work: Path,
    timeout: int,
) -> dict[str, Any]:
    output_dir = work / "tool-failure" / f"probe-o{probe_opt}-{failing_tool}"
    if output_dir.exists():
        shutil.rmtree(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    source = CASES_DIR / case["source"]
    wrapper = output_dir / f"failing-{failing_tool}"
    fail_log = output_dir / "failure.log"
    fail_log.write_bytes(b"")
    wrapper.write_text(
        "#!/bin/sh\n"
        'if [ -n "$WZ_FAIL_LOG" ]; then\n'
        '  printf \'%s\\n\' "$0 $*" >> "$WZ_FAIL_LOG"\n'
        "fi\n"
        "exit 7\n",
        encoding="utf-8",
    )
    wrapper.chmod(0o755)
    ir = output_dir / "source.ll"
    obj = output_dir / "source.o"
    elf = output_dir / "program"
    trap_log = output_dir / "trap.log"
    env = probe_environment(trap_dir, trap_log, extra_env)
    env["WZ_FAIL_LOG"] = str(fail_log)
    command = [
        str(probe),
        str(source),
        str(ir),
        str(obj),
        str(elf),
        str(wrapper if failing_tool == "clang" else clang),
        str(wrapper if failing_tool == "lld" else lld),
        "0",
        str(max(1, timeout // 4) * 1000),
    ]
    result = run(command, env=env, timeout=timeout, cwd=output_dir)
    write_log(output_dir / "probe.log", result)
    stderr = text(result["stderr"])
    errors: list[str] = []
    if result["timed_out"]:
        errors.append("probe timed out")
    if result["exit"] != 3:
        errors.append(f"probe exit {result['exit']} != 3")
    expected_message = f"{failing_tool} failed with status 7"
    if expected_message not in stderr:
        errors.append(f"stderr is missing {expected_message!r}")
    if not fail_log.read_bytes().strip():
        errors.append("failing wrapper was not invoked")
    if trap_log.read_bytes().strip():
        errors.append(f"old-frontend/tool trap was invoked: {text(trap_log.read_bytes())}")
    if not ir.is_file():
        errors.append("IR was not written before the tool failure")
    if failing_tool == "clang":
        if obj.exists() or elf.exists():
            errors.append("clang failure left an object or ELF")
    else:
        if not obj.is_file():
            errors.append("lld failure did not leave the Clang object")
        else:
            try:
                check_elf(obj, relocatable=True)
            except CheckFailure as error:
                errors.append(str(error))
        if elf.exists():
            errors.append("lld failure left an ELF")
    if errors:
        raise CheckFailure("; ".join(errors))
    return {
        "failing_tool": failing_tool,
        "probe_opt": probe_opt,
        "status": "passed",
        "probe_exit": 3,
        "wrapper_log": text(fail_log.read_bytes()).strip().splitlines(),
    }


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--woozyc", required=True, type=Path, help="stage-0 compiler")
    parser.add_argument("--clang", required=True, type=Path, help="Clang used by the probe")
    parser.add_argument("--lld", required=True, type=Path, help="LLD used by the probe")
    parser.add_argument("--work-dir", required=True, type=Path, help="results directory (kept)")
    parser.add_argument("--timeout", type=int, default=60, help="seconds per external command")
    parser.add_argument(
        "--probe-opts",
        default="0,2",
        help="comma-separated native optimization levels for the probe itself",
    )
    parser.add_argument(
        "--ir-opts",
        default="0,2",
        help="comma-separated Clang optimization levels for generated IR",
    )
    parser.add_argument(
        "--env",
        action="append",
        default=[],
        metavar="NAME=VALUE",
        help="extra environment for build, Clang, LLD and probe (repeatable)",
    )
    args = parser.parse_args(argv)

    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=True)
    # Keep the invoked tool basename: ld.lld selects its flavor from argv[0].
    woozyc = Path(os.path.abspath(str(args.woozyc)))
    clang = Path(os.path.abspath(str(args.clang)))
    lld = Path(os.path.abspath(str(args.lld)))
    for name, path in (("woozyc", woozyc), ("clang", clang), ("lld", lld)):
        require(path.is_file(), f"{name} does not exist: {path}")
    require(PROBE_MANIFEST.is_file(), f"missing probe manifest: {PROBE_MANIFEST}")
    require(PROBE_SOURCE.is_file(), f"missing probe source: {PROBE_SOURCE}")
    require(CASES_JSON.is_file(), f"missing case manifest: {CASES_JSON}")

    require(args.timeout > 0, "--timeout must be positive")
    extra_env = parse_env(args.env)
    base_env = os.environ.copy()
    base_env.update(extra_env)
    probe_opts = [int(value) for value in args.probe_opts.split(",") if value]
    ir_opts = [int(value) for value in args.ir_opts.split(",") if value]
    require(probe_opts and all(value in (0, 2) for value in probe_opts), "--probe-opts must contain 0 and/or 2")
    require(ir_opts and all(value in (0, 2) for value in ir_opts), "--ir-opts must contain 0 and/or 2")

    cases = json.loads(CASES_JSON.read_text(encoding="utf-8"))
    positive = cases["positive"]
    negative = cases["negative"]
    for case in positive + negative:
        source = CASES_DIR / case["source"]
        require(source.is_file(), f"missing case source: {source}")

    summary: dict[str, Any] = {
        "schema_version": 1,
        "status": "failed",
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "constraints": {
            "host_target": "Linux x86_64",
            "llvm_triple": "x86_64-unknown-linux-gnu",
            "generated_elf": "static ET_EXEC, no PT_INTERP, no PT_DYNAMIC",
            "probe_tool_path_isolation": "PATH contains failing trap wrappers only",
        "probe_binary_old_frontend_markers": "absent (checked in stage-0 build output)",
            "grammar": cases.get("grammar", ""),
        },
        "stage0": {
            "path": str(woozyc),
            "sha256": sha256_file(woozyc),
        },
        "tools": {},
        "probe": {
            "source": str(PROBE_SOURCE),
            "source_sha256": sha256_file(PROBE_SOURCE),
            "manifest": str(PROBE_MANIFEST),
            "builds": [],
        },
        "runs": [],
        "tool_failures": [],
    }

    try:
        versions = tool_versions({"woozyc": woozyc, "clang": clang, "lld": lld}, base_env, work, args.timeout)
        summary["tools"] = versions
        summary["stage0"]["version"] = versions["woozyc"]["version"]

        for opt in probe_opts:
            summary["probe"]["builds"].append(build_probe(woozyc, opt, work, base_env, args.timeout))

        trap_dir = create_traps(work)
        programs = {item["opt"]: Path(item["program"]) for item in summary["probe"]["builds"]}

        # Positive cases: all probe/IR optimization combinations.
        for probe_opt in probe_opts:
            for ir_opt in ir_opts:
                for case in positive:
                    output_dir = work / "runs" / f"probe-o{probe_opt}" / f"ir-o{ir_opt}" / case["name"]
                    result = run_probe(
                        programs[probe_opt],
                        CASES_DIR / case["source"],
                        output_dir,
                        probe_opt=probe_opt,
                        ir_opt=ir_opt,
                        clang=clang,
                        lld=lld,
                        trap_dir=trap_dir,
                        extra_env=extra_env,
                        timeout=args.timeout,
                    )
                    summary["runs"].append(check_positive(case, result, timeout=args.timeout))

        # Negative cases: diagnostics must be exact and no artifacts may be written.
        for probe_opt in probe_opts:
            for case in negative:
                output_dir = work / "negative" / f"probe-o{probe_opt}" / case["name"]
                result = run_probe(
                    programs[probe_opt],
                    CASES_DIR / case["source"],
                    output_dir,
                    probe_opt=probe_opt,
                    ir_opt=0,
                    clang=clang,
                    lld=lld,
                    trap_dir=trap_dir,
                    extra_env=extra_env,
                    timeout=args.timeout,
                )
                summary["runs"].append(check_negative(case, result))

        # The probe must propagate real tool failures rather than fabricate an ELF.
        for probe_opt in probe_opts:
            for failing_tool in ("clang", "lld"):
                summary["tool_failures"].append(
                    check_tool_failure(
                        programs[probe_opt],
                        positive[0],
                        failing_tool,
                        probe_opt=probe_opt,
                        clang=clang,
                        lld=lld,
                        trap_dir=trap_dir,
                        extra_env=extra_env,
                        work=work,
                        timeout=args.timeout,
                    )
                )

        # The same source must produce different IR for different inputs.
        positive_hashes = {run["ir_sha256"] for run in summary["runs"] if "ir_sha256" in run}
        require(
            len(positive_hashes) == len(positive),
            f"expected {len(positive)} distinct positive IR outputs, got {len(positive_hashes)}",
        )
        # Probe and Clang optimization levels must not change the textual IR for the same case.
        by_case_ir: dict[str, set[str]] = {}
        for run in summary["runs"]:
            if "ir_sha256" in run:
                by_case_ir.setdefault(run["case"], set()).add(run["ir_sha256"])
        require(
            all(len(values) == 1 for values in by_case_ir.values()),
            "optimization levels changed the IR for the same case",
        )

        summary["checks"] = {
            "positive_cases": len(positive),
            "negative_cases": len(negative),
            "positive_runs": sum(1 for run in summary["runs"] if "elf_exit" in run),
            "negative_runs": sum(1 for run in summary["runs"] if "diagnostic" in run),
            "tool_failure_runs": len(summary["tool_failures"]),
            "distinct_positive_ir": len(positive_hashes),
            "trap_logs_checked": len(summary["runs"]) + len(summary["tool_failures"]),
            "old_frontend_traps_invoked": 0,
        }
        summary["status"] = "passed"
        (work / "summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(
            "PASS: stage0 builds O0/O2; "
            f"{summary['checks']['positive_runs']} positive runs, "
            f"{summary['checks']['negative_runs']} negative runs, "
            f"{summary['checks']['tool_failure_runs']} tool-failure runs; "
            f"{summary['checks']['distinct_positive_ir']} distinct IR outputs"
        )
        print(f"summary: {work / 'summary.json'}")
        return 0
    except CheckFailure as error:
        summary["error"] = str(error)
        summary["status"] = "failed"
        (work / "summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(f"FAIL: {error}", file=sys.stderr)
        print(f"summary: {work / 'summary.json'}", file=sys.stderr)
        return 1
    except Exception as error:  # configuration or unexpected host failure
        summary["error"] = f"{type(error).__name__}: {error}"
        summary["status"] = "failed"
        (work / "summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(f"FAIL: {type(error).__name__}: {error}", file=sys.stderr)
        print(f"summary: {work / 'summary.json'}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
