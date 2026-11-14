#!/usr/bin/env python3
import argparse
import hashlib
import json
import math
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "selfhost"))
import corpus  # noqa: E402  (public-object mode reuses its bounded process helper)

WRAPS = [
    "lang_async_task_new_v1",
    "lang_async_sleep_v1",
    "lang_async_wait_fd_v1",
    "lang_async_frame_alloc_v1",
    "lang_async_frame_free_v1",
    "lang_free_v1",
    "lang_async_task_retain_v1",
    "lang_async_task_release_v1",
    "lang_gc_program_finish_v1",
]


class CommandFailure(Exception):
    def __init__(self, message, stdout, stderr, log_dir, timed_out=False):
        super().__init__(message)
        self.stdout = stdout
        self.stderr = stderr
        self.log_dir = log_dir
        self.timed_out = timed_out


def run_command(command, env, timeout, log_dir, label):
    log_dir.mkdir(parents=True, exist_ok=True)
    stdout_path = log_dir / f"{label}.stdout"
    stderr_path = log_dir / f"{label}.stderr"
    with open(stdout_path, "w") as stdout, open(stderr_path, "w") as stderr:
        process = subprocess.Popen(
            [str(part) for part in command],
            env=env,
            stdout=stdout,
            stderr=stderr,
            text=True,
            start_new_session=True,
        )
        timed_out = False
        try:
            status = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            status = process.wait()
            (log_dir / f"{label}.timeout").write_text(
                f"timeout after {timeout}s: {' '.join(map(str, command))}\n"
            )
    captured_stdout = stdout_path.read_text(errors="replace")
    captured_stderr = stderr_path.read_text(errors="replace")
    return status, timed_out, captured_stdout, captured_stderr


def report_failure(title, log_dir, stdout, stderr, timed_out=False):
    print(f"FAILED: {title}")
    if timed_out:
        print("reason: command timed out; process group killed")
    print(f"diagnostics: {log_dir}")
    if stdout:
        print("STDOUT:", stdout)
    if stderr:
        print("STDERR:", stderr)


def run_checked(command, env, timeout, log_dir, label, title):
    status, timed_out, stdout, stderr = run_command(
        command, env, timeout, log_dir, label
    )
    if timed_out or status != 0:
        report_failure(title, log_dir, stdout, stderr, timed_out)
        raise SystemExit(1)
    return stdout, stderr


def run_program(program, env, timeout, log_dir, title):
    status, timed_out, stdout, stderr = run_command(
        [program], env, timeout, log_dir, "run"
    )
    if timed_out:
        report_failure(title, log_dir, stdout, stderr, True)
        raise SystemExit(1)
    return status, stdout, stderr


def parse_counts(stderr):
    match = re.search(r"COUNTS ([^\n]+)", stderr)
    if not match:
        return None
    return {
        key: int(value)
        for key, value in re.findall(r"([A-Za-z_]+)=(\d+)", match.group(1))
    }


def check_observed(title, status, stdout, stderr, case, log_dir):
    expected_exit = case.get("exit", 0)
    if status != expected_exit:
        report_failure(
            f"{title}: exit {status}, expected {expected_exit}", log_dir, stdout, stderr
        )
        raise SystemExit(1)
    if "stdout" in case and stdout != case["stdout"]:
        report_failure(
            f"{title}: stdout mismatch\nEXPECTED:\n{case['stdout']}\nGOT:\n{stdout}",
            log_dir,
            stdout,
            stderr,
        )
        raise SystemExit(1)
    counts = parse_counts(stderr)
    if counts is None:
        report_failure(f"{title}: observer COUNTS line missing", log_dir, stdout, stderr)
        raise SystemExit(1)
    for allocated, freed in (("future_new", "future_free"),
                             ("frame_new", "frame_free"),
                             ("vec_new", "vec_free")):
        if allocated not in counts or counts[allocated] != counts.get(freed):
            report_failure(f"{title}: unbalanced {allocated}/{freed}: {counts}",
                           log_dir, stdout, stderr)
            raise SystemExit(1)
    expected = case["counts"]
    if any(counts.get(key) != value for key, value in expected.items()):
        report_failure(
            f"{title}: counts mismatch\nEXPECTED: {expected}\nGOT: {counts}",
            log_dir,
            stdout,
            stderr,
        )
        raise SystemExit(1)
    return counts


def build_regular_cases(cli, root, out_base, env, catalog, build_timeout, run_timeout):
    passed = 0
    for opt in ("0", "2"):
        for case in catalog.get("positive", []):
            name = case["name"]
            manifest = root / "tests" / "async" / name / "lang.toml"
            log_dir = out_base / f"{name}_O{opt}"
            output_dir = log_dir / "build"
            command = [
                cli,
                "build",
                "--project",
                manifest,
                "--backend",
                "native",
                "--opt",
                opt,
                "--out-dir",
                output_dir,
            ]
            run_checked(
                command,
                env,
                build_timeout,
                log_dir,
                "build",
                f"positive case '{name}' O{opt} build",
            )
            status, stdout, stderr = run_program(
                output_dir / "program",
                env,
                run_timeout,
                log_dir,
                f"positive case '{name}' O{opt}",
            )
            expected_exit = case.get("exit", 0)
            expected_stdout = case.get("stdout")
            if status != expected_exit:
                report_failure(
                    f"positive case '{name}' O{opt}: exit {status}, expected {expected_exit}",
                    log_dir,
                    stdout,
                    stderr,
                )
                raise SystemExit(1)
            if expected_stdout is not None and stdout != expected_stdout:
                report_failure(
                    f"positive case '{name}' O{opt}: stdout mismatch\n"
                    f"EXPECTED:\n{expected_stdout}\nGOT:\n{stdout}",
                    log_dir,
                    stdout,
                    stderr,
                )
                raise SystemExit(1)
            if "stderr" in case and stderr != case["stderr"]:
                report_failure(
                    f"positive case '{name}' O{opt}: stderr mismatch\n"
                    f"EXPECTED:\n{case['stderr']}\nGOT:\n{stderr}",
                    log_dir,
                    stdout,
                    stderr,
                )
                raise SystemExit(1)
            print(f"  [PASS] positive (O{opt}): {name}")
            passed += 1
    return passed


def build_reject_cases(cli, root, out_base, env, catalog, build_timeout):
    passed = 0
    for case in catalog.get("reject", []):
        name = case["name"]
        code = case["code"]
        manifest = root / "tests" / "async" / "reject" / name / "lang.toml"
        log_dir = out_base / "reject" / name
        command = [
            cli,
            "build",
            "--project",
            manifest,
            "--backend",
            "native",
            "--opt",
            "0",
            "--out-dir",
            log_dir / "build",
        ]
        status, timed_out, stdout, stderr = run_command(
            command, env, build_timeout, log_dir, "build"
        )
        if timed_out:
            report_failure(
                f"reject case '{name}' build timed out", log_dir, stdout, stderr, True
            )
            raise SystemExit(1)
        if status == 0:
            report_failure(
                f"reject case '{name}' built successfully, expected '{code}'",
                log_dir,
                stdout,
                stderr,
            )
            raise SystemExit(1)
        if code not in stdout + stderr:
            report_failure(
                f"reject case '{name}' did not report '{code}'",
                log_dir,
                stdout,
                stderr,
            )
            raise SystemExit(1)
        print(f"  [PASS] reject: {name} (expected {code})")
        passed += 1
    return passed


def cached_cxx(cli):
    cache = cli.parent / "CMakeCache.txt"
    if not cache.exists():
        return None
    match = re.search(
        r"^CMAKE_CXX_COMPILER:[^=]+=(.+)$", cache.read_text(), re.MULTILINE
    )
    return match.group(1).strip() if match else None


def observer_paths(args, cli):
    helper = args.helper or cli.parent / "native" / "lang_native_helper"
    runtime = args.runtime or cli.parent / "native" / "liblang_native_runtime.a"
    clang = args.clang
    if clang is None:
        clang = os.environ.get("LANG_CLANGXX") or cached_cxx(cli)
    if clang is None:
        clang = shutil.which("clang++")
    ld = args.ld
    if ld is None and clang is not None:
        candidate = Path(clang).parent / "ld.lld"
        if candidate.exists():
            ld = candidate
    if ld is None:
        ld = shutil.which("ld.lld")
    return Path(helper), Path(runtime), Path(clang) if clang else None, (
        Path(ld) if ld else None
    )


