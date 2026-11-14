#!/usr/bin/env python3
"""Exercise process/signal/shared-memory Source APIs in standalone ELF files.

The original 15-command route is unchanged: O0/O2 build, ELF inspection,
normal run, invalid UTF-8 environment, invalid UTF-8 argv, misuse build/run,
then one Python rejection build.  The legacy CMake invocation (only
``--cli/--root/--out``) keeps the same commands, assertions and 60 second
budget.  A second legacy invocation with the same argv uses a private
``rerun-N`` child directory, so old raw records and programs are not
overwritten; the top-level ``process-source-results.json`` stays at ``out``.
``--record`` and the candidate/reference roles keep the strict empty-output
refusal.

Optional ``--role/--trace/--record`` add per-command raw records and an
exec-only trace.  They are evidence options, not a second test framework: all
processes still go through ``selfhost/corpus.py::_run_process_impl`` and the
same finite cleanup path.  The before/after input snapshot includes the actual
``selfhost/corpus.py`` loaded by ``load_corpus(root)`` and the actual GNU
readelf resolved from ``PATH``/``/usr/bin/readelf``; any LLVM
``llvm-readelf`` is recorded separately and is not used as GNU evidence.
"""
import argparse
import ast
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import sys
import time

HERE = Path(__file__).resolve().parent
OPERATION_ID = "SH15-process-public-records-20261114"
COMMAND_SCHEMA = "woozy/process-public-command/1"
TIMEOUT = 60
MISSING = "/definitely/missing/woozy"
PANIC = "LANG_NATIVE_PANIC:v1:runtime-handle-misuse:"
INVALID_UTF8 = "\udcff"


class EvidenceError(RuntimeError):
    """A command or its record did not satisfy the original route."""


def _reject_json_constant(value):
    raise ValueError(f"non-standard JSON constant {value!r}")


def load_corpus(root):
    path = Path(root) / "selfhost/corpus.py"
    spec = importlib.util.spec_from_file_location("sh15_process_corpus", path)
    if spec is None or spec.loader is None:
        raise EvidenceError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def digest(path):
    import hashlib
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def digest_bytes(data):
    import hashlib
    return hashlib.sha256(data).hexdigest()


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, ensure_ascii=True, indent=2, allow_nan=False) + "\n",
        encoding="utf-8")