def build_observed_cases(args, cli, root, out_base, env, catalog, build_timeout,
                         link_timeout, run_timeout):
    cases = catalog.get("observed", [])
    if not cases or args.observe == "off":
        return 0, False

    helper, runtime, clang, ld = observer_paths(args, cli)
    observer = args.observer or root / "tests" / "async" / "async_observer.cpp"
    missing = [
        str(path)
        for path in (helper, runtime, clang, ld, observer)
        if path is None or not path.exists()
    ]
    if missing:
        message = "observer prerequisites missing: " + ", ".join(missing)
        if args.observe == "on":
            print(f"FAILED: {message}")
            raise SystemExit(1)
        print(f"  [SKIP] observed cases: {message}")
        return 0, False

    observer_object = out_base / "observer.o"
    run_checked(
        [
            clang,
            "-std=c++20",
            "-O2",
            "-ffreestanding",
            "-fno-builtin",
            "-fno-exceptions",
            "-fno-rtti",
            "-fno-stack-protector",
            "-c",
            observer,
            "-o",
            observer_object,
        ],
        env,
        build_timeout,
        out_base / "observer",
        "build",
        "observer build",
    )

    passed = 0
    for opt in ("0", "2"):
        for case in cases:
            name = case["name"]
            manifest = root / "tests" / "async" / name / "lang.toml"
            log_dir = out_base / f"{name}_observe_O{opt}"
            object_path = log_dir / "program.o"
            llvm_path = log_dir / "program.ll"
            program_path = log_dir / "program"
            run_checked(
                [
                    helper,
                    "--project",
                    manifest,
                    "--opt",
                    opt,
                    "--out-object",
                    object_path,
                    "--out-llvm",
                    llvm_path,
                ],
                env,
                build_timeout,
                log_dir,
                "build",
                f"observed case '{name}' O{opt} build",
            )
            link_command = [
                clang,
                "--target=x86_64-unknown-linux-gnu",
                "-nostdlib",
                "-static",
                f"--ld-path={ld}",
                object_path,
                observer_object,
                runtime,
            ]
            link_command.extend(f"-Wl,--wrap={symbol}" for symbol in WRAPS)
            link_command.extend(["-o", program_path])
            run_checked(
                link_command,
                env,
                link_timeout,
                log_dir,
                "link",
                f"observed case '{name}' O{opt} link",
            )
            status, stdout, stderr = run_program(
                program_path,
                env,
                run_timeout,
                log_dir,
                f"observed case '{name}' O{opt}",
            )
            counts = check_observed(
                f"observed case '{name}' O{opt}", status, stdout, stderr, case, log_dir
            )
            print(
                f"  [PASS] observed (O{opt}): {name} "
                f"future={counts['future_new']}/{counts['future_free']} "
                f"frame={counts['frame_new']}/{counts['frame_free']}"
            )
            passed += 1
    return passed, True


PUBLIC_OBJECT_SCHEMA = "woozy/async-public-object-proposal/1"
# Historical comparison only; new compiler sources need not have this digest.
SOURCE116_FROZEN = "727b72dd473a5b2c6b2e9212e7d496131db9141bc4c9371ba60d25ddbf6eefef"
OBSERVED_LIMIT = 12


class PublicFailure(Exception):
    """A public-object run assertion failed; the run continues."""


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_public_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n",
                    encoding="utf-8")


def write_public_bytes(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(value)


def validate_timeout(value, name):
    """Timeout inputs must be positive finite numbers, never bool/string."""
    if type(value) is bool or not isinstance(value, (int, float)):
        raise PublicFailure(f"{name} timeout is not a number: {value!r}")
    if not math.isfinite(float(value)) or float(value) <= 0.0:
        raise PublicFailure(f"{name} timeout is not positive finite: {value!r}")
    return float(value)


def validate_opt(value):
    if type(value) is bool or value not in (0, 2):
        raise PublicFailure(f"optimization level must be 0 or 2: {value!r}")
    return str(value)


def validate_process_record(record, where):
    """Validate the real process fields; bool is not accepted as an int exit."""
    if not isinstance(record, dict):
        raise PublicFailure(f"{where}: process record is not an object")
    if type(record.get("exit")) is not int:
        raise PublicFailure(f"{where}: exit is not a real int: {record.get('exit')!r}")
    for flag in ("timed_out", "cleanup_complete", "abandoned_pipes"):
        if type(record.get(flag)) is not bool:
            raise PublicFailure(f"{where}: {flag} is not a strict bool: "
                                f"{record.get(flag)!r}")
    return record


def validate_successful_process(record, where):
    """Accept a command as successful only with clean timeout/cleanup flags.

    execute_public_command still records failures with their real raw output and
    flags; this gate is applied only where a result would be counted as passed.
    """
    validate_process_record(record, where)
    if record["timed_out"] is not False:
        raise PublicFailure(f"{where}: timed out")
    if record["cleanup_complete"] is not True:
        raise PublicFailure(
            f"{where}: cleanup did not complete: {record['cleanup_complete']!r}")
    if record["abandoned_pipes"] is not False:
        raise PublicFailure(
            f"{where}: output pipes were abandoned: {record['abandoned_pipes']!r}")
    return record


def parse_json_lines(stdout, stderr):
    documents = []
    for line in (stdout + "\n" + stderr).splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            value = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict):
            documents.append(value)
    return documents


def check_program_expectation(record, case, where):
    validate_successful_process(record, where)
    expected_exit = case.get("exit", 0)
    if type(expected_exit) is not int:
        raise PublicFailure(f"{where}: expected exit is not an int: {expected_exit!r}")
    if record["exit"] != expected_exit:
        raise PublicFailure(
            f"{where}: exit {record['exit']}, expected {expected_exit}")
    stdout = record.get("_stdout", record.get("stdout", b"")).decode(
        "utf-8", "replace")
    stderr = record.get("_stderr", record.get("stderr", b"")).decode(
        "utf-8", "replace")
    if "stdout" in case and stdout != case["stdout"]:
        raise PublicFailure(
            f"{where}: stdout mismatch\nEXPECTED:\n{case['stdout']}\nGOT:\n{stdout}")
    if "stderr" in case and stderr != case["stderr"]:
        raise PublicFailure(
            f"{where}: stderr mismatch\nEXPECTED:\n{case['stderr']}\nGOT:\n{stderr}")
    return stdout, stderr


def observed_counts(stderr_text, status, stdout, case, where):
    expected_exit = case.get("exit", 0)
    if type(expected_exit) is not int:
        raise PublicFailure(f"{where}: expected exit is not an int: {expected_exit!r}")
    if status != expected_exit:
        raise PublicFailure(f"{where}: exit {status}, expected {expected_exit}")
    if "stdout" in case and stdout != case["stdout"]:
        raise PublicFailure(
            f"{where}: stdout mismatch\nEXPECTED:\n{case['stdout']}\nGOT:\n{stdout}")
    counts = parse_counts(stderr_text)
    if counts is None:
        raise PublicFailure(f"{where}: observer COUNTS line missing")
    for allocated, freed in (("future_new", "future_free"),
                             ("frame_new", "frame_free"),
                             ("vec_new", "vec_free")):
        if allocated not in counts or counts[allocated] != counts.get(freed):
            raise PublicFailure(
                f"{where}: unbalanced {allocated}/{freed}: {counts}")
    expected = case.get("counts")
    if not isinstance(expected, dict) or not expected:
        raise PublicFailure(f"{where}: case counts are missing or empty")
    for key, value in expected.items():
        if type(value) is not int:
            raise PublicFailure(f"{where}: expected count {key} is not an int")
        if counts.get(key) != value:
            raise PublicFailure(
                f"{where}: counts mismatch\nEXPECTED: {expected}\nGOT: {counts}")
    return counts


def prepare_public_output(path):
    if path.exists():
        if not path.is_dir() or path.is_symlink():
            raise PublicFailure(f"output path is not a directory: {path}")
        if any(path.iterdir()):
            raise PublicFailure(f"output directory is not empty: {path}")
    else:
        path.mkdir(parents=True, exist_ok=True)
    return path


def safe_label(value):
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", value)


def tree_manifest(root):
    root = Path(root).resolve()
    if not root.is_dir():
        return {"path": str(root), "exists": False, "file_count": 0,
                "map_sha256": None, "files": {}}
    files = {
        str(path.relative_to(root)): sha256_file(path)
        for path in sorted(root.rglob("*")) if path.is_file()
    }
    digest = sha256_bytes(json.dumps(
        files, sort_keys=True, separators=(",", ":")).encode("utf-8"))
    return {"path": str(root), "exists": True, "file_count": len(files),
            "map_sha256": digest, "files": files}


def source_map_digest(root):
    return tree_manifest(root)["map_sha256"]


def validate_source_identity(root_source, candidate_source):
    for label, source in (("Root", root_source),
                          ("candidate", candidate_source)):
        if (not isinstance(source, dict) or
                type(source.get("file_count")) is not int or
                source["file_count"] <= 0 or not source.get("map_sha256")):
            raise PublicFailure(f"{label} frozen Source map is missing or empty")
    if root_source["map_sha256"] != candidate_source["map_sha256"]:
        raise PublicFailure("candidate frozen Source differs from Root Source")


def add_identity(entries, path, label):
    path = Path(path)
    if not path.is_file():
        raise PublicFailure(f"identity input is not a file: {label}: {path}")
    entries[label] = {"path": str(path.resolve()),
                      "size": path.stat().st_size,
                      "sha256": sha256_file(path)}


def snapshot_inputs(context, cases):
    """Take the before/after digest set for every declared input."""
    args = context["args"]
    root = context["root"]
    entries = {}
    reject_names = {case["name"] for case in cases.get("reject", [])}
    for case in cases.get("positive", []) + cases.get("reject", []):
        base = root / "tests" / "async"
        is_reject = case["name"] in reject_names
        manifest = (base / "reject" / case["name"] / "lang.toml"
                    if is_reject else base / case["name"] / "lang.toml")
        source = manifest.parent / "src" / "main.wz"
        add_identity(entries, manifest, f"manifest:{manifest.relative_to(root)}")
        add_identity(entries, source, f"source:{source.relative_to(root)}")
    add_identity(entries, root / "tests/async/cases.json", "cases.json")
    add_identity(entries, context["observer"], "observer.cpp")
    add_identity(entries, Path(__file__), "runner")
    entries["wraps"] = {
        "count": len(WRAPS),
        "symbols": list(WRAPS),
        "sha256": sha256_bytes(json.dumps(
            WRAPS, separators=(",", ":")).encode("utf-8")),
    }
    add_identity(entries, context["runtime"], "runtime")
    for header in ("vec.hpp", "async.hpp", "gc_program.hpp"):
        add_identity(entries, root / "runtime/native" / header,
                     f"header:{header}")
    add_identity(entries, context["cli"], "cli")
    add_identity(entries, context["strace"], "strace")
    add_identity(entries, context["python"], "python")
    add_identity(entries, context["corpus"], "corpus.py")
    if context["libxml2"] is not None:
        libxml2 = tree_manifest(context["libxml2"])
        entries["libxml2"] = {
            "path": libxml2["path"],
            "file_count": libxml2["file_count"],
            "map_sha256": libxml2["map_sha256"],
            "files": libxml2["files"],
        }
    for name, path in context["tools"].items():
        add_identity(entries, path, f"tool:{name}")
    for index, identity in enumerate(args.identity or []):
        add_identity(entries, identity, f"identity:{index}:{Path(identity).name}")
    for name, path in context["trace_allow"].items():
        add_identity(entries, path, f"trace_allow:{name}")
    bindings = sorted((root / "tests/async").rglob("binding.json"))
    entries["bindings"] = {
        "count": len(bindings),
        "files": {str(path.relative_to(root)): sha256_file(path)
                  for path in bindings},
        "note": "tests/async has no binding.json files",
    }
    stdlib = tree_manifest(context["stdlib_root"])
    stdlib_wz = {key: value for key, value in stdlib["files"].items()
                 if key.endswith(".wz")}
    entries["stdlib37"] = {
        "path": stdlib["path"],
        "file_count": len(stdlib_wz),
        "all_file_count": stdlib["file_count"],
        "map_sha256": sha256_bytes(json.dumps(
            stdlib_wz, sort_keys=True, separators=(",", ":")).encode("utf-8")),
        "expected_file_count": 37,
        "files": stdlib_wz,
    }
    root_source = tree_manifest(root / "selfhost/src")
    entries["source116_root"] = {
        "path": root_source["path"],
        "file_count": root_source["file_count"],
        "map_sha256": root_source["map_sha256"],
        "matches_frozen_727b": root_source["map_sha256"] == SOURCE116_FROZEN,
        "scope": "self-host compiler sources in this checked-out Root; not the async fixtures",
    }
    if args.candidate_source is not None:
        candidate_source = tree_manifest(args.candidate_source)
        entries["source116_candidate"] = {
            "path": candidate_source["path"],
            "file_count": candidate_source["file_count"],
            "map_sha256": candidate_source["map_sha256"],
            "matches_frozen_727b": candidate_source["map_sha256"] == SOURCE116_FROZEN,
            "scope": "frozen candidate self-host source argument; not read by the CLI in this run",
        }
    return entries


def public_tool_paths(args, clang, ld):
    if args.toolchain is not None:
        tool_bin = Path(args.toolchain)
        if (tool_bin / "bin").is_dir():
            tool_bin = tool_bin / "bin"
    else:
        tool_bin = Path(clang).parent
    tools = {}
    for name in corpus.TOOL_NAMES:
        path = tool_bin / name
        if path.exists():
            tools[name] = path.resolve()
    tools["clang"] = Path(clang).resolve()
    tools["ld.lld"] = Path(ld).resolve()
    return tools


def public_prerequisites(args, repo_root):
    """Resolve public-mode executables and fail closed when anything is absent."""
    if args.helper is not None:
        raise PublicFailure("public-object mode never accepts --helper")
    if args.role is None:
        raise PublicFailure("public-object mode requires --role")
    cli = Path(args.cli)
    if not cli.is_file():
        raise PublicFailure(f"public CLI is missing: {cli}")
    runtime = Path(args.runtime) if args.runtime else None
    if runtime is None or not runtime.is_file():
        raise PublicFailure(f"runtime is missing: {runtime}")
    strace = Path(args.strace) if args.strace else None
    if strace is None or not strace.is_file():
        raise PublicFailure(f"strace is missing: {strace}")
    if args.clang is not None:
        clang = Path(args.clang)
    elif args.toolchain is not None:
        clang = Path(args.toolchain) / "bin" / "clang"
    else:
        raise PublicFailure("public-object mode requires --clang or --toolchain")
    if args.ld is not None:
        ld = Path(args.ld)
    elif args.toolchain is not None:
        ld = Path(args.toolchain) / "bin" / "ld.lld"
    else:
        ld = clang.parent / "ld.lld"
    if not clang.is_file() or not ld.is_file():
        raise PublicFailure(f"observer toolchain is missing: {clang} / {ld}")
    observer = (Path(args.observer) if args.observer else
                repo_root / "tests/async/async_observer.cpp")
    observe = require_observer_for_public(args.observe, observer)
    if args.role == "candidate" and args.trace_allow:
        raise PublicFailure("candidate public-object mode forbids --trace-allow")
    trace_allow = {}
    if args.role == "reference":
        if len(args.trace_allow or []) != 1:
            raise PublicFailure(
                "reference public-object mode requires exactly one matching helper")
        helper = Path(args.trace_allow[0]).resolve()
        if helper.name != "lang_native_helper":
            raise PublicFailure(f"reference trace helper is not matching: {helper}")
        if helper.parent != cli.resolve().parent / "native":
            raise PublicFailure(
                f"reference trace helper is not beside the CLI: {helper}")
        trace_allow[helper.name] = helper
    tools = public_tool_paths(args, clang, ld)
    stdlib_root = None
    if args.stdlib_root is not None:
        stdlib_root = Path(args.stdlib_root)
    else:
        for candidate in (cli.parent / "stdlib", cli.parent.parent / "stdlib"):
            if candidate.is_dir():
                stdlib_root = candidate
                break
    if stdlib_root is None or not stdlib_root.is_dir():
        raise PublicFailure("cannot locate the role stdlib root; pass --stdlib-root")
    libxml2 = Path(args.libxml2_lib) if args.libxml2_lib else None
    if libxml2 is not None and not libxml2.is_dir():
        raise PublicFailure(f"libxml2 library directory is missing: {libxml2}")
    python = Path(args.python) if args.python else Path(sys.executable)
    if not python.is_file():
        raise PublicFailure(f"python is missing: {python}")
    return {
        "cli": cli.resolve(),
        "runtime": runtime.resolve(),
        "strace": strace.resolve(),
        "clang": clang,
        "ld": ld,
        "observer": observer.resolve(),
        "observe": observe,
        "tools": tools,
        "trace_allow": trace_allow,
        "stdlib_root": stdlib_root.resolve(),
        "libxml2": libxml2.resolve() if libxml2 else None,
        "python": python.resolve(),
        "corpus": (repo_root / "selfhost/corpus.py").resolve(),
    }