def append_jsonl(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as stream:
        stream.write(json.dumps(value, ensure_ascii=True,
                                allow_nan=False, separators=(",", ":")) + "\n")


def file_record(path):
    path = Path(path)
    if not path.is_file():
        return None
    return {"path": str(path), "size": path.stat().st_size,
            "sha256": digest(path)}


def tree_map(root):
    root = Path(root)
    result = {}
    if not root.is_dir():
        return result
    for path in sorted(root.rglob("*")):
        if path.is_file():
            result[str(path.relative_to(root))] = digest(path)
    return result


def map_sha(value):
    return digest_bytes(json.dumps(
        value, sort_keys=True, separators=(",", ":")).encode("utf-8"))


def tree_record(root):
    root = Path(root)
    if not root.is_dir():
        return None
    files = tree_map(root)
    return {"path": str(root), "file_count": len(files),
            "map_sha256": map_sha(files), "files": files}


def argv_display(argv):
    return [os.fsencode(str(part)).decode("utf-8", "backslashreplace")
            for part in argv]


def argv_hex(argv):
    return [os.fsencode(str(part)).hex() for part in argv]


def _quoted_tokens(text):
    return [ast.literal_eval("b" + token) for token in
            re.findall(r'"(?:[^"\\]|\\.)*"', text)]


def _matching_bracket(text, start):
    depth = 0
    quote = False
    escape = False
    for index in range(start, len(text)):
        char = text[index]
        if quote:
            if escape:
                escape = False
            elif char == "\\":
                escape = True
            elif char == '"':
                quote = False
            continue
        if char == '"':
            quote = True
        elif char == "[":
            depth += 1
        elif char == "]":
            depth -= 1
            if depth == 0:
                return index
    return -1


def parse_exec_trace(path):
    """Return execve/execveat records, including failed attempts.

    ``corpus.parse_trace`` only needs successful compiler subprocesses.  The
    process program deliberately performs three failing execs, so this parser
    keeps the result and errno as well as the path and argv bytes.
    """
    records = []
    text = Path(path).read_text(encoding="utf-8", errors="surrogateescape")
    for line in text.splitlines():
        match = re.search(r"\b(execve|execveat)\(", line)
        if not match:
            continue
        call = match.group(1)
        pid_match = re.match(r"\s*(\d+)\s+", line)
        tokens = _quoted_tokens(line)
        if not tokens:
            records.append({"call": call, "pid": int(pid_match.group(1))
                            if pid_match else None, "error": "no pathname",
                            "raw": line})
            continue
        path_bytes = tokens[0]
        pathname = path_bytes.decode(
            sys.getfilesystemencoding(), "surrogateescape")
        bracket = line.find("[", line.find('"') + 1)
        close = _matching_bracket(line, bracket) if bracket >= 0 else -1
        if bracket < 0 or close < 0:
            records.append({"call": call, "pid": int(pid_match.group(1))
                            if pid_match else None, "error": "no argv",
                            "path": pathname, "path_hex": path_bytes.hex(),
                            "raw": line})
            continue
        argv_bytes = _quoted_tokens(line[bracket:close + 1])
        result_match = re.search(
            r"\)\s*=\s*(-?\d+)(?:\s+([A-Z0-9_]+))?", line)
        if result_match is None:
            records.append({"call": call, "pid": int(pid_match.group(1))
                            if pid_match else None, "error": "no result",
                            "path": pathname, "path_hex": path_bytes.hex(),
                            "raw": line})
            continue
        result = int(result_match.group(1))
        errno = result_match.group(2)
        try:
            absolute = str(Path(pathname).resolve())
        except OSError:
            absolute = pathname
        records.append({
            "call": call,
            "pid": int(pid_match.group(1)) if pid_match else None,
            "path": pathname,
            "path_abs": absolute,
            "path_hex": path_bytes.hex(),
            "path_sha256": digest(absolute) if Path(absolute).is_file() else None,
            "argv": [part.decode(sys.getfilesystemencoding(), "surrogateescape")
                     for part in argv_bytes],
            "argv_hex": [part.hex() for part in argv_bytes],
            "result": result,
            "success": result == 0,
            "errno": errno,
            "raw": line,
        })
    return records


def analyze_program(records, program, requested_argv, require_spawn):
    program_abs = str(Path(program).resolve())
    shell = Path("/bin/sh").resolve()
    initial = records[0] if records else None
    success = [row for row in records if row.get("success")]
    failed = [row for row in records if not row.get("success")]
    shells = [row for row in success if row.get("path_abs") == str(shell)]
    missing = [row for row in failed if row.get("path_abs") == MISSING]
    observation = {
        "initial_exec": initial,
        "requested_argv_hex": argv_hex(requested_argv),
        "successful_execs": success,
        "failed_execs": failed,
        "shell": {
            "requested": "/bin/sh",
            "resolved": str(shell),
            "sha256": digest(shell) if shell.is_file() else None,
            "count": len(shells),
            "execs": shells,
        },
        "missing_enoent": {
            "requested": MISSING,
            "count": len(missing),
            "execs": missing,
        },
        "observed_pids": sorted({row["pid"] for row in records
                                 if row.get("pid") is not None}),
    }
    problems = []
    if initial is None:
        problems.append("program trace has no initial exec")
    else:
        if initial.get("path_abs") != program_abs:
            problems.append("initial exec is not the program")
        if initial.get("argv_hex") != argv_hex(requested_argv):
            problems.append("initial exec argv bytes differ")
    if require_spawn:
        if len(shells) != 2:
            problems.append(f"expected 2 /bin/sh execs, saw {len(shells)}")
        if len(missing) != 3:
            problems.append(
                f"expected 3 missing exec attempts, saw {len(missing)}")
        if any(row.get("errno") != "ENOENT" for row in missing):
            problems.append("missing exec attempt did not fail with ENOENT")
    return observation, problems


def process_problems(exit_code, timed_out, cleanup_complete,
                     abandoned_pipes, spawn_error, expected):
    problems = []
    if spawn_error is not None:
        problems.append("spawn failed: " + spawn_error)
    if type(exit_code) is not int:
        problems.append("exit is not an integer")
    elif exit_code < 0:
        problems.append(f"terminated by signal {-exit_code}")
    if timed_out is not False:
        problems.append("timed out or timeout flag is not false")
    if cleanup_complete is not True:
        problems.append("process cleanup was incomplete")
    if abandoned_pipes is not False:
        problems.append("pipes were abandoned")
    if type(exit_code) is int and exit_code != expected:
        problems.append(f"expected exit {expected}, saw {exit_code}")
    return problems


def panic_problems(stderr_text):
    if PANIC not in stderr_text:
        return ["panic marker is missing"]
    return []


def python_rejection(document):
    if not isinstance(document, dict):
        return ["Python rejection is not a JSON object"]
    problems = []
    if document.get("status") != "error":
        problems.append("Python rejection status is not error")
    diagnostics = document.get("diagnostics")
    if not isinstance(diagnostics, list):
        problems.append("Python rejection has no diagnostics list")
        diagnostics = []
    codes = [item.get("code") for item in diagnostics
             if isinstance(item, dict)]
    if "E_CAPABILITY_UNAVAILABLE" not in codes:
        problems.append("Python rejection lacks E_CAPABILITY_UNAVAILABLE")
    return problems


def scan_artifacts(directory):
    directory = Path(directory)
    result = {}
    if not directory.is_dir():
        return result
    for path in sorted(directory.rglob("*")):
        if path.is_file():
            result[str(path.relative_to(directory))] = {
                "size": path.stat().st_size, "sha256": digest(path)}
    return result


def detect_stdlib(cli):
    cli = Path(cli)
    sibling = cli.parent / "stdlib"
    if sibling.is_dir():
        return sibling.resolve()
    data = cli.read_bytes()
    for match in re.finditer(rb"(/home/[^\x00\s\"']+/stdlib)", data):
        path = Path(match.group(1).decode("utf-8", "surrogateescape"))
        if path.is_dir():
            return path.resolve()
    return None


def detect_helper(cli):
    cli = Path(cli)
    sibling = cli.parent / "native/lang_native_helper"
    if sibling.is_file():
        return sibling.resolve()
    return None


def runtime_roots(cli, helper, peer_cli, peer_helper):
    roots = {}
    for label, cli_path, helper_path in (
            ("role", cli, helper), ("peer", peer_cli, peer_helper)):
        sibling = Path(cli_path).parent / "runtime"
        if sibling.is_dir():
            roots[label] = sibling.resolve()
        elif helper_path is not None:
            roots[label] = Path(helper_path).parent.resolve()
    return roots


def snapshot_inputs(ctx):
    root = ctx["root"]
    family = root / "tests/native/process"
    files = {
        "family-manifest": family / "lang.toml",
        "family-source": family / "src/main.wz",
        "misuse-manifest": family / "misuse/lang.toml",
        "misuse-source": family / "misuse/src/main.wz",
        "shared-runner": Path(__file__).resolve(),
        "corpus": ctx["corpus_path"],
        "family-static-runtime": family / "static_runtime.cpp",
        "role-cli": ctx["cli"],
        "peer-cli": ctx["peer_cli"],
        "python": ctx["python"],
    }
    if ctx.get("readelf") is not None:
        files["gnu-readelf"] = ctx["readelf"]
    if ctx["helper"] is not None:
        files["reference-helper"] = ctx["helper"]
    if ctx["peer_helper"] is not None:
        files["peer-helper"] = ctx["peer_helper"]
    for label, path in ctx["llvm_tools"].items():
        files["llvm-" + label] = path
    if ctx["libxml"] is not None:
        files["libxml"] = ctx["libxml"]
    if ctx["strace"] is not None:
        files["strace"] = ctx["strace"]
    for label, path in ctx["runtime_roots"].items():
        if path.is_dir():
            for child in sorted(path.iterdir()):
                if child.is_file():
                    files[f"{label}-runtime-{child.name}"] = child
        elif path.is_file():
            files[f"{label}-runtime"] = path
    trees = {
        "repository-source116": tree_record(root / "selfhost/src"),
        "candidate-source116": tree_record(ctx["candidate_source"]),
        "candidate-stdlib": tree_record(ctx["candidate_stdlib"]),
        "reference-stdlib": tree_record(ctx["reference_stdlib"]),
    }
    snapshot = {
        "files": {label: file_record(path) for label, path in files.items()},
        "trees": trees,
    }
    snapshot["snapshot_sha256"] = map_sha(snapshot)
    return snapshot


def public_row(record):
    return dict(argv=record["argv"], exit=record["exit"],
                stdout_hex=record["stdout_hex"],
                stderr=record["stderr_text"])


def command(argv, *, label, phase, ctx, expected=0, env=None,
            trace_success=None, trace_failed=(), require_failed=None,
            program=None, require_spawn=False, artifacts_dir=None,
            env_overrides=None):
    actual = [str(part) for part in argv]
    trace_success = ({str(Path(part).resolve()) for part in trace_success}
                     if trace_success is not None else None)
    trace_failed = {str(Path(part).resolve()) for part in trace_failed}
    directory = ctx["record"] / "commands" / f"{len(ctx['commands']):02d}-{label}"
    directory.mkdir(parents=True, exist_ok=False)
    trace_path = directory / "exec.log"
    run_argv = actual
    if ctx["strace"] is not None:
        run_argv = [str(ctx["strace"]), "--seccomp-bpf", "-f", "-qq",
                    "-s", "16384", "-e", "trace=execve,execveat",
                    "-o", str(trace_path), *actual]
    start = time.monotonic()
    spawn_error = None
    try:
        process = ctx["corpus"]._run_process_impl(
            run_argv, ctx["cwd"], env, TIMEOUT)
    except (OSError, ValueError) as error:
        process = None
        spawn_error = f"{type(error).__name__}: {error}"
    elapsed = round(time.monotonic() - start, 6)
    stdout = process["stdout"] if process else b""
    stderr = process["stderr"] if process else b""
    (directory / "stdout.bin").write_bytes(stdout)
    (directory / "stderr.bin").write_bytes(stderr)
    trace_error = None
    trace_records = []
    if ctx["strace"] is not None:
        try:
            trace_records = parse_exec_trace(trace_path)
            if not trace_records:
                trace_error = "trace has no execve/execveat record"
        except (OSError, ValueError) as error:
            trace_error = f"{type(error).__name__}: {error}"
    problems = []
    exit_code = process["exit"] if process else None
    timed_out = process["timed_out"] if process else None
    cleanup_complete = process["cleanup_complete"] if process else None
    abandoned_pipes = process["abandoned_pipes"] if process else None
    problems.extend(process_problems(
        exit_code, timed_out, cleanup_complete, abandoned_pipes,
        spawn_error, expected))
    if ctx["strace"] is not None:
        if trace_error is not None:
            problems.append(trace_error)
        else:
            success = [row for row in trace_records if row.get("success")]
            failed = [row for row in trace_records if not row.get("success")]
            if trace_success is not None:
                for row in success:
                    if row.get("path_abs") not in trace_success:
                        problems.append(
                            "unexpected successful exec: " + str(row.get("path")))
            for row in failed:
                if row.get("path_abs") not in trace_failed:
                    problems.append(
                        "unexpected failed exec: " + str(row.get("path")))
            if require_failed is not None and len(failed) != require_failed:
                problems.append(
                    f"expected {require_failed} failed execs, saw {len(failed)}")
    program_observation = None
    if program is not None and ctx["strace"] is not None:
        program_observation, program_problems = analyze_program(
            trace_records, program, actual, require_spawn)
        problems.extend(program_problems)
    record = {
        "schema": COMMAND_SCHEMA,
        "operation_id": OPERATION_ID,
        "index": len(ctx["commands"]),
        "label": label,
        "phase": phase,
        "role": ctx["role"],
        "argv": actual,
        "argv_display": argv_display(actual),
        "argv_hex": argv_hex(actual),
        "cwd": str(ctx["cwd"]),
        "timeout": TIMEOUT,
        "expected_exit": expected,
        "elapsed": elapsed,
        "exit": exit_code,
        "exit_is_int": type(exit_code) is int,
        "timed_out": timed_out,
        "cleanup_complete": cleanup_complete,
        "abandoned_pipes": abandoned_pipes,
        "spawn_error": spawn_error,
        "stdout_path": "stdout.bin",
        "stdout_size": len(stdout),
        "stdout_sha256": digest_bytes(stdout),
        "stdout_hex": stdout.hex(),
        "stderr_path": "stderr.bin",
        "stderr_size": len(stderr),
        "stderr_sha256": digest_bytes(stderr),
        "stderr_hex": stderr.hex(),
        "stderr_text": stderr.decode("utf-8", "backslashreplace"),
        "environment_overrides": env_overrides or {},
        "inherited_environment_recorded": False,
        "trace_path": "exec.log" if ctx["strace"] is not None else None,
        "trace_sha256": digest(trace_path)
        if ctx["strace"] is not None and trace_path.is_file() else None,
        "trace_error": trace_error,
        "trace_records": trace_records,
        "trace_success_paths": sorted({
            row["path_abs"] for row in trace_records if row.get("success")}),
        "trace_failed_paths": sorted({
            row["path_abs"] for row in trace_records if not row.get("success")}),
        "program_observation": program_observation,
        "artifacts": scan_artifacts(artifacts_dir)
        if artifacts_dir is not None else None,
        "problems": problems,
    }
    record["record_dir"] = str(directory)
    write_json(directory / "command.json", record)
    append_jsonl(ctx["record"] / "commands.jsonl", record)
    ctx["commands"].append(record)
    if problems:
        raise EvidenceError(f"{label}: " + "; ".join(problems))
    return record


def update_record(record, key, value):
    record[key] = value
    write_json(Path(record["record_dir"]) / "command.json", record)


def env_with_resources(base, ctx):
    env = dict(base)
    paths = []
    if ctx["llvm_bin"] is not None:
        paths.append(str((ctx["llvm_bin"].parent / "lib").resolve()))
    if ctx["libxml"] is not None:
        paths.append(str(ctx["libxml"].parent.resolve()))
    if paths:
        existing = env.get("LD_LIBRARY_PATH")
        if existing:
            paths.append(existing)
        env["LD_LIBRARY_PATH"] = ":".join(paths)
    return env


def legacy_run_root(out):
    """Keep the default CMake-style rerun without overwriting old raw data."""
    out = Path(out)
    if not out.is_dir() or not any(out.iterdir()):
        return out
    index = 1
    while True:
        candidate = out / f"rerun-{index}"
        if not candidate.exists() and not candidate.is_symlink():
            return candidate
        index += 1


def main():
    parser = argparse.ArgumentParser()
    for name in ("cli", "root", "out"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--role", choices=("legacy", "candidate", "reference"),
                        default="legacy")
    parser.add_argument("--trace", type=Path, default=None,
                        help="strace executable; enables per-command exec trace")
    parser.add_argument("--record", type=Path, default=None,
                        help="record directory (default: --out)")
    parser.add_argument("--peer-cli", type=Path, default=None)
    parser.add_argument("--candidate-source", type=Path, default=None)
    parser.add_argument("--llvm-bin", type=Path, default=None)
    parser.add_argument("--libxml", type=Path, default=None)
    parser.add_argument("--python", type=Path, default=Path(sys.executable))
    parser.add_argument("--expect-cli-sha256", default=None)
    parser.add_argument("--expect-candidate-source-sha256", default=None)
    args = parser.parse_args()

    out = args.out.resolve()
    record_argument = args.record.resolve() if args.record is not None else None
    strict_record = (record_argument is not None or
                     args.role in ("candidate", "reference"))
    if strict_record:
        record_root = record_argument or out
        run_root = out
        for path in {out, record_root}:
            if path.exists() and any(path.iterdir()):
                print(json.dumps({"status": "failed", "out": str(path),
                                  "error": "output directory is not empty"},
                                 ensure_ascii=True))
                return 2
        out.mkdir(parents=True, exist_ok=True)
        record_root.mkdir(parents=True, exist_ok=True)
    else:
        record_root = legacy_run_root(out)
        run_root = record_root
        out.mkdir(parents=True, exist_ok=True)
        record_root.mkdir(parents=True, exist_ok=True)

    strict = args.role in ("candidate", "reference")
    cli = args.cli.resolve()
    root = args.root.resolve()
    peer_cli = args.peer_cli.resolve() if args.peer_cli else cli
    helper = detect_helper(cli)
    peer_helper = detect_helper(peer_cli)
    candidate_cli = cli if args.role == "candidate" else peer_cli
    reference_cli = cli if args.role == "reference" else peer_cli
    candidate_stdlib = detect_stdlib(candidate_cli)
    reference_stdlib = detect_stdlib(reference_cli)
    candidate_source = (args.candidate_source.resolve()
                        if args.candidate_source else root / "selfhost/src")
    libxml = args.libxml.resolve() if args.libxml else None
    strace = args.trace.resolve() if args.trace else None
    python = args.python.resolve() if args.python else Path(sys.executable)
    llvm_tools = {}
    if args.llvm_bin is not None:
        llvm_bin = args.llvm_bin.resolve()
        for name in ("clang", "clang-22", "opt", "llc", "lld", "ld.lld",
                     "llvm-readelf"):
            path = llvm_bin / name
            if path.is_file():
                llvm_tools[name] = path.resolve()
    readelf = Path(shutil.which("readelf") or "/usr/bin/readelf").resolve()

    preflight = {
        "operation_id": OPERATION_ID,
        "role": args.role,
        "cli": file_record(cli),
        "peer_cli": file_record(peer_cli),
        "helper": file_record(helper) if helper else None,
        "peer_helper": file_record(peer_helper) if peer_helper else None,
        "candidate_source": tree_record(candidate_source),
        "candidate_stdlib": tree_record(candidate_stdlib)
        if candidate_stdlib else None,
        "reference_stdlib": tree_record(reference_stdlib)
        if reference_stdlib else None,
        "llvm_tools": {label: file_record(path)
                       for label, path in llvm_tools.items()},
        "libxml": file_record(libxml) if libxml else None,
        "strace": file_record(strace) if strace else None,
        "python": file_record(python),
    }
    write_json(record_root / "preflight.json", preflight)
    if args.expect_cli_sha256 and preflight["cli"]["sha256"] != args.expect_cli_sha256:
        print(json.dumps({"status": "failed", "error": "CLI SHA mismatch",
                          "expected": args.expect_cli_sha256,
                          "actual": preflight["cli"]["sha256"]}))
        return 2
    if strict:
        source_record = preflight["candidate_source"]
        if args.expect_candidate_source_sha256 and (
                source_record is None or
                source_record["map_sha256"] != args.expect_candidate_source_sha256):
            print(json.dumps({"status": "failed",
                              "error": "candidate Source116 SHA mismatch",
                              "expected": args.expect_candidate_source_sha256,
                              "actual": source_record["map_sha256"]
                              if source_record else None}))
            return 2
        if candidate_stdlib is None or reference_stdlib is None:
            print(json.dumps({"status": "failed",
                              "error": "cannot identify both macro stdlibs"}))
            return 2
        if args.role == "reference" and helper is None:
            print(json.dumps({"status": "failed",
                              "error": "reference helper is missing"}))
            return 2
        if args.role == "candidate" and helper is not None:
            print(json.dumps({"status": "failed",
                              "error": "candidate unexpectedly has a helper"}))
            return 2

    corpus = load_corpus(root)
    ctx = {
        "role": args.role,
        "cli": cli,
        "peer_cli": peer_cli,
        "helper": helper,
        "peer_helper": peer_helper,
        "candidate_source": candidate_source,
        "candidate_stdlib": candidate_stdlib,
        "reference_stdlib": reference_stdlib,
        "llvm_bin": args.llvm_bin.resolve() if args.llvm_bin else None,
        "llvm_tools": llvm_tools,
        "libxml": libxml,
        "strace": strace,
        "python": python,
        "readelf": readelf,
        "corpus_path": root / "selfhost/corpus.py",
        "runtime_roots": runtime_roots(cli, helper, peer_cli, peer_helper),
        "root": root,
        "out": run_root,
        "record": record_root,
        "cwd": root,
        "corpus": corpus,
        "commands": [],
    }
    base_env = env_with_resources(os.environ.copy(), ctx)
    resource_env = {}
    if "LD_LIBRARY_PATH" in base_env and base_env["LD_LIBRARY_PATH"] != os.environ.get("LD_LIBRARY_PATH"):
        resource_env["LD_LIBRARY_PATH"] = {
            "value": base_env["LD_LIBRARY_PATH"],
            "source": "runner-resource-path"}
    build_success = {cli}
    build_success |= set(llvm_tools.values())
    if args.role == "reference":
        if helper is not None:
            build_success.add(helper)
        for name in ("ld", "ld.bfd"):
            path = Path("/usr/bin") / name
            if path.is_file():
                build_success.add(path.resolve())
    shell = Path("/bin/sh").resolve()
    report = {
        "schema": "woozy/process-public-result/1",
        "operation_id": OPERATION_ID,
        "role": args.role,
        "cli": str(cli),
        "cli_sha256": digest(cli),
        "root": str(root),
        "result_root": str(out),
        "run_root": str(run_root),
        "record_root": str(record_root),
        "candidate_source": preflight["candidate_source"],
        "candidate_stdlib": preflight["candidate_stdlib"],
        "reference_stdlib": preflight["reference_stdlib"],
        "helper": str(helper) if helper else None,
        "strace": str(strace) if strace else None,
        "timeout": TIMEOUT,
        "commands": [],
        "runs": [],
        "failures": [],
        "status": "failed",
        "environment_scope": resource_env,
    }
    before = snapshot_inputs(ctx)
    write_json(record_root / "inputs.before.json", before)
    try:
        for opt in (0, 2):
            project = root / "tests/native/process/lang.toml"
            misuse_project = root / "tests/native/process/misuse/lang.toml"
            program_dir = run_root / f"process-o{opt}"
            misuse_dir = run_root / f"misuse-o{opt}"
            row = dict(opt=opt)
            report["runs"].append(row)
            row["build"] = public_row(command(
                [cli, "build", "--project", project, "--backend", "native",
                 "--opt", opt, "--out-dir", program_dir, "--format", "json"],
                label=f"build-o{opt}", phase="build", ctx=ctx,
                trace_success=build_success, artifacts_dir=program_dir,
                env=base_env, env_overrides=resource_env))
            row["elf"] = public_row(command(
                [ctx["readelf"], "-l", "-d", program_dir / "program"],
                label=f"elf-o{opt}", phase="readelf", ctx=ctx,
                trace_success={ctx["readelf"]}, env=base_env))
            headers = bytes.fromhex(row["elf"]["stdout_hex"]).decode(
                "utf-8", "backslashreplace")
            update_record(ctx["commands"][-1], "elf_observation", {
                "interp_present": "INTERP" in headers,
                "needed_present": "(NEEDED)" in headers,
            })
            if "INTERP" in headers or "(NEEDED)" in headers:
                raise EvidenceError(f"elf-o{opt}: ELF has INTERP or NEEDED")
            row["run"] = public_row(command(
                [program_dir / "program", "probe😀"],
                label=f"program-o{opt}", phase="program", ctx=ctx,
                trace_success={program_dir.resolve() / "program", shell},
                trace_failed={MISSING}, require_failed=3,
                program=program_dir / "program", require_spawn=True,
                env=base_env))
            if row["run"]["stdout_hex"] or row["run"]["stderr"]:
                raise EvidenceError(f"program-o{opt}: expected empty streams")
            invalid = dict(base_env)
            invalid["WOOZY_INVALID_UTF8"] = INVALID_UTF8
            row["invalid_environment"] = public_row(command(
                [program_dir / "program", "probe😀"],
                label=f"invalid-environment-o{opt}",
                phase="invalid_environment", ctx=ctx, expected=90,
                env=invalid, env_overrides={
                    "WOOZY_INVALID_UTF8": {
                        "hex": os.fsencode(INVALID_UTF8).hex(),
                        "display": os.fsencode(INVALID_UTF8).decode(
                            "utf-8", "backslashreplace"),
                        "source": "raw-bytes"}},
                trace_success={program_dir.resolve() / "program", shell},
                trace_failed={MISSING},
                program=program_dir / "program", require_spawn=False))
            row["invalid_argument"] = public_row(command(
                [program_dir / "program", INVALID_UTF8],
                label=f"invalid-argument-o{opt}",
                phase="invalid_argument", ctx=ctx, expected=90, env=base_env,
                trace_success={program_dir.resolve() / "program", shell},
                trace_failed={MISSING},
                program=program_dir / "program", require_spawn=False))
            if ctx["strace"] is not None:
                argument_hex = argv_hex([INVALID_UTF8])
                initial = ctx["commands"][-1][
                    "program_observation"]["initial_exec"]
                if initial is None or initial["argv_hex"][-1] != argument_hex[0]:
                    raise EvidenceError(
                        f"invalid-argument-o{opt}: raw 0xff argv was not preserved")
                if any("efbfbd" in part for part in initial["argv_hex"]):
                    raise EvidenceError(
                        f"invalid-argument-o{opt}: argv was changed to U+FFFD")
            row["misuse_build"] = public_row(command(
                [cli, "build", "--project", misuse_project, "--backend",
                 "native", "--opt", opt, "--out-dir", misuse_dir,
                 "--format", "json"],
                label=f"misuse-build-o{opt}", phase="misuse_build", ctx=ctx,
                trace_success=build_success, artifacts_dir=misuse_dir,
                env=base_env, env_overrides=resource_env))
            row["misuse"] = public_row(command(
                [misuse_dir / "program"],
                label=f"misuse-program-o{opt}", phase="misuse",
                ctx=ctx, expected=101,
                trace_success={misuse_dir.resolve() / "program"},
                trace_failed={}, env=base_env,
                program=misuse_dir / "program", require_spawn=False))
            if panic_problems(row["misuse"]["stderr"]):
                raise EvidenceError(
                    f"misuse-program-o{opt}: panic marker is missing")
        rejected_dir = run_root / "python-rejected"
        rejected = command(
            [cli, "build", "--project", root / "tests/native/process/lang.toml",
             "--backend", "python", "--out-dir", rejected_dir,
             "--format", "json"],
            label="python-rejected", phase="python_rejected", ctx=ctx,
            expected=1, trace_success={cli}, artifacts_dir=rejected_dir,
            env=base_env, env_overrides=resource_env)
        report["python_rejected"] = public_row(rejected)
        text = bytes.fromhex(rejected["stdout_hex"]).decode("utf-8")
        document = json.loads(text, parse_constant=_reject_json_constant)
        problems = python_rejection(document)
        update_record(rejected, "python_rejection", {
            "document": document, "problems": problems})
        if problems:
            raise EvidenceError("python-rejected: " + "; ".join(problems))
        if (rejected_dir / "program.py").exists():
            raise EvidenceError("python-rejected: program.py was published")
        report["status"] = "passed"
    except Exception as error:
        report["failures"].append(f"{type(error).__name__}: {error}")
    finally:
        after = snapshot_inputs(ctx)
        write_json(record_root / "inputs.after.json", after)
        report["inputs_unchanged"] = (
            before["snapshot_sha256"] == after["snapshot_sha256"])
        if not report["inputs_unchanged"]:
            report["failures"].append("input snapshot changed during the run")
        report["commands"] = ctx["commands"]
        report["command_count"] = len(ctx["commands"])
        report["actual_counts"] = {
            "total": len(ctx["commands"]),
            "builds": sum(1 for row in ctx["commands"]
                          if row["phase"] == "build"),
            "elf": sum(1 for row in ctx["commands"]
                       if row["phase"] == "readelf"),
            "programs": sum(1 for row in ctx["commands"]
                            if row["phase"] == "program"),
            "invalid_environment": sum(
                1 for row in ctx["commands"]
                if row["phase"] == "invalid_environment"),
            "invalid_argument": sum(
                1 for row in ctx["commands"]
                if row["phase"] == "invalid_argument"),
            "misuse_builds": sum(1 for row in ctx["commands"]
                                 if row["phase"] == "misuse_build"),
            "misuse_programs": sum(1 for row in ctx["commands"]
                                   if row["phase"] == "misuse"),
            "python_rejected": sum(1 for row in ctx["commands"]
                                   if row["phase"] == "python_rejected"),
        }
        if report["status"] == "passed" and report["failures"]:
            report["status"] = "failed"
        path = out / "process-source-results.json"
        write_json(path, report)
        print(json.dumps(dict(status=report["status"],
                              runs=len(report["runs"]),
                              commands=report["command_count"],
                              report=str(path))))
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    sys.exit(main())