def resolve_public_object(build_dir, name, opt):
    """Return the exact object produced by this case's public build."""
    opt = validate_opt(int(opt) if type(opt) is not bool else opt)
    build_dir = Path(build_dir)
    expected_name = f"{name}_O{opt}"
    if build_dir.name != expected_name:
        raise PublicFailure(
            f"wrong object directory for {name} O{opt}: {build_dir}")
    object_path = build_dir / "program.o"
    if not object_path.is_file() or object_path.is_symlink():
        raise PublicFailure(f"public object is missing: {object_path}")
    return object_path


def execute_public_command(context, argv, label, kind, timeout,
                           trace_allowed=None):
    index = context["next_command"]
    context["next_command"] += 1
    timeout = validate_timeout(timeout, kind)
    argv = [str(part) for part in argv]
    command_dir = context["out"] / "commands" / f"{index:03d}-{safe_label(label)}"
    command_dir.mkdir(parents=True, exist_ok=False)
    trace_path = command_dir / "exec.log"
    spawn_error = None
    if trace_allowed is not None:
        traced = [str(context["strace"]), "--seccomp-bpf", "-f", "-qq",
                  "-s", "16384", "-e", "trace=execve,execveat",
                  "-o", str(trace_path), *argv]
        wrapper = traced
    else:
        wrapper = None
    started = time.time()
    try:
        process = corpus._run_process_impl(
            wrapper or argv, context["root"], context["env"], timeout)
    except OSError as error:
        spawn_error = str(error)
        process = {"exit": 127, "stdout": b"", "stderr": str(error).encode(),
                   "timed_out": False, "cleanup_complete": True,
                   "abandoned_pipes": False}
    elapsed = time.time() - started
    validate_process_record(process, f"{kind}:{label}")
    stdout = process["stdout"]
    stderr = process["stderr"]
    write_public_bytes(command_dir / "stdout.bin", stdout)
    write_public_bytes(command_dir / "stderr.bin", stderr)
    trace = None
    if trace_allowed is not None:
        if spawn_error is not None or not trace_path.is_file():
            trace = {"status": "not_run" if spawn_error else "missing",
                     "allowed": sorted(str(path) for path in trace_allowed)}
        else:
            try:
                trace = corpus.audit_trace(
                    trace_path, [str(path) for path in trace_allowed])
                trace["path"] = str(trace_path)
            except Exception as error:  # TraceError/CorpusError must fail closed
                trace = {"status": "failed", "error": str(error),
                         "path": str(trace_path),
                         "allowed": sorted(str(path) for path in trace_allowed)}
    record = {
        "index": index,
        "label": label,
        "kind": kind,
        "argv": argv,
        "cwd": str(context["root"]),
        "timeout": timeout,
        "elapsed_seconds": round(elapsed, 6),
        "exit": process["exit"],
        "timed_out": process["timed_out"],
        "cleanup_complete": process["cleanup_complete"],
        "abandoned_pipes": process["abandoned_pipes"],
        "spawn_error": spawn_error,
        "stdout_sha256": sha256_bytes(stdout),
        "stderr_sha256": sha256_bytes(stderr),
        "stdout_path": "stdout.bin",
        "stderr_path": "stderr.bin",
        "trace_scope": ("source compile exact role allowlist" if trace_allowed
                        is not None else "test-only observer step; not a Source allowlist"),
        "trace": trace,
        "directory": str(command_dir.relative_to(context["out"])),
    }
    write_public_json(command_dir / "command.json", record)
    context["commands"].append(record)
    record["_stdout"] = stdout
    record["_stderr"] = stderr
    return record



def require_observer_for_public(observe, observer_path):
    """auto is not a skip in public-object mode; missing tools fail closed."""
    if observe == "off":
        return "off"
    if not Path(observer_path).is_file():
        raise PublicFailure(f"observer source is missing: {observer_path}")
    return "on"


def case_manifest(root, case, reject=False):
    base = root / "tests/async"
    if reject:
        return base / "reject" / case["name"] / "lang.toml"
    return base / case["name"] / "lang.toml"


def validate_positive_build(context, command, name, opt, build_dir):
    validate_successful_process(command, f"positive build {name} O{opt}")
    if command["exit"] != 0:
        raise PublicFailure(
            f"positive build {name} O{opt}: exit {command['exit']}")
    trace = command.get("trace")
    if not trace or trace.get("status") != "ok":
        raise PublicFailure(
            f"positive build {name} O{opt}: source trace failed: {trace}")
    documents = parse_json_lines(
        command["_stdout"].decode("utf-8", "replace"),
        command["_stderr"].decode("utf-8", "replace"))
    if not documents or documents[-1].get("status") != "ok":
        raise PublicFailure(
            f"positive build {name} O{opt}: status is not ok: "
            f"{documents[-1] if documents else 'no json'}")
    published = {}
    for filename in ("program.o", "program"):
        path = build_dir / filename
        if not path.is_file() or path.is_symlink():
            raise PublicFailure(
                f"positive build {name} O{opt}: missing {filename}")
        published[filename] = {
            "path": str(path),
            "size": path.stat().st_size,
            "sha256": sha256_file(path),
        }
        context["artifacts"][str(path)] = published[filename]["sha256"]
    return {
        "status": "passed",
        "command": command["index"],
        "compiler_version": documents[-1].get("compiler_version"),
        "trace": trace,
        "published": published,
    }


def validate_reject_build(context, command, case, build_dir):
    name = case["name"]
    code = case["code"]
    validate_successful_process(command, f"reject build {name}")
    if command["exit"] <= 0:
        raise PublicFailure(
            f"reject build {name}: not a normal nonzero exit: {command['exit']}")
    combined = (command["_stdout"] + command["_stderr"]).decode(
        "utf-8", "replace")
    if code not in combined:
        raise PublicFailure(
            f"reject build {name}: did not report {code!r}")
    documents = parse_json_lines(
        command["_stdout"].decode("utf-8", "replace"),
        command["_stderr"].decode("utf-8", "replace"))
    if documents:
        document = documents[-1]
        if document.get("status") != "error":
            raise PublicFailure(
                f"reject build {name}: JSON status is not error")
        diagnostics = document.get("diagnostics") or []
        codes = [item.get("code") for item in diagnostics
                 if isinstance(item, dict)]
        if code not in codes:
            raise PublicFailure(
                f"reject build {name}: JSON diagnostics lack {code}: {codes}")
    trace = command.get("trace")
    if not trace or trace.get("status") != "ok":
        raise PublicFailure(
            f"reject build {name}: source trace failed: {trace}")
    published = {}
    for filename in ("program", "program.o"):
        path = build_dir / filename
        if path.exists() or path.is_symlink():
            raise PublicFailure(
                f"reject build {name}: published forbidden artifact {filename}")
        published[filename] = False
    return {"status": "passed", "command": command["index"],
            "code": code, "trace": trace, "published": published}


def proposal_case_adaptation(name, manifest, results, observed):
    """Derive ready only from this case's required actual results."""
    if "/reject/" in manifest:
        required = [results["reject_results"].get(name)]
    else:
        required = [results["positive_results"].get((name, opt))
                    for opt in ("0", "2")]
        if name in observed:
            required += [results["observed_results"].get((name, opt))
                         for opt in ("0", "2")]
    return ("ready" if all(isinstance(row, dict) and
                           row.get("status") == "passed"
                           for row in required) else "pending")


def write_public_proposal(args, root, out, results, summary):
    corpus_json = root / "selfhost/corpus.json"
    if not corpus_json.is_file():
        raise PublicFailure(f"catalog is missing: {corpus_json}")
    catalog = json.loads(corpus_json.read_text(encoding="utf-8"))
    async_manifests = [
        str(case_manifest(root, case, False)).replace(str(root) + "/", "")
        for case in results["positive"]
    ] + [
        str(case_manifest(root, case, True)).replace(str(root) + "/", "")
        for case in results["reject"]
    ]
    rows = [row for row in catalog.get("cases", [])
            if row.get("manifest") in async_manifests]
    by_manifest = {}
    for row in rows:
        by_manifest.setdefault(row.get("manifest"), []).append(row)
    missing = [manifest for manifest in async_manifests
               if manifest not in by_manifest]
    if missing:
        raise PublicFailure(f"catalog lacks existing rows for: {missing}")
    observed = {case["name"] for case in results["observed"]}
    proposal_cases = []
    for manifest in async_manifests:
        candidates = by_manifest[manifest]
        row = candidates[0]
        name = Path(manifest).parent.name
        if "/reject/" in manifest:
            ordinary = results["reject_results"].get(name)
            observed_status = "not_in_original_observed_subset"
        else:
            ordinary = {
                opt: results["positive_results"].get((name, opt))
                for opt in ("0", "2")
            }
            observed_status = ({
                opt: results["observed_results"].get((name, opt))
                for opt in ("0", "2")
            } if name in observed else "not_in_original_observed_subset")
        adaptation = proposal_case_adaptation(
            name, manifest, results, observed)
        proposal_cases.append({
            "id": row["id"],
            "manifest": manifest,
            "existing": {
                "enabled": row.get("enabled"),
                "support": row.get("support"),
                "adaptation": row.get("adaptation"),
                "runner_status": row.get("runner_status"),
            },
            "proposed": {
                "enabled": False,
                "required": False,
                "support": "existing_runner",
                "adaptation": adaptation,
                "runner": "tests/async/run_async_cases.py",
                "runner_status": "public_cli",
            },
            "ordinary": ordinary,
            "observed": observed_status,
            "observation_schema": (
                "not expressible in corpus RUN_EXPECTATION_KEYS; "
                "exact COUNTS remain separate runner evidence"
                if name in observed else "not in original observed subset"),
            "aliases": {
                "existing_case_ids": [item["id"] for item in candidates],
                "enabled_case_ids": [item["id"] for item in candidates
                                     if item.get("enabled")],
            },
        })
    ids = [item["id"] for item in proposal_cases]
    if len(ids) != len(set(ids)):
        raise PublicFailure("proposal accidentally appended duplicate case ids")
    runner_rows = [row for row in catalog.get("runners", [])
                   if row.get("path") == "tests/async/run_async_cases.py"]
    proposal = {
        "schema": PUBLIC_OBJECT_SCHEMA,
        "source": "SH12s async public-object migration",
        "role": args.role,
        "enabled": False,
        "case_id_count": len(proposal_cases),
        "only_existing_ids_updated": True,
        "append_duplicates": False,
        "runner": {
            "existing": runner_rows[0] if runner_rows else None,
            "proposed": {
                "path": "tests/async/run_async_cases.py",
                "status": "public_cli",
                "reason": (
                    "public-object mode uses the role CLI for all 44 Source "
                    "builds and reuses those objects for 24 observer links; "
                    "legacy helper mode is retained"),
            },
        },
        "cases": proposal_cases,
        "static_ordinary_execution": {
            "all_25_existing_rows_currently_enabled": False,
            "enabled_alias_case_ids_for_these_manifests": [
                alias for item in proposal_cases
                for alias in item["aliases"]["enabled_case_ids"]],
            "note": (
                "A static ordinary build/execute row is not a claim that the "
                "12 exact observer COUNTS fields are represented in the "
                "catalog schema."),
        },
        "schema_limitations": [
            "Corpus RUN_EXPECTATION_KEYS do not include future/frame/Vec "
            "COUNTS, bad_frame_free, or Task/Vec retain/release fields.",
            "The 12 observer results remain separate runner evidence in "
            f"{out} and are not appended as duplicate catalog cases.",
        ],
        "evidence": {
            "summary": str(out / "summary.json"),
            "summary_sha256": summary.get("summary_sha256"),
        },
        "notes": [
            "Proposal only; Root catalog is not modified by this runner.",
            "Only the 25 pre-existing project ids are listed; no append.",
            "Observer shim compilation/link/execution is not part of the "
            "Source exec allowlist.",
        ],
    }
    write_public_json(args.proposal_out, proposal)
    return proposal


def public_summary_status(failures, checks_passed, expected_checks, watched,
                          timed_out, cleanup_incomplete, abandoned):
    """The final summary cannot pass with timeouts or dirty cleanup state."""
    if not watched:
        return "incomplete"
    if (failures or checks_passed != expected_checks or timed_out != 0 or
            cleanup_incomplete != 0 or abandoned != 0):
        return "failed"
    return "passed"


def run_public_mode(args, repo_root):
    validate_timeout(args.build_timeout, "build")
    validate_timeout(args.link_timeout, "link")
    validate_timeout(args.run_timeout, "run")
    started_at = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    context = public_prerequisites(args, repo_root)
    if context["observe"] != "off":
        context["observe"] = require_observer_for_public(
            args.observe, context["observer"])
    out = prepare_public_output(Path(args.out))
    cases_file = repo_root / "tests/async/cases.json"
    cases = json.loads(cases_file.read_text(encoding="utf-8"))
    positives = cases.get("positive", [])
    rejects = cases.get("reject", [])
    observed_cases = cases.get("observed", [])
    if (len(positives) != 19 or len(rejects) != 6 or
            len(observed_cases) != OBSERVED_LIMIT):
        raise PublicFailure(
            f"unexpected catalog scope: {len(positives)} positive, "
            f"{len(rejects)} reject, {len(observed_cases)} observed")
    observed_names = {case["name"] for case in observed_cases}
    positive_names = {case["name"] for case in positives}
    if not observed_names <= positive_names:
        raise PublicFailure("observed subset is not contained in positives")
    env = os.environ.copy()
    if context["libxml2"] is not None:
        previous = env.get("LD_LIBRARY_PATH")
        env["LD_LIBRARY_PATH"] = (str(context["libxml2"]) +
                                  ((":" + previous) if previous else ""))
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    context.update({
        "root": repo_root,
        "out": out,
        "env": env,
        "args": args,
        "role": args.role,
        "next_command": 0,
        "commands": [],
        "artifacts": {},
        "failures": [],
    })
    allowed = {str(context["cli"])}
    allowed.update(str(path) for path in context["tools"].values())
    allowed.update(str(path) for path in context["trace_allow"].values())
    before = snapshot_inputs(context, cases)
    write_public_json(out / "inputs.before.json", before)
    candidate_source = before.get("source116_candidate")
    if args.role == "candidate" or candidate_source is not None:
        try:
            validate_source_identity(before["source116_root"], candidate_source)
        except PublicFailure as error:
            context["failures"].append({
                "where": "source116_candidate", "error": str(error),
            })
    if before["stdlib37"]["file_count"] != 37:
        context["failures"].append({
            "where": "stdlib37",
            "error": f"stdlib file count is {before['stdlib37']['file_count']}, not 37",
        })
    positive_results = {}
    reject_results = {}
    observed_results = {}
    for opt in ("0", "2"):
        for case in positives:
            name = case["name"]
            build_dir = out / "builds" / f"{name}_O{opt}"
            result = {"case": name, "opt": opt, "status": "failed"}
            try:
                command = execute_public_command(
                    context,
                    [context["cli"], "build", "--project",
                     case_manifest(repo_root, case), "--backend", "native",
                     "--opt", opt, "--out-dir", build_dir, "--format", "json"],
                    f"source-build-{name}-O{opt}", "source_build_positive",
                    args.build_timeout, trace_allowed=allowed)
                result["build"] = validate_positive_build(
                    context, command, name, opt, build_dir)
                program = execute_public_command(
                    context, [build_dir / "program"],
                    f"program-{name}-O{opt}", "program_positive",
                    args.run_timeout)
                stdout, stderr = check_program_expectation(
                    program, case, f"positive {name} O{opt}")
                result.update({"status": "passed", "program": program["index"],
                               "program_exit": program["exit"],
                               "stdout_sha256": sha256_bytes(program["_stdout"]),
                               "stderr_sha256": sha256_bytes(program["_stderr"]),
                               "stdout": stdout, "stderr": stderr})
            except PublicFailure as error:
                result["error"] = str(error)
                context["failures"].append({"where": f"positive {name} O{opt}",
                                            "error": str(error)})
            positive_results[(name, opt)] = result
            write_public_json(out / "cases/positive" /
                              f"{safe_label(name)}_O{opt}.json", result)
    for case in rejects:
        name = case["name"]
        build_dir = out / "builds" / "reject" / name
        result = {"case": name, "opt": "0", "status": "failed"}
        try:
            command = execute_public_command(
                context,
                [context["cli"], "build", "--project",
                 case_manifest(repo_root, case, True), "--backend", "native",
                 "--opt", "0", "--out-dir", build_dir, "--format", "json"],
                f"reject-build-{name}", "source_build_reject",
                args.build_timeout, trace_allowed=allowed)
            result.update(validate_reject_build(context, command, case, build_dir))
        except PublicFailure as error:
            result["error"] = str(error)
            context["failures"].append({"where": f"reject {name}",
                                        "error": str(error)})
        reject_results[name] = result
        write_public_json(out / "cases/reject" / f"{safe_label(name)}.json", result)
    observer_ready = context["observe"] != "off"
    observer_object = out / "observer/async_observer.o"
    observer_object.parent.mkdir(parents=True, exist_ok=True)
    if observer_ready:
        try:
            compile_command = execute_public_command(
                context,
                [context["clang"], "-std=c++20", "-O2", "-ffreestanding",
                 "-fno-builtin", "-fno-exceptions", "-fno-rtti",
                 "-fno-stack-protector", "-c", context["observer"],
                 "-o", observer_object],
                "observer-compile", "observer_compile", args.build_timeout)
            validate_successful_process(compile_command, "observer compile")
            if compile_command["exit"] != 0:
                raise PublicFailure(
                    f"observer compile failed: exit {compile_command['exit']}")
            context["artifacts"][str(observer_object)] = sha256_file(observer_object)
        except PublicFailure as error:
            observer_ready = False
            context["failures"].append({"where": "observer compile",
                                        "error": str(error)})
    if observer_ready:
        for opt in ("0", "2"):
            for case in observed_cases:
                name = case["name"]
                result = {"case": name, "opt": opt, "status": "failed"}
                try:
                    build_dir = out / "builds" / f"{name}_O{opt}"
                    object_path = resolve_public_object(build_dir, name, opt)
                    observer_program = (out / "observer" /
                                        f"{safe_label(name)}_O{opt}" / "program")
                    observer_program.parent.mkdir(parents=True, exist_ok=True)
                    link_command = [
                        context["clang"],
                        "--target=x86_64-unknown-linux-gnu",
                        "-nostdlib", "-static",
                        f"--ld-path={context['ld']}",
                        object_path, observer_object, context["runtime"],
                    ]
                    link_command.extend(f"-Wl,--wrap={symbol}" for symbol in WRAPS)
                    link_command.extend(["-o", observer_program])
                    link = execute_public_command(
                        context, link_command,
                        f"observer-link-{name}-O{opt}", "observer_link",
                        args.link_timeout)
                    validate_successful_process(
                        link, f"observer link {name} O{opt}")
                    if link["exit"] != 0:
                        raise PublicFailure(
                            f"observer link failed: exit {link['exit']}")
                    context["artifacts"][str(observer_program)] = sha256_file(
                        observer_program)
                    run = execute_public_command(
                        context, [observer_program],
                        f"observer-run-{name}-O{opt}", "observer_execute",
                        args.run_timeout)
                    validate_successful_process(run, f"observer {name} O{opt}")
                    stdout = run["_stdout"].decode("utf-8", "replace")
                    stderr = run["_stderr"].decode("utf-8", "replace")
                    counts = observed_counts(
                        stderr, run["exit"], stdout, case,
                        f"observer {name} O{opt}")
                    result.update({
                        "status": "passed",
                        "link": link["index"],
                        "observe": run["index"],
                        "program_object": str(object_path),
                        "program_object_sha256": sha256_file(object_path),
                        "observer_object_sha256": sha256_file(observer_object),
                        "runtime_sha256": sha256_file(context["runtime"]),
                        "counts": counts,
                        "stdout": stdout,
                        "stdout_sha256": sha256_bytes(run["_stdout"]),
                        "stderr_sha256": sha256_bytes(run["_stderr"]),
                    })
                except PublicFailure as error:
                    result["error"] = str(error)
                    context["failures"].append(
                        {"where": f"observed {name} O{opt}", "error": str(error)})
                observed_results[(name, opt)] = result
                write_public_json(out / "cases/observed" /
                                  f"{safe_label(name)}_O{opt}.json", result)
    artifact_final = {}
    for path, initial in context["artifacts"].items():
        file_path = Path(path)
        final = sha256_file(file_path) if file_path.is_file() else None
        artifact_final[path] = {"initial_sha256": initial,
                                "final_sha256": final,
                                "unchanged": final == initial}
        if final != initial:
            context["failures"].append(
                {"where": "artifact", "error": f"artifact changed: {path}"})
    after = snapshot_inputs(context, cases)
    write_public_json(out / "inputs.after.json", after)
    changed_inputs = [label for label in set(before) | set(after)
                      if before.get(label) != after.get(label)]
    if changed_inputs:
        context["failures"].append(
            {"where": "inputs", "error": f"changed inputs: {sorted(changed_inputs)}"})
    kinds = {}
    for command in context["commands"]:
        kinds[command["kind"]] = kinds.get(command["kind"], 0) + 1
    positive_passed = sum(1 for item in positive_results.values()
                          if item["status"] == "passed")
    reject_passed = sum(1 for item in reject_results.values()
                        if item["status"] == "passed")
    observed_passed = sum(1 for item in observed_results.values()
                          if item["status"] == "passed")
    checks_passed = positive_passed + reject_passed + observed_passed
    watched = context["observe"] != "off"
    expected_commands = 44 + 38 + (1 + 24 + 24 if watched else 0)
    expected_checks = 44 + (24 if watched else 0)
    timed_out = sum(1 for command in context["commands"] if command["timed_out"])
    cleanup_incomplete = sum(
        1 for command in context["commands"] if not command["cleanup_complete"])
    abandoned = sum(
        1 for command in context["commands"] if command["abandoned_pipes"])
    status = public_summary_status(
        context["failures"], checks_passed, expected_checks, watched,
        timed_out, cleanup_incomplete, abandoned)
    summary = {
        "schema": "SH12s-async-public-object-summary/1",
        "operation_id": "SH12s-async-source-all25-public-objects-20261114",
        "role": args.role,
        "status": status,
        "started_at": started_at,
        "finished_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "cli": {"path": str(context["cli"]),
                "sha256": sha256_file(context["cli"])},
        "runtime": {"path": str(context["runtime"]),
                    "sha256": sha256_file(context["runtime"])},
        "observer": {"path": str(context["observer"]),
                     "sha256": sha256_file(context["observer"]),
                     "object": str(observer_object),
                     "object_sha256": (sha256_file(observer_object)
                                       if observer_object.is_file() else None)},
        "tools": {name: {"path": str(path), "sha256": sha256_file(path)}
                  for name, path in context["tools"].items()},
        "trace_allow": {name: {"path": str(path), "sha256": sha256_file(path)}
                        for name, path in context["trace_allow"].items()},
        "wraps": {"count": len(WRAPS), "symbols": list(WRAPS),
                  "sha256": sha256_bytes(json.dumps(
                      WRAPS, separators=(",", ":")).encode("utf-8"))},
        "expected": {
            "build_public_total": 44,
            "build_positive": 38,
            "build_reject": 6,
            "program_positive": 38,
            "observer_compile": 1 if watched else 0,
            "observer_link": 24 if watched else 0,
            "observer_execute": 24 if watched else 0,
            "top_level_commands": expected_commands,
            "checks": expected_checks,
        },
        "actual": {"commands": dict(sorted(kinds.items())),
                   "top_level_commands": len(context["commands"]),
                   "checks_passed": checks_passed,
                   "positive_passed": positive_passed,
                   "reject_passed": reject_passed,
                   "observed_passed": observed_passed},
        "counts": {
            "build_positive": kinds.get("source_build_positive", 0),
            "build_reject": kinds.get("source_build_reject", 0),
            "build_public_total": (kinds.get("source_build_positive", 0) +
                                    kinds.get("source_build_reject", 0)),
            "program_positive": kinds.get("program_positive", 0),
            "observer_compile": kinds.get("observer_compile", 0),
            "observer_link": kinds.get("observer_link", 0),
            "observer_execute": kinds.get("observer_execute", 0),
        },
        "failures": context["failures"],
        "artifacts": artifact_final,
        "inputs": {
            "before": "inputs.before.json",
            "after": "inputs.after.json",
            "unchanged": not changed_inputs,
            "changed": sorted(changed_inputs),
            "binding_files": before["bindings"],
            "stdlib37": {k: before["stdlib37"][k]
                          for k in ("path", "file_count", "map_sha256",
                                    "expected_file_count")},
            "source116_root": before["source116_root"],
            "source116_candidate": before.get("source116_candidate"),
        },
        "cleanup": {
            "commands": len(context["commands"]),
            "timed_out": timed_out,
            "cleanup_incomplete": cleanup_incomplete,
            "abandoned_pipes": abandoned,
            "known_owned_children_outstanding": (
                0 if cleanup_incomplete == 0 and abandoned == 0 else "unknown"),
            "external_processes": "not_verified",
            "note": ("_run_process_impl reaps each top-level command; "
                     "unknown external processes are not claimed as zero"),
        },
        "observation_mode": context["observe"],
    }
    summary_path = out / "summary.json"
    write_public_json(summary_path, summary)
    summary_sha = sha256_file(summary_path)
    if args.proposal_out is not None:
        write_public_proposal(args, repo_root, out, {
            "positive": positives,
            "reject": rejects,
            "observed": observed_cases,
            "positive_results": positive_results,
            "reject_results": reject_results,
            "observed_results": observed_results,
        }, {**summary, "summary_sha256": summary_sha})
    print(json.dumps({
        "status": summary["status"],
        "role": summary["role"],
        "checks_passed": checks_passed,
        "checks_expected": expected_checks,
        "commands": len(context["commands"]),
        "failures": len(context["failures"]),
        "out": str(out),
    }, ensure_ascii=False))
    return 0 if summary["status"] == "passed" else 1


def run_self_test(args):
    tests = []

    def record(name, function, should_fail):
        try:
            function()
        except PublicFailure as error:
            tests.append({"name": name, "expected": "fail",
                          "observed": "fail", "detail": str(error)})
            return should_fail
        tests.append({"name": name, "expected": "fail" if should_fail else "pass",
                      "observed": "pass"})
        return not should_fail

    def valid_process():
        return {"exit": 0, "stdout": b"", "stderr": b"",
                "timed_out": False, "cleanup_complete": True,
                "abandoned_pipes": False}

    def process_ok():
        validate_process_record(valid_process(), "control")

    def process_bad(key, value):
        def check():
            record = valid_process()
            record[key] = value
            validate_process_record(record, "control")
        return check

    def timeout_bad(value):
        return lambda: validate_timeout(value, "control")

    def opt_ok():
        validate_opt(0)
        validate_opt(2)

    def program_bad(**changes):
        def check():
            case = {"exit": 0, "stdout": "", "stderr": ""}
            record_value = valid_process()
            record_value.update(changes)
            check_program_expectation(record_value, case, "control")
        return check

    def successful_process_bad(key, value):
        def check():
            record_value = valid_process()
            record_value[key] = value
            validate_successful_process(record_value, "control")
        return check

    def summary_ok():
        if public_summary_status([], 68, 68, True, 0, 0, 0) != "passed":
            raise PublicFailure("clean summary did not pass")

    def summary_bad(**changes):
        def check():
            values = {"failures": [], "checks_passed": 68,
                      "expected_checks": 68, "watched": True,
                      "timed_out": 0, "cleanup_incomplete": 0,
                      "abandoned": 0}
            values.update(changes)
            status = public_summary_status(**values)
            if status != "failed":
                raise AssertionError(f"summary accepted {changes}: {status}")
            raise PublicFailure(f"summary rejected {changes}: {status}")
        return check

    proposal_results = {
        "positive_results": {
            ("ok", "0"): {"status": "passed"},
            ("ok", "2"): {"status": "passed"},
            ("bare", "0"): {"status": "passed"},
            ("bare", "2"): {"status": "passed"},
            ("io", "0"): {"status": "failed"},
            ("io", "2"): {"status": "failed"},
        },
        "reject_results": {
            "bad": {"status": "passed"},
            "gone": {"status": "failed"},
        },
        "observed_results": {
            ("ok", "0"): {"status": "passed"},
            ("ok", "2"): {"status": "passed"},
        },
    }
    proposal_observed = {"ok", "bare"}

    def proposal_ok():
        for name, manifest in (
                ("ok", "tests/async/ok/lang.toml"),
                ("bad", "tests/async/reject/bad/lang.toml")):
            if proposal_case_adaptation(
                    name, manifest, proposal_results,
                    proposal_observed) != "ready":
                raise PublicFailure(f"complete case {name} was not ready")

    def proposal_bad(name, manifest, observed=()):
        def check():
            state = proposal_case_adaptation(
                name, manifest, proposal_results, observed)
            if state != "pending":
                raise AssertionError(f"incomplete case {name}: {state}")
            raise PublicFailure(f"incomplete case {name}: {state}")
        return check

    good_counts = ("COUNTS future_new=1 future_free=1 frame_new=1 frame_free=1 "
                   "task_retain=0 task_release=1 bad_frame_free=0 vec_new=0 "
                   "vec_free=0 vec_retain=0 vec_release=0\n")
    count_case = {
        "name": "control", "exit": 0, "stdout": "",
        "counts": {"future_new": 1, "future_free": 1, "frame_new": 1,
                   "frame_free": 1, "task_retain": 0, "task_release": 1,
                   "bad_frame_free": 0, "vec_new": 0, "vec_free": 0,
                   "vec_retain": 0, "vec_release": 0},
    }

    def observed_ok():
        observed_counts(good_counts, 0, "", count_case, "control")

    def observed_bad(stderr, status=0, stdout=""):
        return lambda: observed_counts(stderr, status, stdout, count_case, "control")

    with tempfile.TemporaryDirectory(prefix="async-public-control-") as temp:
        base = Path(temp)
        object_dir = base / "control_O0"
        object_dir.mkdir()
        (object_dir / "program.o").write_bytes(b"object")
        (object_dir / "program").write_bytes(b"program")

        def positive_build_ok():
            command = {
                "index": 0, "exit": 0, "timed_out": False,
                "cleanup_complete": True, "abandoned_pipes": False,
                "_stdout": json.dumps({
                    "status": "ok", "diagnostics": [],
                }).encode(), "_stderr": b"", "trace": {"status": "ok"},
            }
            validate_positive_build(
                {"artifacts": {}}, command, "control", "0", object_dir)

        def positive_build_bad(key, value):
            def check():
                command = {
                    "index": 0, "exit": 0, "timed_out": False,
                    "cleanup_complete": True, "abandoned_pipes": False,
                    "_stdout": json.dumps({
                        "status": "ok", "diagnostics": [],
                    }).encode(), "_stderr": b"", "trace": {"status": "ok"},
                }
                command[key] = value
                validate_positive_build(
                    {"artifacts": {}}, command, "control", "0", object_dir)
            return check

        def object_ok():
            resolve_public_object(object_dir, "control", "0")

        def object_bad(where, name, opt):
            return lambda: resolve_public_object(where, name, opt)

        missing_observer = base / "missing_observer.cpp"

        def observer_ok():
            if require_observer_for_public("off", missing_observer) != "off":
                raise PublicFailure("off was not preserved")

        reject_build = base / "reject-build"
        reject_build.mkdir()
        reject_command = {
            "index": 0, "exit": 1, "timed_out": False,
            "cleanup_complete": True, "abandoned_pipes": False,
            "_stdout": json.dumps({
                "status": "error",
                "diagnostics": [{"code": "E_CONTROL"}],
            }).encode(), "_stderr": b"", "trace": {"status": "ok"},
        }

        def reject_ok():
            validate_reject_build({}, reject_command,
                                  {"name": "control", "code": "E_CONTROL"},
                                  reject_build)

        def reject_bad_exit():
            command = dict(reject_command)
            command["exit"] = 0
            validate_reject_build({}, command,
                                  {"name": "control", "code": "E_CONTROL"},
                                  reject_build)

        def reject_bad_publication():
            published = base / "reject-published"
            published.mkdir()
            (published / "program").write_bytes(b"no")
            validate_reject_build({}, reject_command,
                                  {"name": "control", "code": "E_CONTROL"},
                                  published)

        def reject_bad_flag(key, value):
            def check():
                command = dict(reject_command)
                command[key] = value
                validate_reject_build(
                    {}, command, {"name": "control", "code": "E_CONTROL"},
                    reject_build)
            return check

        def observer_step_bad(where, key, value):
            def check():
                command = {
                    "index": 0, "exit": 0, "timed_out": False,
                    "cleanup_complete": True, "abandoned_pipes": False,
                }
                command[key] = value
                validate_successful_process(command, where)
            return check

        def program_exit1_ok():
            case = {"exit": 1, "stdout": "", "stderr": ""}
            record_value = valid_process()
            record_value["exit"] = 1
            check_program_expectation(record_value, case, "control")

        checks = [
            ("source_new_matching", lambda: validate_source_identity(
                {"file_count": 116, "map_sha256": "new-source"},
                {"file_count": 116, "map_sha256": "new-source"}), False),
            ("source_mismatch", lambda: validate_source_identity(
                {"file_count": 116, "map_sha256": "new-source"},
                {"file_count": 116, "map_sha256": "old-source"}), True),
            ("source_candidate_missing", lambda: validate_source_identity(
                {"file_count": 116, "map_sha256": "new-source"}, None), True),
            ("source_candidate_empty", lambda: validate_source_identity(
                {"file_count": 116, "map_sha256": "new-source"},
                {"file_count": 0, "map_sha256": None}), True),
            ("source_root_empty", lambda: validate_source_identity(
                {"file_count": 0, "map_sha256": None},
                {"file_count": 116, "map_sha256": "new-source"}), True),
            ("process_record_valid", process_ok, False),
            ("process_exit_bool", process_bad("exit", True), True),
            ("process_exit_string", process_bad("exit", "0"), True),
            ("process_timed_out_int", process_bad("timed_out", 1), True),
            ("process_cleanup_string", process_bad("cleanup_complete", "yes"), True),
            ("process_abandoned_int", process_bad("abandoned_pipes", 0), True),
            ("timeout_zero", timeout_bad(0), True),
            ("timeout_negative", timeout_bad(-1), True),
            ("timeout_inf", timeout_bad(float("inf")), True),
            ("timeout_nan", timeout_bad(float("nan")), True),
            ("timeout_bool", timeout_bad(True), True),
            ("timeout_string", timeout_bad("15"), True),
            ("opt_valid", opt_ok, False),
            ("opt_bad_1", lambda: validate_opt(1), True),
            ("opt_bad_true", lambda: validate_opt(True), True),
            ("program_exit_valid", program_bad(), False),
            ("program_exit1_valid", program_exit1_ok, False),
            ("program_nonzero_exit", program_bad(exit=1), True),
            ("program_wrong_stdout", program_bad(stdout=b"wrong"), True),
            ("program_cleanup_false",
             program_bad(cleanup_complete=False), True),
            ("program_abandoned_true", program_bad(abandoned_pipes=True), True),
            ("successful_process_cleanup_false",
             successful_process_bad("cleanup_complete", False), True),
            ("successful_process_abandoned_true",
             successful_process_bad("abandoned_pipes", True), True),
            ("positive_build_valid", positive_build_ok, False),
            ("positive_build_cleanup_false",
             positive_build_bad("cleanup_complete", False), True),
            ("positive_build_abandoned_true",
             positive_build_bad("abandoned_pipes", True), True),
            ("observed_valid", observed_ok, False),
            ("observed_missing_counts_line", observed_bad(""), True),
            ("observed_missing_field", observed_bad(
                good_counts.replace("task_release=1 ", "")), True),
            ("observed_wrong_exact", observed_bad(
                good_counts.replace("future_new=1", "future_new=2")), True),
            ("observed_unbalanced", observed_bad(
                good_counts.replace("future_free=1", "future_free=0")), True),
            ("observed_nonzero_exit", observed_bad(good_counts, status=1), True),
            ("object_valid", object_ok, False),
            ("object_wrong_opt", object_bad(object_dir, "control", "2"), True),
            ("object_wrong_case", object_bad(object_dir, "other", "0"), True),
            ("object_missing", object_bad(base / "control_O2", "control", "2"), True),
            ("observer_auto_missing",
             lambda: require_observer_for_public("auto", missing_observer), True),
            ("observer_on_missing",
             lambda: require_observer_for_public("on", missing_observer), True),
            ("observer_off_preserved", observer_ok, False),
            ("reject_valid", reject_ok, False),
            ("reject_zero_exit", reject_bad_exit, True),
            ("reject_published_program", reject_bad_publication, True),
            ("reject_cleanup_false",
             reject_bad_flag("cleanup_complete", False), True),
            ("reject_abandoned_true",
             reject_bad_flag("abandoned_pipes", True), True),
            ("observer_compile_cleanup_false",
             observer_step_bad("observer compile", "cleanup_complete", False), True),
            ("observer_link_abandoned_true",
             observer_step_bad("observer link", "abandoned_pipes", True), True),
            ("observer_execute_cleanup_false",
             observer_step_bad("observer execute", "cleanup_complete", False), True),
            ("summary_valid", summary_ok, False),
            ("summary_cleanup_incomplete",
             summary_bad(cleanup_incomplete=1), True),
            ("summary_abandoned", summary_bad(abandoned=1), True),
            ("summary_timed_out", summary_bad(timed_out=1), True),
            ("proposal_complete_ready", proposal_ok, False),
            ("proposal_failed_ordinary_pending",
             proposal_bad("io", "tests/async/io/lang.toml"), True),
            ("proposal_missing_observed_pending",
             proposal_bad("bare", "tests/async/bare/lang.toml",
                          proposal_observed), True),
            ("proposal_failed_reject_pending",
             proposal_bad("gone", "tests/async/reject/gone/lang.toml"), True),
        ]
        passed = 0
        for name, function, should_fail in checks:
            if record(name, function, should_fail):
                passed += 1
    result = {
        "schema": "SH12s-async-public-controls/1",
        "passed": passed,
        "total": len(tests),
        "status": "passed" if passed == len(tests) else "failed",
        "validators": (
            "validate_timeout, validate_opt, validate_process_record, "
            "validate_source_identity, "
            "validate_successful_process, check_program_expectation, "
            "observed_counts, public_summary_status, "
            "proposal_case_adaptation, resolve_public_object, "
            "require_observer_for_public, validate_positive_build, "
            "validate_reject_build: the same functions used by public mode"),
        "tests": tests,
    }
    if args.self_test_out is not None:
        write_public_json(args.self_test_out, result)
    print(json.dumps({"status": result["status"], "passed": passed,
                      "total": len(tests)}, ensure_ascii=False))
    return 0 if result["status"] == "passed" else 1


def main():
    repo_root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--cli",
        type=Path,
        default=repo_root / "build" / "woozy-stdlib-native" / "woozyc",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=None,
        help="diagnostics/output root; defaults to a fresh independent temp dir",
    )
    parser.add_argument(
        "--root",
        type=Path,
        default=repo_root,
        help="repository root containing tests/async",
    )
    parser.add_argument("--helper", type=Path, default=None)
    parser.add_argument("--runtime", type=Path, default=None)
    parser.add_argument("--clang", type=Path, default=None)
    parser.add_argument("--ld", type=Path, default=None)
    parser.add_argument("--observer", type=Path, default=None)
    parser.add_argument(
        "--observe",
        choices=("auto", "on", "off"),
        default="auto",
        help="run allocation-observer cases when prerequisites are available",
    )
    parser.add_argument("--build-timeout", type=float, default=180)
    parser.add_argument("--link-timeout", type=float, default=120)
    parser.add_argument("--run-timeout", type=float, default=15)
    parser.add_argument(
        "--public-object",
        action="store_true",
        help="reuse each role's public program.o for the original 12 observers",
    )
    parser.add_argument("--role", choices=("candidate", "reference"), default=None)
    parser.add_argument("--strace", type=Path, default=None)
    parser.add_argument(
        "--trace-allow",
        type=Path,
        action="append",
        default=[],
        help="reference-only explicit matching lang_native_helper path",
    )
    parser.add_argument("--toolchain", type=Path, default=None)
    parser.add_argument("--stdlib-root", type=Path, default=None)
    parser.add_argument("--libxml2-lib", type=Path, default=None)
    parser.add_argument("--python", type=Path, default=None)
    parser.add_argument("--candidate-source", type=Path, default=None)
    parser.add_argument("--identity", type=Path, action="append", default=[])
    parser.add_argument("--proposal-out", type=Path, default=None)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--self-test-out", type=Path, default=None)
    args = parser.parse_args()

    if args.self_test:
        return run_self_test(args)
    if args.public_object:
        if args.out is None:
            args.out = Path(tempfile.mkdtemp(prefix="woozy-async-public-"))
        try:
            return run_public_mode(args, args.root)
        except PublicFailure as error:
            print(f"FAILED: public-object mode: {error}")
            return 1

    if args.out is None:
        args.out = Path(tempfile.mkdtemp(prefix="woozy-async-cases-"))
    args.out.mkdir(parents=True, exist_ok=True)

    env = os.environ.copy()
    print(f"diagnostics root: {args.out}")

    cases_file = args.root / "tests" / "async" / "cases.json"
    with open(cases_file) as stream:
        catalog = json.load(stream)

    passed = 0
    passed += build_regular_cases(
        args.cli,
        args.root,
        args.out,
        env,
        catalog,
        args.build_timeout,
        args.run_timeout,
    )
    passed += build_reject_cases(
        args.cli, args.root, args.out, env, catalog, args.build_timeout
    )
    observed_passed, observed_ran = build_observed_cases(
        args,
        args.cli,
        args.root,
        args.out,
        env,
        catalog,
        args.build_timeout,
        args.link_timeout,
        args.run_timeout,
    )
    passed += observed_passed

    expected = (
        len(catalog.get("positive", [])) * 2
        + len(catalog.get("reject", []))
        + (len(catalog.get("observed", [])) * 2 if observed_ran else 0)
    )
    print(f"\nALL {passed}/{expected} ASYNC CASES PASSED!")
    return 0


if __name__ == "__main__":
    sys.exit(main())
