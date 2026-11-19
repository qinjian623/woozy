#!/usr/bin/env python3
"""Canonical floating observer harness (test-only).

Six corrected areas:
  1. canonical role is derived from the accepted proof, candidate path/SHA and
     the frozen source snapshot; there is no naked canonical flag;
  2. every subprocess requires exact exit, no timeout, complete cleanup and no
     abandoned pipes; controls must be observed, not merely recorded;
  3. a reused driver must match a successful build record, the binary, the
     driver main and every production Wz source file;
  4. all Source, fixture, vector, gold and helper reads use the actual --root;
  5. Python observation counts and each row are checked before any zip, even
     when both sides are equally short/long/wrong;
  6. a non-empty --work-dir is rejected before any log/result/staging write.

The driver itself is pure Wz and runs the real production pipeline.  The
observer is LLVM-only.  Bootstrap/helper use is allowed only to build the
test driver, never to produce Source results.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import math
import os
import re
import shutil
import signal
import sys
import tempfile
import time
import traceback
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
TOOLCHAIN_ROOT = Path(os.environ.get("WOOZY_TOOLCHAIN_ROOT") or
                      (REPO.parent / "toolchains"))
EVIDENCE_ROOT = REPO.parent / "checks"
DEFAULT_ROOT = REPO
DEFAULT_PROOF = EVIDENCE_ROOT / "native-function-type-fix-accepted-20261114.json"
DEFAULT_BOOTSTRAP = DEFAULT_ROOT / "build/woozy-stdlib-native/woozyc"
DEFAULT_TOOLCHAIN = TOOLCHAIN_ROOT / "llvm-22.1.8/bin"
DEFAULT_STRACE = TOOLCHAIN_ROOT / "strace-7.2/usr/bin/strace"
DEFAULT_PYTHON = TOOLCHAIN_ROOT / "t10-python-cpu/venv/bin/python"
TOOLS = ("opt", "llc", "clang", "clang++", "ld.lld")
REPRESENTATIVE = (
    "f32-round-ties-even", "f64-round-ties-even",
    "f32-overflow", "f64-overflow",
    "f32-subnormal", "f64-subnormal",
    "f32-zero-div-nan", "f64-zero-div-nan",
    "f32-no-fma", "f64-no-fma",
)
FULL_BASE = REPRESENTATIVE
REQUIRED_CONTROLS = (
    "canonical-proof-rejects-old-cli",
    "canonical-proof-accepts-6178",
    "cleanup-failure-rejected",
    "reuse-binary-rejected",
    "root-scoped-stage-and-vector",
    "python-count-and-value-controls",
    "nonempty-workdir-rejected",
    "timeout-rejected",
    "signal-rejected",
)


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def tree_hashes(root: Path, pattern: str = "*") -> dict[str, str]:
    root = Path(root)
    return {str(path.relative_to(root)): sha(path)
            for path in sorted(root.rglob(pattern))
            if path.is_file() and "__pycache__" not in path.parts}


def wz_tree(root: Path) -> dict[str, str]:
    return tree_hashes(root, "*.wz")


def aggregate_hashes(values: dict[str, str]) -> str:
    payload = json.dumps(values, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(payload.encode()).hexdigest()


def text_head(data: bytes, limit: int = 4096) -> str:
    return data[:limit].decode("utf-8", "replace")


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load module {name} from {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


_VALIDATION_MODULES = {}


def validation_module(name: str):
    """Use the runner repository's validation code, not the input root's."""
    if name not in _VALIDATION_MODULES:
        _VALIDATION_MODULES[name] = load_module(
            "float_observer_validation_" + name,
            HERE.parents[2] / "selfhost" / (name + ".py"))
    return _VALIDATION_MODULES[name]


def same_json(expected, actual) -> bool:
    return not validation_module("corpus").json_diff(expected, actual)


class Harness:
    """Modules and paths read from the explicit root."""

    def __init__(self, root: Path):
        self.root = root.resolve()
        self.corpus = load_module(
            "sh10q_corpus", self.root / "selfhost/corpus.py")
        self.float_expected = load_module(
            "float_expected", self.root / "tests/native/float_expected.py")
        self.floating_cases = load_module(
            "sh10q_floating_cases", self.root / "tests/python/floating_cases.py")
        self.native_runner = load_module(
            "sh10q_native_run_floating",
            self.root / "tests/native/run_floating.py")
        self.run_process = self.corpus._run_process_impl
        self.parse_trace = self.corpus.parse_trace


class CommandError(RuntimeError):
    def __init__(self, message: str, record: dict):
        super().__init__(message)
        self.record = record


class Recorder:
    def __init__(self, work: Path):
        self.work = work
        self.logs = work / "logs"
        self.logs.mkdir(parents=True)
        self.records: list[dict] = []
        self.serial = 0
        self.run_process = None

    def run(self, label: str, argv, cwd: Path, env: dict, timeout: float,
            expected_exit: int = 0, binary: bool = False):
        if self.run_process is None:
            raise RuntimeError("Recorder has no root-scoped process helper")
        self.serial += 1
        safe = re.sub(r"[^A-Za-z0-9_.-]+", "_", label)[:100]
        prefix = f"{self.serial:03d}-{safe}"
        started = time.monotonic()
        result = self.run_process(argv, cwd, env, timeout)
        elapsed = time.monotonic() - started
        out_path = self.logs / (prefix + ".stdout")
        err_path = self.logs / (prefix + ".stderr")
        out_path.write_bytes(result["stdout"])
        err_path.write_bytes(result["stderr"])
        clean = (result["timed_out"] is False and
                 result["cleanup_complete"] is True and
                 result["abandoned_pipes"] is False)
        exit_ok = result["exit"] == expected_exit
        status = "passed" if clean and exit_ok else (
            "timeout" if result["timed_out"] else
            "signaled" if result["exit"] < 0 else "failed")
        record = {
            "label": label,
            "argv": [str(value) for value in argv],
            "cwd": str(cwd),
            "status": status,
            "exit": result["exit"],
            "expected_exit": expected_exit,
            "exit_ok": exit_ok,
            "timed_out": result["timed_out"],
            "cleanup_complete": result["cleanup_complete"],
            "abandoned_pipes": result["abandoned_pipes"],
            "process_ok": clean,
            "elapsed_s": round(elapsed, 6),
            "stdout_sha256": hashlib.sha256(result["stdout"]).hexdigest(),
            "stderr_sha256": hashlib.sha256(result["stderr"]).hexdigest(),
            "stdout_bytes": len(result["stdout"]),
            "stderr_bytes": len(result["stderr"]),
            "stdout_file": str(out_path),
            "stderr_file": str(err_path),
        }
        if not binary:
            record["stdout_head"] = text_head(result["stdout"])
            record["stderr_head"] = text_head(result["stderr"])
        self.records.append(record)
        if not clean:
            raise CommandError(
                f"{label}: timeout={result['timed_out']} "
                f"cleanup={result['cleanup_complete']} "
                f"abandoned={result['abandoned_pipes']}", record)
        if not exit_ok:
            raise CommandError(
                f"{label}: exit {result['exit']} != {expected_exit}; "
                f"stdout={text_head(result['stdout'])} "
                f"stderr={text_head(result['stderr'])}", record)
        return record, result


def prepare_work_dir(path: Path) -> Path:
    """Reject non-empty directories before any harness write."""
    if path.exists():
        if path.is_symlink() or not path.is_dir():
            raise RuntimeError(f"work dir is not a real directory: {path}")
        entries = list(path.iterdir())
        if entries:
            raise RuntimeError(
                f"work dir must be empty, found {len(entries)} entries: {path}")
    else:
        path.mkdir(parents=True)
    return path


def copy_tree(source: Path, destination: Path) -> None:
    if destination.exists():
        raise RuntimeError(f"staging destination already exists: {destination}")
    shutil.copytree(source, destination,
                    ignore=shutil.ignore_patterns("__pycache__"))


def stage_driver(work: Path, root: Path, expected_count: int = 115) -> Path:
    project = work / "driver-project"
    if project.exists():
        raise RuntimeError("driver project already exists")
    (project / "src/wzcc").mkdir(parents=True)
    source_root = root / "selfhost/src/wzcc"
    sources = sorted(source_root.rglob("*.wz"))
    if len(sources) != expected_count:
        raise RuntimeError(
            f"expected {expected_count} production Wz files, found {len(sources)}")
    for source in sources:
        target = project / "src/wzcc" / source.relative_to(source_root)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
    shutil.copy2(HERE / "driver/main.wz", project / "src/main.wz")
    (project / "lang.toml").write_text(
        'schema_version=1\n[package]\nname="float_observer_driver"\n'
        'source_root="src"\nentry="app"\n[build]\nbackend="native"\n'
        'word_bits=64\n')
    return project


def find_runtime_dir(bootstrap: Path, explicit: Path | None) -> Path:
    candidates = []
    if explicit:
        candidates.append(explicit)
    candidates.extend([bootstrap.parent / "native", bootstrap.parent / "runtime"])
    for candidate in candidates:
        if ((candidate / "liblang_native_runtime.a").is_file() and
                (candidate / "liblang_hosted_runtime.a").is_file()):
            return candidate
    raise RuntimeError("cannot find native runtime archives; pass --runtime-dir")


def prepare_bundle(work: Path, name: str, binary: Path, binary_name: str,
                   stdlib: Path, runtime_dir: Path, python_runtime: Path,
                   llvm_tools: Path) -> Path:
    bundle = work / name
    if bundle.exists():
        raise RuntimeError(f"bundle already exists: {bundle}")
    bundle.mkdir()
    shutil.copy2(binary, bundle / binary_name)
    (bundle / binary_name).chmod(0o755)
    copy_tree(stdlib, bundle / "stdlib")
    (bundle / "runtime/python").mkdir(parents=True)
    for archive in ("liblang_native_runtime.a", "liblang_hosted_runtime.a"):
        shutil.copy2(runtime_dir / archive, bundle / "runtime" / archive)
    for runtime in ("unicode18.py", "langrt.py"):
        shutil.copy2(python_runtime / runtime, bundle / "runtime/python" / runtime)
    (bundle / "toolchain").mkdir()
    for tool in TOOLS:
        (bundle / "toolchain" / tool).symlink_to(llvm_tools / tool)
    return bundle


def root_identity(root: Path, proof: dict | None = None) -> dict:
    verify = root / "selfhost/src/wzcc/nativeverify/verify_ops.wz"
    values = {
        "root": str(root),
        "source_tree": wz_tree(root / "selfhost/src/wzcc"),
        "verify_ops_sha256": sha(verify),
    }
    values["source_tree_sha256"] = aggregate_hashes(values["source_tree"])
    if proof:
        canonical = Path(proof["canonical_source"])
        frozen = canonical / "src/wzcc"
        values["proof_source"] = str(canonical)
        values["proof_source_tree"] = wz_tree(frozen) if frozen.is_dir() else {}
        values["proof_source_tree_sha256"] = aggregate_hashes(
            values["proof_source_tree"])
        values["proof_source_main_sha256"] = (
            sha(canonical / "src/main.wz")
            if (canonical / "src/main.wz").is_file() else None)
        differences = []
        for name in sorted(set(values["source_tree"]) |
                           set(values["proof_source_tree"])):
            if values["source_tree"].get(name) != values["proof_source_tree"].get(name):
                differences.append(name)
        values["proof_source_differences"] = differences
    return values


def canonical_identity(candidate: Path | None, proof_path: Path, root: Path) -> dict:
    result = {"role": "provisional", "checks": {}, "reasons": []}
    if candidate is None:
        result["reasons"].append("no candidate CLI")
        return result
    result["candidate_path"] = str(candidate)
    result["candidate_sha256"] = sha(candidate)
    if not proof_path.is_file():
        result["reasons"].append(f"accepted proof is absent: {proof_path}")
        return result
    proof = json.loads(proof_path.read_text())
    result["proof_path"] = str(proof_path)
    result["proof_sha256"] = sha(proof_path)
    result["proof_validation"] = proof.get("validation")
    checks = result["checks"]
    checks["validation_accepted"] = same_json("accepted", proof.get("validation"))
    expected_path = Path(proof.get("canonical_stage1", "")).resolve()
    checks["candidate_path_matches"] = candidate.resolve() == expected_path
    checks["candidate_sha_matches"] = (
        result["candidate_sha256"] == proof.get("canonical_stage1_sha256"))
    checks["root_verify_ops_matches"] = (
        sha(root / "selfhost/src/wzcc/nativeverify/verify_ops.wz") ==
        proof.get("production_sha256"))
    canonical_source = Path(proof.get("canonical_source", ""))
    checks["proof_source_exists"] = (
        canonical_source.is_dir() and
        (canonical_source / "lang.toml").is_file() and
        (canonical_source / "src/main.wz").is_file() and
        len(list((canonical_source / "src/wzcc").rglob("*.wz"))) == 115)
    result["proof_canonical_path"] = str(expected_path)
    result["proof_canonical_sha256"] = proof.get("canonical_stage1_sha256")
    result["proof_source"] = str(canonical_source)
    result["proof_source_tree_sha256"] = (
        aggregate_hashes(wz_tree(canonical_source / "src/wzcc"))
        if checks["proof_source_exists"] else None)
    result["proof_input_digest"] = proof.get("canonical_input_digest")
    checks["proof_frozen_inputs_match"] = False
    checks["proof_input_digest_matches"] = False
    try:
        boot = validation_module("bootstrap")
        boot_dir = canonical_source.parent
        boot_result_path = boot_dir / "result.json"
        boot_result = json.loads(boot_result_path.read_text())
        expected = boot_result["frozen_inputs"]
        actual = boot._frozen_inputs(
            boot_dir, boot_result["stage0"],
            Path(expected["paths"]["selected_toolchain"]), strict=True)
        actual_digest = boot._snapshot_digest(actual)
        expected_digest = boot._snapshot_digest(expected)
        checks["proof_frozen_inputs_match"] = same_json(expected, actual)
        checks["proof_input_digest_matches"] = (
            same_json(proof.get("canonical_input_digest"), actual_digest)
            and same_json(expected_digest, actual_digest))
        result["proof_actual_input_digest"] = actual_digest
        result["proof_expected_input_digest"] = expected_digest
        result["proof_boot_result_sha256"] = sha(boot_result_path)
        result["proof_frozen_input_differences"] = validation_module(
            "corpus").json_diff(expected, actual)
    except (OSError, ValueError, KeyError, TypeError, RuntimeError) as error:
        result["proof_frozen_input_error"] = str(error)
    if all(checks.values()):
        result["role"] = "canonical"
        result["proof"] = proof
    else:
        result["reasons"].extend(
            [name for name, ok in checks.items() if not ok])
    return result


def validate_driver_evidence(driver_binary: Path, evidence_path: Path,
                             root: Path, bootstrap: Path) -> dict:
    if not evidence_path or not evidence_path.is_file():
        raise RuntimeError("reused driver requires an existing build evidence JSON")
    evidence = json.loads(evidence_path.read_text())
    required = ("argv", "status", "exit", "timed_out", "cleanup_complete",
                "driver_source_sha256", "driver_binary_sha256",
                "source_tree", "bootstrap_sha256", "helper_sha256",
                "runtime_sha256")
    missing = [name for name in required if name not in evidence]
    if missing:
        raise RuntimeError(f"driver build evidence lacks {missing}")
    root_tree = wz_tree(root / "selfhost/src/wzcc")
    checks = {
        "status_success": evidence["status"] in ("passed", "exited"),
        "exit_zero": same_json(0, evidence["exit"]),
        "not_timed_out": evidence["timed_out"] is False,
        "cleanup_complete": evidence["cleanup_complete"] is True,
        "abandoned_pipes_false": evidence.get("abandoned_pipes") is False,
        "driver_main_matches": evidence["driver_source_sha256"] ==
            sha(HERE / "driver/main.wz"),
        "driver_binary_matches": evidence["driver_binary_sha256"] ==
            sha(driver_binary),
        "source_tree_matches": same_json(evidence["source_tree"], root_tree),
        "source_tree_hash_matches": evidence.get("source_tree_sha256") ==
            aggregate_hashes(root_tree),
        "bootstrap_matches": evidence["bootstrap_sha256"] == sha(bootstrap),
    }
    if "root" in evidence:
        checks["root_matches"] = Path(evidence["root"]).resolve() == root.resolve()
    helper = bootstrap.parent / "native/lang_native_helper"
    if not helper.is_file():
        raise RuntimeError(f"C++ bootstrap helper is missing: {helper}")
    runtime = find_runtime_dir(bootstrap, None) / "liblang_native_runtime.a"
    checks["helper_matches"] = same_json(evidence["helper_sha256"], sha(helper))
    checks["runtime_matches"] = same_json(evidence["runtime_sha256"], sha(runtime))
    argv = evidence["argv"]
    checks["argv_is_build"] = (
        isinstance(argv, list) and all(isinstance(value, str) for value in argv)
        and len(argv) >= 2 and Path(argv[0]).resolve() == bootstrap.resolve()
        and argv[1] == "build" and argv.count("--backend") == 1
        and argv.index("--backend") + 1 < len(argv)
        and argv[argv.index("--backend") + 1] == "native")
    if not all(checks.values()):
        raise RuntimeError(
            f"driver build evidence rejected: "
            f"{[name for name, ok in checks.items() if not ok]}")
    evidence["file_sha256"] = sha(evidence_path)
    evidence["validated_checks"] = checks
    return evidence


def build_driver(work: Path, root: Path, bootstrap: Path, recorder: Recorder,
                 env: dict, timeout: float) -> tuple[Path, dict, dict]:
    project = stage_driver(work, root)
    source_tree = wz_tree(project / "src/wzcc")
    if source_tree != wz_tree(root / "selfhost/src/wzcc"):
        raise RuntimeError("staged driver source differs from root")
    argv = [bootstrap, "build", "--project", project / "lang.toml",
            "--backend", "native", "--opt", "0", "--out-dir",
            work / "driver-build", "--format", "json"]
    record, result = recorder.run("driver-build", argv, work, env, timeout,
                                  expected_exit=0)
    binary = work / "driver-build/program"
    if not binary.is_file():
        raise RuntimeError("driver build did not produce program")
    runtime_dir = find_runtime_dir(bootstrap, None)
    evidence = {
        "argv": [str(value) for value in argv],
        "cwd": str(work),
        "status": record["status"],
        "exit": record["exit"],
        "timed_out": record["timed_out"],
        "cleanup_complete": record["cleanup_complete"],
        "abandoned_pipes": record["abandoned_pipes"],
        "elapsed_s": record["elapsed_s"],
        "stdout_sha256": record["stdout_sha256"],
        "stderr_sha256": record["stderr_sha256"],
        "stdout_file": record["stdout_file"],
        "stderr_file": record["stderr_file"],
        "driver_source_sha256": sha(HERE / "driver/main.wz"),
        "driver_binary_sha256": sha(binary),
        "source_tree": source_tree,
        "source_tree_sha256": aggregate_hashes(source_tree),
        "bootstrap": str(bootstrap),
        "bootstrap_sha256": sha(bootstrap),
        "helper_sha256": sha(bootstrap.parent / "native/lang_native_helper"),
        "runtime_sha256": sha(
            runtime_dir / "liblang_native_runtime.a"),
        "root": str(root),
        "root_identity": root_identity(root),
    }
    path = work / "driver-build-evidence.json"
    path.write_text(json.dumps(evidence, indent=2) + "\n")
    evidence["file_sha256"] = sha(path)
    return binary, evidence, {"project": project, "record": record}


def compile_observer(work: Path, recorder: Recorder, env: dict,
                     clangxx: Path, llvm_config: Path, json_include: Path,
                     timeout: float) -> Path:
    out = work / "observer" / "llvm_observer"
    out.parent.mkdir(parents=True)
    cxxflags = recorder.run(
        "observer-cxxflags", [llvm_config, "--cxxflags"], work, env,
        timeout, expected_exit=0)[1]["stdout"].decode().split()
    ldflags = recorder.run(
        "observer-ldflags", [llvm_config, "--ldflags"], work, env,
        timeout, expected_exit=0)[1]["stdout"].decode().split()
    libs = recorder.run(
        "observer-libs", [llvm_config, "--libs", "asmparser", "core",
                          "support"], work, env, timeout,
        expected_exit=0)[1]["stdout"].decode().split()
    argv = [clangxx, "-std=c++20", *cxxflags, "-fexceptions", "-fno-rtti",
            "-O2", f"-I{json_include}", HERE / "llvm_observer.cpp", *ldflags,
            *libs, "-ljson-c", "-lz", "-lzstd", "-lxml2", "-ldl", "-lm",
            "-lpthread", "-o", out]
    recorder.run("observer-build", argv, work, env, timeout, expected_exit=0)
    return out


def prepare_fixture(work: Path, root: Path, case: dict,
                    derive_main: bool = False) -> dict:
    source = root / case["manifest"]
    if not source.is_file():
        raise RuntimeError(f"fixture manifest is missing under root: {source}")
    destination = work / "fixtures" / case["id"]
    copy_tree(source.parent, destination)
    original_main = destination / "src/main.wz"
    original_hash = sha(original_main)
    derived = False
    if derive_main:
        text = original_main.read_text()
        index = text.find("func main()")
        if index < 0:
            raise RuntimeError(f"{case['id']}: cannot derive main")
        original_main.write_text(text[:index] + "func main() i32 { return 0; }\n")
        derived = True
    return {
        "id": case["id"],
        "manifest": destination / "lang.toml",
        "source": destination / "src/main.wz",
        "root": destination,
        "original_source_sha256": original_hash,
        "source_sha256": sha(original_main),
        "manifest_sha256": sha(destination / "lang.toml"),
        "derived_main_return_zero": derived,
        "probes": case["runtime_expected"].get("probes", []),
        "main_return": case["runtime_expected"].get("main_return", 0),
        "backends": ["native", "python"],
    }


def make_dynamic_fixture(work: Path, harness: Harness) -> dict:
    root = work / "fixtures/dynamic"
    if root.exists():
        raise RuntimeError("dynamic fixture already exists")
    (root / "src").mkdir(parents=True)
    (root / "src/main.wz").write_text(
        "package app;\n" + harness.floating_cases.dynamic_source())
    (root / "lang.toml").write_text(
        'schema_version=1\n[package]\nname="app"\nsource_root="src"\n'
        'entry="app"\n[build]\nbackend="native"\nword_bits=64\n')
    return {
        "id": "dynamic",
        "manifest": root / "lang.toml",
        "source": root / "src/main.wz",
        "root": root,
        "source_sha256": sha(root / "src/main.wz"),
        "manifest_sha256": sha(root / "lang.toml"),
        "derived_main_return_zero": False,
        "probes": [],
        "main_return": 0,
        "backends": ["python"],
    }


def make_native_dynamic_fixture(work: Path, root: Path,
                                probes: list[dict]) -> dict:
    source = root / "tests/native/floating/dynamic/lang.toml"
    destination = work / "fixtures/native-dynamic"
    copy_tree(source.parent, destination)
    return {
        "id": "native-dynamic",
        "manifest": destination / "lang.toml",
        "source": destination / "src/main.wz",
        "root": destination,
        "source_sha256": sha(destination / "src/main.wz"),
        "manifest_sha256": sha(destination / "lang.toml"),
        "derived_main_return_zero": False,
        "probes": probes,
        "main_return": 0,
        "backends": ["native"],
    }


def make_cleanup_fixture(work: Path, root: Path,
                         probes: list[dict]) -> dict:
    source = root / "tests/native/floating/values-cleanup/lang.toml"
    destination = work / "fixtures/values-cleanup"
    copy_tree(source.parent, destination)
    return {
        "id": "values-cleanup",
        "manifest": destination / "lang.toml",
        "source": destination / "src/main.wz",
        "root": destination,
        "source_sha256": sha(destination / "src/main.wz"),
        "manifest_sha256": sha(destination / "lang.toml"),
        "derived_main_return_zero": False,
        "probes": probes,
        "main_return": 0,
        "backends": ["native"],
    }


def make_panic_fixture(work: Path) -> dict:
    root = work / "fixtures/panic-control"
    (root / "src").mkdir(parents=True)
    (root / "src/main.wz").write_text(
        'package app;\n'
        'func probe_panic() f32 {var z i32=0;let q=1/z;return 1.0;}\n'
        'func main() i32 {return 0;}\n')
    (root / "lang.toml").write_text(
        'schema_version=1\n[package]\nname="app"\nsource_root="src"\n'
        'entry="app"\n[build]\nbackend="native"\nword_bits=64\n')
    return {"id": "panic-control", "manifest": root / "lang.toml",
            "source": root / "src/main.wz", "root": root,
            "source_sha256": sha(root / "src/main.wz"),
            "manifest_sha256": sha(root / "lang.toml"),
            "derived_main_return_zero": False,
            "probes": [{"function": "probe_panic", "arguments": [],
                        "result": {"type": "f32", "class": "finite",
                                   "bits": "3f800000", "sign": "positive"},
                        "expected_source": "panic control"}],
            "main_return": 0, "backends": ["native"]}


def vector_rows(root: Path) -> list[dict]:
    path = root / "tests/python/floating_vectors.tsv"
    rows = []
    for line in path.read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        width, op, a, b, category, bits, sign = line.split("\t")
        rows.append(dict(width=width, op=op, a=a, b=b, category=category,
                         bits=bits, sign=sign, line=line))
    return rows


def vector_probe(row: dict) -> dict:
    width = int(row["width"])
    typ = f"f{width}"
    if row["category"] == "bool":
        expected = {"type": "bool", "class": "bool",
                    "value": row["bits"] == "true"}
    else:
        expected = {"type": typ, "class": row["category"],
                    "bits": None if row["category"] == "nan" else row["bits"],
                    "sign": None if row["category"] == "nan" else row["sign"]}
    arguments = [{"type": typ, "bits": row["a"]}]
    if row["op"] != "neg":
        arguments.append({"type": typ, "bits": row["b"]})
    return {"function": row["op"] + row["width"],
            "arguments": arguments, "result": expected,
            "expected_source": "tests/python/floating_vectors.tsv APFloat 22.1.8"}


def request_text(probes: list[dict]) -> str:
    return "".join(
        f"{probe['function']}\t{probe['result']['type']}" +
        "".join("\t" + argument["bits"]
                for argument in probe.get("arguments", [])) + "\n"
        for probe in probes)


def compatible(actual: dict, expected: dict) -> bool:
    if not isinstance(actual, dict) or not isinstance(expected, dict):
        return False
    if expected["type"] == "bool":
        return (type(actual.get("value")) is bool
                and type(expected.get("value")) is bool
                and ("type" not in actual or same_json(actual["type"], "bool"))
                and same_json(actual.get("class"), "bool")
                and same_json(actual["value"], expected["value"]))
    if not same_json(actual.get("type"), expected["type"]):
        return False
    if not same_json(actual.get("class"), expected.get("class")):
        return False
    if expected.get("class") == "nan":
        return True
    return (same_json(actual.get("bits"), expected.get("bits")) and
            same_json(actual.get("sign"), expected.get("sign")))


def observation_values(data: bytes, probes: list[dict]) -> dict:
    if len(data) < 4:
        raise RuntimeError("observation output lacks MXCSR")
    mxcsr = int.from_bytes(data[:4], "little")
    environment = dict(mxcsr=f"{mxcsr:08x}", rounding=(mxcsr >> 13) & 3,
                       ftz=bool(mxcsr & (1 << 15)),
                       daz=bool(mxcsr & (1 << 6)),
                       exceptions_masked=(mxcsr & 0x1f80) == 0x1f80)
    if (mxcsr & 0xe040) != 0 or (mxcsr & 0x1f80) != 0x1f80:
        raise RuntimeError(f"unexpected floating environment: {environment}")
    offset = 4
    values = []
    for probe in probes:
        expected = probe["result"]
        width = 1 if expected["type"] == "bool" else int(expected["type"][1:]) // 8
        if offset + width > len(data):
            raise RuntimeError(
                f"observation output is short at probe {len(values)}")
        raw = int.from_bytes(data[offset:offset + width], "little")
        offset += width
        if width == 1:
            if raw not in (0, 1):
                raise RuntimeError(f"bool observation byte is not 0/1: {raw}")
            actual = {"type": "bool", "class": "bool", "value": bool(raw)}
        else:
            actual = HarnessActive.float_expected.result(raw, width * 8)
            actual["observed_bits"] = f"{raw:0{width * 2}x}"
        values.append({"function": probe["function"], "expected": expected,
                       "actual": actual, "status":
                       "passed" if compatible(actual, expected) else "failed"})
    if len(values) != len(probes):
        raise RuntimeError(
            f"observation count {len(values)} != requested {len(probes)}")
    if offset != len(data):
        raise RuntimeError(
            f"observation output has {len(data) - offset} trailing bytes")
    return {"environment": environment, "values": values}


class HarnessActive:
    float_expected = None


def metadata_functions(metadata: dict) -> dict:
    return {item["source_name"]: item for item in metadata["functions"]}


def check_metadata(metadata: dict, probes: list[dict]) -> None:
    functions = metadata_functions(metadata)
    for probe in probes:
        if probe["function"] not in functions:
            raise RuntimeError(f"metadata lacks {probe['function']}")
        item = functions[probe["function"]]
        if item.get("status") != "covered":
            raise RuntimeError(
                f"metadata does not cover {probe['function']}: {item}")
        expected_parameters = [argument["type"]
                               for argument in probe.get("arguments", [])]
        if item["parameters"] != expected_parameters:
            raise RuntimeError(
                f"metadata parameters differ for {probe['function']}: "
                f"{item['parameters']} != {expected_parameters}")
        if item["result"] != probe["result"]["type"]:
            raise RuntimeError(
                f"metadata result differs for {probe['function']}")


def validate_python_observations(probes: list[dict], driver_rows: list[dict],
                                 cli_rows: list[dict] | None = None) -> list[dict]:
    requested = len(probes)
    if len(driver_rows) != requested:
        raise RuntimeError(
            f"Python driver observation count {len(driver_rows)} != {requested}")
    if cli_rows is not None and len(cli_rows) != requested:
        raise RuntimeError(
            f"Python CLI observation count {len(cli_rows)} != {requested}")
    values = []
    for index, (probe, driver) in enumerate(zip(probes, driver_rows)):
        if not same_json(driver.get("function"), probe["function"]):
            raise RuntimeError(
                f"Python driver order mismatch at {index}: "
                f"{driver.get('function')} != {probe['function']}")
        if not same_json(driver.get("outcome"), "return"):
            raise RuntimeError(
                f"Python driver outcome for {probe['function']} is "
                f"{driver.get('outcome')}")
        signature = driver.get("signature", {})
        parameters = [argument["type"] for argument in probe.get("arguments", [])]
        if not same_json(signature.get("result"), probe["result"]["type"]) or not same_json(
                signature.get("parameters"), parameters):
            raise RuntimeError(
                f"Python driver signature result mismatch for "
                f"{probe['function']}")
        if not compatible(driver["result"], probe["result"]):
            raise RuntimeError(
                f"Python driver value mismatch for {probe['function']}: "
                f"{driver['result']} != {probe['result']}")
        if cli_rows is not None:
            cli = cli_rows[index]
            if not same_json(cli.get("function"), probe["function"]):
                raise RuntimeError(
                    f"Python CLI order mismatch at {index}: "
                    f"{cli.get('function')} != {probe['function']}")
            if not same_json(cli.get("outcome"), "return"):
                raise RuntimeError(
                    f"Python CLI outcome for {probe['function']} is "
                    f"{cli.get('outcome')}")
            cli_signature = cli.get("signature", {})
            if not same_json(cli_signature.get("result"), probe["result"]["type"]) or not same_json(
                    cli_signature.get("parameters"), parameters):
                raise RuntimeError(f"Python CLI signature mismatch for {probe['function']}")
            if not compatible(cli.get("result"), probe["result"]):
                raise RuntimeError(f"Python CLI result mismatch for {probe['function']}")
            if not same_json(cli.get("result"), driver.get("result")):
                raise RuntimeError(
                    f"Python CLI/driver result mismatch for "
                    f"{probe['function']}")
        values.append({"function": probe["function"],
                       "expected": probe["result"],
                       "actual": driver["result"],
                       "status": "passed"})
    if cli_rows is not None and len(cli_rows) != len(driver_rows):
        raise RuntimeError("Python driver/CLI observation counts differ")
    return values


def trace_check(trace: Path, allowed: set[str], label: str,
                parse_trace) -> dict:
    observed = set(parse_trace(trace))
    unexpected = sorted(observed - allowed)
    if unexpected:
        raise RuntimeError(f"{label}: unexpected execs: {unexpected}")
    if not observed:
        raise RuntimeError(f"{label}: no execve/execveat observed")
    return {"trace": str(trace), "executed": sorted(observed),
            "allowed": sorted(allowed), "status": "passed"}


def execute_program(recorder: Recorder, label: str, program: Path,
                    expected_exit: int, env: dict, timeout: float) -> dict:
    record, result = recorder.run(label, [program], program.parent, env,
                                  timeout, expected_exit=expected_exit,
                                  binary=True)
    if result["stdout"] or result["stderr"]:
        raise RuntimeError(f"{label}: unexpected output")
    return record


def python_observe(recorder: Recorder, root: Path, label: str, python: Path,
                   observation_dir: Path, probes_path: Path, env: dict,
                   timeout: float) -> list[dict]:
    script = root / "tests/python/observe_floating.py"
    record, result = recorder.run(
        label, [python, "-B", "-s", script, observation_dir, probes_path],
        observation_dir, env, timeout, expected_exit=0)
    return json.loads(result["stdout"])


def make_root_control(work: Path, name: str, different_text: str,
                      vector_line: str) -> Path:
    root = work / "root-controls" / name
    (root / "selfhost/src/wzcc").mkdir(parents=True)
    (root / "selfhost/src/wzcc/different.wz").write_text(different_text)
    (root / "tests/python").mkdir(parents=True)
    (root / "tests/python/floating_vectors.tsv").write_text(
        "# control\n" + vector_line + "\n")
    (root / "tests/floating").mkdir(parents=True)
    (root / "tests/floating/cases.json").write_text(
        json.dumps({"cases": []}) + "\n")
    (root / "tests/native").mkdir(parents=True)
    (root / "tests/native/float_expected.py").write_text(
        "def result(raw, width):\n    return {}\n")
    return root


def dirty_cleanup_control(recorder: Recorder, work: Path, python: Path,
                          env: dict, timeout: float) -> tuple[bool, dict | None]:
    """Generate the escaping child ourselves and stop that exact child."""
    dirty_dir = work / "dirty-cleanup"
    dirty_dir.mkdir()
    dirty = dirty_dir / "parent.py"
    child_pid_file = dirty_dir / "child.pid"
    dirty.write_text(
        "import subprocess,sys\nfrom pathlib import Path\n"
        "child=subprocess.Popen([sys.executable,'-c','import time; time.sleep(30)'],"
        "start_new_session=True)\n"
        "Path(sys.argv[1]).write_text(str(child.pid))\n")
    cleanup_detected = False
    cleanup_record = None
    try:
        try:
            recorder.run("control-dirty-cleanup",
                         [python, "-B", "-s", dirty, child_pid_file], dirty_dir,
                         env, timeout, expected_exit=0)
        except CommandError as error:
            cleanup_detected = not error.record["cleanup_complete"] or \
                error.record["abandoned_pipes"]
            cleanup_record = error.record
    finally:
        if child_pid_file.exists():
            child_pid = int(child_pid_file.read_text())
            try:
                os.kill(child_pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            for _ in range(40):
                state = Path(f"/proc/{child_pid}/stat")
                if not state.exists() or state.read_text().split()[2] == "Z":
                    break
                time.sleep(0.025)
            else:
                raise RuntimeError("dirty-cleanup control child did not stop")
    return cleanup_detected, cleanup_record


def run_controls(work: Path, root: Path, canonical: Path, old: Path,
                 proof_path: Path, bootstrap: Path, toolchain: Path,
                 python: Path, env: dict, timeout: float) -> dict:
    work = prepare_work_dir(work)
    recorder = Recorder(work)
    harness = Harness(root)
    recorder.run_process = harness.run_process
    HarnessActive.float_expected = harness.float_expected
    controls = []

    def add(name: str, detected: bool, **extra):
        controls.append({"name": name, "detected": detected, **extra})

    # 1. Canonical proof identity, both directions.
    old_role = canonical_identity(old, proof_path, root)
    add("canonical-proof-rejects-old-cli",
        old_role["role"] != "canonical",
        actual=old_role["candidate_sha256"],
        expected=json.loads(proof_path.read_text())["canonical_stage1_sha256"],
        role=old_role["role"])
    canonical_role = canonical_identity(canonical, proof_path, root)
    add("canonical-proof-accepts-6178",
        canonical_role["role"] == "canonical",
        candidate=canonical_role.get("candidate_sha256"),
        checks=canonical_role.get("checks"))

    # 2. Cleanup failure must be rejected by Recorder, never accepted.
    cleanup_detected, cleanup_record = dirty_cleanup_control(
        recorder, work, python, env, timeout)
    add("cleanup-failure-rejected", cleanup_detected,
        record=cleanup_record or {"missing": "dirty-cleanup record"})

    # 3. Fake reuse must be rejected; contradictory evidence must be rejected.
    fake_binary = Path("/bin/true")
    fake_tree = work / "fake-driver-evidence.json"
    fake_tree.write_text(json.dumps({
        "argv": [str(fake_binary)], "status": "exited", "exit": 9,
        "timed_out": True, "cleanup_complete": False,
        "driver_source_sha256": "0" * 64,
        "driver_binary_sha256": "0" * 64,
        "source_tree": {"different.wz": "0" * 64},
        "bootstrap_sha256": "0" * 64,
    }) + "\n")
    reuse_detected = False
    try:
        validate_driver_evidence(fake_binary, fake_tree, root, bootstrap)
    except RuntimeError:
        reuse_detected = True
    add("reuse-binary-rejected", reuse_detected,
        binary=str(fake_binary), binary_sha256=sha(fake_binary))

    # 4. Root scoping: stage and vectors must come from the actual root.
    root_a = make_root_control(
        work, "a", "package a;\nfunc a() {}\n",
        "32\tadd\t00000000\t00000000\tzero\t00000000\tpositive")
    root_b = make_root_control(
        work, "b", "package b;\nfunc b() {}\n",
        "64\tmul\t0000000000000000\t0000000000000000\tzero\t0000000000000000\tpositive")
    stage_a = stage_driver(work / "stage-a", root_a, expected_count=1)
    stage_b = stage_driver(work / "stage-b", root_b, expected_count=1)
    vectors_a = vector_rows(root_a)
    vectors_b = vector_rows(root_b)
    root_detected = (
        (stage_a / "src/wzcc/different.wz").is_file() and
        (stage_b / "src/wzcc/different.wz").is_file() and
        "package a" in (stage_a / "src/wzcc/different.wz").read_text() and
        "package b" in (stage_b / "src/wzcc/different.wz").read_text() and
        vectors_a[0]["width"] == "32" and
        vectors_b[0]["width"] == "64")
    add("root-scoped-stage-and-vector", root_detected,
        a=aggregate_hashes(wz_tree(stage_a / "src/wzcc")),
        b=aggregate_hashes(wz_tree(stage_b / "src/wzcc")))

    # 5. Actual Python process returning [] and short/long/wrong lists.
    python_probe = work / "python-controls"
    python_probe.mkdir()
    probes = [{"function": "f", "arguments": [],
               "result": {"type": "f32", "class": "finite",
                          "bits": "3f800000", "sign": "positive"}}]
    good = [{"function": "f", "outcome": "return",
             "signature": {"result": "f32", "parameters": []},
             "result": {"type": "f32", "class": "finite",
                        "bits": "3f800000", "sign": "positive"}}]
    validate_python_observations(probes, good, good)
    wrong = json.loads(json.dumps(good))
    wrong[0]["result"]["bits"] = "3f800001"
    python_cases = {
        "empty": [],
        "short": [],
        "long": good + good,
        "wrong": wrong,
    }
    python_detected = True
    python_details = {}
    for name, payload in python_cases.items():
        script = python_probe / f"{name}.py"
        script.write_text("import json\nprint(json.dumps(" +
                          repr(payload) + "))\n")
        record, result = recorder.run(
            f"control-python-{name}", [python, "-B", "-s", script],
            python_probe, env, timeout, expected_exit=0)
        rows = json.loads(result["stdout"])
        detected = True
        try:
            validate_python_observations(probes, rows, rows)
            detected = False
        except RuntimeError:
            pass
        python_details[name] = detected
        python_detected = python_detected and detected
    add("python-count-and-value-controls", python_detected,
        **python_details)

    # 6. Non-empty work directory must be rejected before any write.
    nonempty = work / "nonempty"
    nonempty.mkdir()
    old_file = nonempty / "results.json"
    old_file.write_text("old evidence\n")
    old_link = nonempty / "link"
    old_link.symlink_to(old_file)
    before = (sha(old_file), old_link.is_symlink(),
              os.readlink(old_link), sha(old_file))
    work_rejected = False
    try:
        prepare_work_dir(nonempty)
    except RuntimeError:
        work_rejected = True
    after = (sha(old_file), old_link.is_symlink(),
             os.readlink(old_link), sha(old_file))
    add("nonempty-workdir-rejected", work_rejected and before == after,
        before=before, after=after)

    timeout_detected = False
    timeout_record = None
    try:
        recorder.run("control-timeout", ["/bin/sh", "-c", "sleep 5"],
                     work, env, 0.2, expected_exit=0)
    except CommandError as error:
        timeout_detected = (error.record["timed_out"] is True and
                            error.record["exit_ok"] is False)
        timeout_record = error.record
    add("timeout-rejected", timeout_detected, record=timeout_record)

    signal_detected = False
    signal_record = None
    try:
        recorder.run("control-signal", ["/bin/sh", "-c", "kill -TERM $$"],
                     work, env, timeout, expected_exit=0)
    except CommandError as error:
        signal_detected = (error.record["timed_out"] is False and
                           error.record["exit"] == -15 and
                           error.record["exit_ok"] is False)
        signal_record = error.record
    add("signal-rejected", signal_detected, record=signal_record)

    required = {item["name"]: item["detected"] for item in controls}
    missing = [name for name in REQUIRED_CONTROLS if name not in required]
    failed = [name for name in REQUIRED_CONTROLS
              if not required.get(name, False)]
    positive_clean = all(
        record["process_ok"] for record in recorder.records
        if record["label"] not in ("control-dirty-cleanup",
                                  "control-timeout", "control-signal"))
    status = "passed" if (not missing and not failed and positive_clean and
                          cleanup_detected) else "failed"
    return {"status": status, "controls": controls, "missing": missing,
            "failed": failed, "records": recorder.records}


def full_fixtures(work: Path, root: Path, harness: Harness,
                  profile: str) -> list[dict]:
    cases = {case["id"]: case for case in
             json.loads((root / "tests/floating/cases.json").read_text())["cases"]}
    if profile == "representative":
        fixtures = []
        for ident in REPRESENTATIVE:
            case = cases[ident]
            fixture = prepare_fixture(work, root, case, derive_main=False)
            fixtures.append(fixture)
        dynamic = make_dynamic_fixture(work, harness)
        rows = vector_rows(root)
        chosen = []
        seen = set()
        selectors = [
            lambda r: r["category"] == "infinity",
            lambda r: r["category"] == "nan",
            lambda r: r["category"] == "zero" and r["sign"] == "negative",
            lambda r: r["category"] == "bool" and r["bits"] == "false",
            lambda r: r["op"] == "div" and r["width"] == "32",
            lambda r: r["op"] == "div" and r["width"] == "64",
        ]
        for predicate in selectors:
            for row in rows:
                if row["line"] in seen or not predicate(row):
                    continue
                seen.add(row["line"])
                chosen.append(vector_probe(row))
                break
        dynamic["probes"] = chosen
        fixtures.append(dynamic)
        return fixtures

    fixtures = []
    for ident in FULL_BASE:
        case = cases[ident]
        if case["expected_static"]["status"] != "accept":
            raise RuntimeError(f"{ident} is not accepted")
        fixtures.append(prepare_fixture(work, root, case, derive_main=False))
    native_dynamic = harness.native_runner.additional()
    fixtures.append(make_native_dynamic_fixture(
        work, root, native_dynamic))
    cleanup_values = [
        ("probe_values32", 32, 0x41280000),
        ("probe_values64", 64, 0x4008000000000000),
        ("probe_order", 32, 0x42240000),
        ("probe_cleanup", 32, 0x40b80000),
        ("probe_same_type", 64, 0x400e000000000000),
        ("probe_inout", 32, 0x40400000),
    ]
    cleanup = [{"function": name,
                "result": harness.float_expected.result(raw, width),
                "expected_source": "tests/native/run_floating.py values-cleanup"}
               for name, width, raw in cleanup_values]
    fixtures.append(make_cleanup_fixture(work, root, cleanup))
    py_dynamic = make_dynamic_fixture(work, harness)
    py_dynamic["probes"] = [vector_probe(row)
                            for row in vector_rows(root)]
    fixtures.append(py_dynamic)
    return fixtures


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--woozyc", type=Path, default=DEFAULT_BOOTSTRAP,
                        help="bootstrap used once to build the Wz driver")
    parser.add_argument("--driver-binary", type=Path)
    parser.add_argument("--driver-build-evidence", type=Path)
    parser.add_argument("--candidate-cli", type=Path)
    parser.add_argument("--old-candidate", type=Path,
                        help="pre-fix CLI; required with --controls-only")
    parser.add_argument("--accepted-proof", type=Path, default=DEFAULT_PROOF)
    parser.add_argument("--root", type=Path, default=DEFAULT_ROOT)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--stdlib", type=Path)
    parser.add_argument("--runtime-dir", type=Path)
    parser.add_argument("--python-runtime", type=Path)
    parser.add_argument("--python", type=Path, default=DEFAULT_PYTHON)
    parser.add_argument("--toolchain", type=Path, default=DEFAULT_TOOLCHAIN)
    parser.add_argument("--strace", type=Path, default=DEFAULT_STRACE)
    parser.add_argument("--json-include", type=Path, default=Path("/usr/include/json-c"))
    parser.add_argument("--build-timeout", type=float, default=3600)
    parser.add_argument("--command-timeout", type=float, default=180)
    parser.add_argument("--observer-timeout", type=float, default=600)
    parser.add_argument("--profile", choices=("representative", "full"),
                        default="full")
    parser.add_argument("--controls-only", action="store_true")
    args = parser.parse_args(argv)
    if not all(math.isfinite(value) and value > 0 for value in (
            args.build_timeout, args.command_timeout, args.observer_timeout)):
        parser.error("timeouts must be finite and positive")
    root = args.root.resolve()
    bootstrap = args.woozyc.resolve()
    toolchain = args.toolchain.resolve()
    if args.controls_only:
        if args.old_candidate is None:
            parser.error("--controls-only requires --old-candidate: the pre-fix "
                         "CLI recorded in the accepted proof is an external "
                         "input")
        if not args.accepted_proof.is_file():
            parser.error(f"accepted proof is absent: {args.accepted_proof}")
        work = prepare_work_dir(
            args.work_dir.resolve() if args.work_dir else
            Path(tempfile.mkdtemp(prefix="float-controls-")))
        env = dict(os.environ)
        env["PYTHONDONTWRITEBYTECODE"] = "1"
        env["PYTHONNOUSERSITE"] = "1"
        result = run_controls(work, root, args.candidate_cli.resolve()
                              if args.candidate_cli else Path(
                                  json.loads(args.accepted_proof.read_text())
                                  ["canonical_stage1"]),
                              args.old_candidate.resolve(), args.accepted_proof.resolve(),
                              bootstrap, toolchain, args.python.resolve(),
                              env, args.command_timeout)
        result["work_dir"] = str(work)
        (work / "controls.json").write_text(
            json.dumps(result, indent=2) + "\n")
        print(json.dumps({"status": result["status"],
                          "work_dir": str(work),
                          "failed": result["failed"],
                          "missing": result["missing"]}, indent=2))
        return 0 if result["status"] == "passed" else 1

    work = prepare_work_dir(
        args.work_dir.resolve() if args.work_dir else
        Path(tempfile.mkdtemp(prefix="float-observer-")))
    recorder = Recorder(work)
    harness = Harness(root)
    recorder.run_process = harness.run_process
    HarnessActive.float_expected = harness.float_expected
    env = dict(os.environ)
    compat = str(TOOLCHAIN_ROOT / "libxml2-legacy-2.13.9/usr/lib")
    env["LD_LIBRARY_PATH"] = compat + (
        ":" + env["LD_LIBRARY_PATH"] if env.get("LD_LIBRARY_PATH") else "")
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    env["PYTHONNOUSERSITE"] = "1"
    stdlib = (args.stdlib or (root / "stdlib")).resolve()
    python_runtime = (args.python_runtime or
                      (root / "runtime/python")).resolve()
    candidate = (args.candidate_cli.resolve() if args.candidate_cli else None)
    if candidate is None and args.accepted_proof.is_file():
        proof = json.loads(args.accepted_proof.read_text())
        if proof.get("validation") == "accepted":
            candidate = Path(proof["canonical_stage1"]).resolve()
    started = time.monotonic()
    report = {
        "operation_id": "SH10q-floating-observer-single-correction-20261114",
        "contract": "compiler-dev/1",
        "status": "failed",
        "work_dir": str(work),
        "harness_source_sha256": sha(Path(__file__).resolve()),
        "root": str(root),
        "root_identity": root_identity(root),
        "bootstrap": {"path": str(bootstrap), "sha256": sha(bootstrap)},
        "candidate": canonical_identity(candidate, args.accepted_proof.resolve(), root),
        "tool_sha256": {name: sha(toolchain / name) for name in TOOLS},
        "python": {"path": str(args.python.resolve()), "version": None},
        "inputs": {}, "resources": {}, "runs": [], "controls": [],
        "not_run": [], "failures": [],
    }
    try:
        py_record, py_result = recorder.run(
            "python-version", [args.python, "--version"], work, env,
            args.command_timeout, expected_exit=0)
        report["python"]["version"] = py_result["stdout"].decode().strip()
        if report["python"]["version"] != "Python 3.12.15":
            raise RuntimeError(f"unexpected Python: {report['python']['version']}")
        report["inputs"]["driver_source_sha256"] = sha(HERE / "driver/main.wz")
        report["inputs"]["observer_source_sha256"] = sha(HERE / "llvm_observer.cpp")
        report["inputs"]["stdlib_tree"] = tree_hashes(stdlib)
        report["inputs"]["python_runtime_tree"] = tree_hashes(python_runtime)
        report["inputs"]["cases_json_sha256"] = sha(
            root / "tests/floating/cases.json")
        report["inputs"]["vector_tsv_sha256"] = sha(
            root / "tests/python/floating_vectors.tsv")
        if report["root_identity"]["verify_ops_sha256"] != \
                "a9c375953da70100fbeae8d1220f37cac72788d9a162d0fd3428e24e08da3a48":
            raise RuntimeError("root verify_ops is not the accepted a9c375 source")

        if args.driver_binary:
            driver_binary = args.driver_binary.resolve()
            evidence = validate_driver_evidence(
                driver_binary, args.driver_build_evidence, root, bootstrap)
            build_info = {"reused": True}
        else:
            driver_binary, evidence, build_info = build_driver(
                work, root, bootstrap, recorder, env, args.build_timeout)
        report["driver"] = {
            "binary": str(driver_binary), "sha256": sha(driver_binary),
            "evidence": evidence, "build": build_info,
            "evidence_sha256": evidence.get("file_sha256"),
        }
        runtime_dir = find_runtime_dir(bootstrap, args.runtime_dir)
        report["inputs"]["runtime_tree"] = tree_hashes(runtime_dir)
        driver_bundle = prepare_bundle(
            work, "driver-bundle", driver_binary, "driver",
            stdlib, runtime_dir, python_runtime, toolchain)
        candidate_bundle = None
        if candidate:
            candidate_bundle = prepare_bundle(
                work, "candidate-bundle", candidate, "woozyc",
                stdlib, runtime_dir, python_runtime, toolchain)
        report["resources"]["driver_bundle"] = tree_hashes(driver_bundle)
        if candidate_bundle:
            report["resources"]["candidate_bundle"] = tree_hashes(candidate_bundle)
        observer = compile_observer(
            work, recorder, env, toolchain / "clang++",
            toolchain / "llvm-config", args.json_include,
            args.observer_timeout)
        report["observer"] = {"binary": str(observer), "sha256": sha(observer)}

        fixtures = full_fixtures(work, root, harness, args.profile)
        panic = make_panic_fixture(work)
        report["inputs"]["fixtures"] = {
            fixture["id"]: {
                "source_sha256": fixture["source_sha256"],
                "manifest_sha256": fixture["manifest_sha256"],
                "original_source_sha256": fixture.get("original_source_sha256"),
                "derived_main_return_zero": fixture.get(
                    "derived_main_return_zero", False),
                "probes": len(fixture["probes"]),
                "backends": fixture.get("backends", []),
            } for fixture in fixtures + [panic]}

        def run_driver(fixture: dict, backend: str, opt: int) -> dict:
            run_dir = work / "runs" / fixture["id"] / backend / f"O{opt}"
            run_dir.mkdir(parents=True)
            trace = run_dir / "driver.strace"
            trace_enabled = fixture["id"] == fixtures[0]["id"]
            driver_argv = [driver_bundle / "driver",
                           "--project", fixture["manifest"],
                           "--stdlib", driver_bundle / "stdlib",
                           "--resources", driver_bundle,
                           "--out", run_dir / "out", "--backend", backend,
                           "--opt", str(opt)]
            if trace_enabled:
                argv = [args.strace, "--seccomp-bpf", "-f", "-qq", "-s",
                        "16384", "-e", "trace=execve,execveat",
                        "-o", trace, *driver_argv]
            else:
                argv = driver_argv
            record, result = recorder.run(
                f"{fixture['id']}-driver-{backend}-O{opt}", argv, run_dir,
                env, args.command_timeout, expected_exit=0)
            stdout = json.loads(result["stdout"])
            if stdout.get("status") != "ok":
                raise RuntimeError(f"driver status is not ok: {stdout}")
            trace_info = None
            if trace_enabled:
                allowed = {str((driver_bundle / "driver").resolve())}
                for tool in TOOLS:
                    allowed.add(str((driver_bundle / "toolchain" / tool).resolve()))
                    allowed.add(str((toolchain / tool).resolve()))
                trace_info = trace_check(
                    trace, allowed, f"{fixture['id']}-{backend}-O{opt}",
                    harness.parse_trace)
            return {"record": record, "stdout": stdout, "dir": run_dir,
                    "trace": trace_info}

        def build_cli(fixture: dict, backend: str, opt: int) -> dict:
            run_dir = work / "runs" / fixture["id"] / backend / f"O{opt}"
            out = run_dir / "cli"
            trace = run_dir / "cli.strace"
            trace_enabled = fixture["id"] == fixtures[0]["id"]
            cli_argv = [candidate_bundle / "woozyc", "build",
                        "--project", fixture["manifest"], "--backend", backend,
                        "--out-dir", out, "--format", "json"]
            if backend == "native":
                cli_argv += ["--opt", str(opt)]
            if trace_enabled:
                argv = [args.strace, "--seccomp-bpf", "-f", "-qq", "-s",
                        "16384", "-e", "trace=execve,execveat",
                        "-o", trace, *cli_argv]
            else:
                argv = cli_argv
            record, result = recorder.run(
                f"{fixture['id']}-cli-{backend}-O{opt}", argv, run_dir,
                env, args.command_timeout, expected_exit=0)
            stdout = json.loads(result["stdout"])
            if stdout.get("status") != "ok":
                raise RuntimeError(f"CLI status is not ok: {stdout}")
            trace_info = None
            if trace_enabled:
                allowed = {str((candidate_bundle / "woozyc").resolve())}
                for tool in TOOLS:
                    allowed.add(str((candidate_bundle / "toolchain" / tool).resolve()))
                    allowed.add(str((toolchain / tool).resolve()))
                trace_info = trace_check(
                    trace, allowed, f"{fixture['id']}-cli-{backend}-O{opt}",
                    harness.parse_trace)
            return {"record": record, "stdout": stdout, "dir": out,
                    "trace": trace_info}

        for fixture in fixtures:
            for backend in fixture.get("backends", []):
                for opt in ((0, 2) if backend == "native" else (0,)):
                    run = {"id": fixture["id"], "backend": backend,
                           "opt": opt, "status": "failed", "values": [],
                           "source_sha256": fixture["source_sha256"],
                           "manifest_sha256": fixture["manifest_sha256"]}
                    report["runs"].append(run)
                    try:
                        driver_run = run_driver(fixture, backend, opt)
                        out = driver_run["dir"] / "out"
                        normal = out / "normal"
                        metadata = json.loads(
                            (out / "float-metadata.json").read_text())
                        run["driver"] = driver_run["record"]
                        run["driver_trace"] = driver_run["trace"]
                        run["metadata_sha256"] = sha(
                            out / "float-metadata.json")
                        run["normal_sha256"] = tree_hashes(normal)
                        check_metadata(metadata, fixture["probes"])
                        run["probe_count"] = len(fixture["probes"])
                        if backend == "native":
                            run["preopt_sha256"] = sha(out / "preopt.ll")
                            if sha(out / "preopt.ll") != metadata["preopt_llvm_sha256"]:
                                raise RuntimeError("pre-opt hash differs from metadata")
                            if sha(normal / "program.o") != metadata["normal_object_sha256"]:
                                raise RuntimeError("program.o hash differs from metadata")
                            if sha(normal / "program") != metadata["normal_program_sha256"]:
                                raise RuntimeError("program hash differs from metadata")
                            execute_program(
                                recorder, f"{fixture['id']}-program-O{opt}",
                                normal / "program", fixture["main_return"],
                                env, args.command_timeout)
                        cli_run = None
                        if candidate_bundle:
                            cli_run = build_cli(fixture, backend, opt)
                            run["cli"] = cli_run["record"]
                            run["cli_trace"] = cli_run["trace"]
                            run["cli_sha256"] = tree_hashes(cli_run["dir"])
                            if backend == "native":
                                run["object_identical_to_cli"] = (
                                    (normal / "program.o").read_bytes() ==
                                    (cli_run["dir"] / "program.o").read_bytes())
                                run["program_identical_to_cli"] = (
                                    (normal / "program").read_bytes() ==
                                    (cli_run["dir"] / "program").read_bytes())
                                if not run["object_identical_to_cli"]:
                                    raise RuntimeError("driver and CLI program.o differ")
                                if not run["program_identical_to_cli"]:
                                    raise RuntimeError("driver and CLI program differ")
                            else:
                                run["code_identical_to_cli"] = (
                                    (normal / "program.py").read_bytes() ==
                                    (cli_run["dir"] / "program.py").read_bytes())
                                run["runtime_identical_to_cli"] = (
                                    (normal / "langrt.py").read_bytes() ==
                                    (cli_run["dir"] / "langrt.py").read_bytes())
                                if not run["code_identical_to_cli"]:
                                    raise RuntimeError("driver and CLI program.py differ")
                                if not run["runtime_identical_to_cli"]:
                                    raise RuntimeError("driver and CLI langrt.py differ")
                                cli_code = (cli_run["dir"] / "program.py").read_text()
                                metadata_symbols = {
                                    item["generated"] for item in metadata["symbols"]
                                    if re.fullmatch(r"_f\d+", item["generated"])}
                                cli_defs = set(re.findall(
                                    r"^def (_f\d+)\(", cli_code, re.MULTILINE))
                                run["symbols_exact_in_cli_code"] = (
                                    metadata_symbols == cli_defs)
                                if not run["symbols_exact_in_cli_code"]:
                                    raise RuntimeError(
                                        "driver Output.symbols function set differs "
                                        "from CLI program.py definitions")
                        if backend == "native":
                            requests = driver_run["dir"] / "requests.tsv"
                            requests.write_text(request_text(fixture["probes"]))
                            run["requests_sha256"] = sha(requests)
                            observe_dir = driver_run["dir"] / "observe"
                            observer_record, observer_result = recorder.run(
                                f"{fixture['id']}-observer-O{opt}",
                                [observer, out / "preopt.ll",
                                 out / "float-metadata.json", requests,
                                 observe_dir], driver_run["dir"], env,
                                args.observer_timeout, expected_exit=0)
                            run["observer"] = observer_record
                            observe_ll = observe_dir / "observe.ll"
                            run["observer_metadata_sha256"] = sha(
                                observe_dir / "observer-metadata.json")
                            run["observe_ll_sha256"] = sha(observe_ll)
                            observe_bin = driver_run["dir"] / "observe-program"
                            recorder.run(
                                f"{fixture['id']}-observe-compile-O{opt}",
                                [toolchain / "clang", f"-O{opt}",
                                 "-ffp-contract=off", observe_ll,
                                 driver_bundle / "runtime/liblang_native_runtime.a",
                                 "-o", observe_bin], driver_run["dir"], env,
                                args.observer_timeout, expected_exit=0)
                            execute_record, execute_result = recorder.run(
                                f"{fixture['id']}-observe-run-O{opt}",
                                [observe_bin], driver_run["dir"], env,
                                args.command_timeout, expected_exit=0,
                                binary=True)
                            run["observe_execute"] = execute_record
                            observed = observation_values(
                                execute_result["stdout"], fixture["probes"])
                            if len(observed["values"]) != len(fixture["probes"]):
                                raise RuntimeError("Native observation count mismatch")
                            run["floating_environment"] = observed["environment"]
                            run["values"] = observed["values"]
                            if any(value["status"] != "passed"
                                   for value in run["values"]):
                                bad = next(value for value in run["values"]
                                           if value["status"] != "passed")
                                raise RuntimeError(f"observed value mismatch: {bad}")
                            run["observed_count"] = len(run["values"])
                        else:
                            probes_path = driver_run["dir"] / "probes.json"
                            probes_path.write_text(
                                json.dumps(fixture["probes"], indent=2) + "\n")
                            run["probes_sha256"] = sha(probes_path)
                            driver_obs = python_observe(
                                recorder, root,
                                f"{fixture['id']}-python-observe-driver",
                                args.python, normal, probes_path, env,
                                args.command_timeout)
                            cli_obs = None
                            if cli_run:
                                cli_obs_dir = driver_run["dir"] / "observe-cli"
                                cli_obs_dir.mkdir()
                                for name in ("program.py", "langrt.py"):
                                    shutil.copy2(cli_run["dir"] / name,
                                                 cli_obs_dir / name)
                                shutil.copy2(normal / "metadata.json",
                                             cli_obs_dir / "metadata.json")
                                cli_obs = python_observe(
                                    recorder, root,
                                    f"{fixture['id']}-python-observe-cli",
                                    args.python, cli_obs_dir, probes_path,
                                    env, args.command_timeout)
                            run["values"] = validate_python_observations(
                                fixture["probes"], driver_obs, cli_obs)
                            run["python_driver_observations"] = len(driver_obs)
                            if cli_obs is not None:
                                run["python_cli_observations"] = len(cli_obs)
                        run["status"] = "passed"
                    except Exception as error:
                        run["error"] = str(error)
                        report["failures"].append(
                            f"{fixture['id']}/{backend}/O{opt}: {error}")
                        if isinstance(error, CommandError):
                            run["failed_record"] = error.record

        # Required controls in the production run, not just a recording.
        control_runs = []
        sample = next((run for run in report["runs"]
                       if run["status"] == "passed" and run["backend"] == "native"
                       and run["values"]), None)
        if sample:
            expected = dict(sample["values"][0]["expected"])
            actual = sample["values"][0]["actual"]
            if expected["type"] == "bool":
                expected["value"] = not expected["value"]
            else:
                bits = expected.get("bits") or "00000000"
                width = len(bits)
                expected["bits"] = f"{int(bits, 16) ^ 1:0{width}x}"
            detected = not compatible(actual, expected)
            control_runs.append({"name": "wrong-expected-bits",
                                 "detected": detected})
            if not detected:
                raise RuntimeError("wrong expected bits were not rejected")
            run_dir = work / "controls/metadata-mismatch"
            run_dir.mkdir(parents=True)
            source_dir = work / "runs" / sample["id"] / "native" / \
                f"O{sample['opt']}"
            out = source_dir / "out"
            meta = json.loads((out / "float-metadata.json").read_text())
            fixture = next(f for f in fixtures + [panic]
                           if f["id"] == sample["id"])
            target = next(item for item in meta["functions"]
                          if item.get("status") == "covered" and
                          item["source_name"] in {p["function"] for p in fixture["probes"]})
            target["result"] = "f64" if target["result"] != "f64" else "f32"
            bad_meta = run_dir / "bad-metadata.json"
            bad_meta.write_text(json.dumps(meta))
            probe = next(p for p in fixture["probes"]
                         if p["function"] == target["source_name"])
            requests = run_dir / "requests.tsv"
            requests.write_text(request_text([probe]))
            record, result = recorder.run(
                "control-metadata-mismatch",
                [observer, out / "preopt.ll", bad_meta, requests,
                 run_dir / "observe"], run_dir, env,
                args.observer_timeout, expected_exit=1)
            control_runs.append({"name": "metadata-type-mismatch",
                                 "detected": result["exit"] == 1})
            bad_ir = run_dir / "bad-preopt.ll"
            text = (out / "preopt.ll").read_text()
            if "ret float" in text:
                text = text.replace("ret float", "ret i32", 1)
            elif "ret double" in text:
                text = text.replace("ret double", "ret i64", 1)
            else:
                text = text.replace("ret i32", "ret i64", 1)
            bad_ir.write_text(text)
            bad_meta = json.loads((out / "float-metadata.json").read_text())
            bad_meta["preopt_llvm_sha256"] = hashlib.sha256(
                text.encode()).hexdigest()
            bad_meta_path = run_dir / "bad-ir-metadata.json"
            bad_meta_path.write_text(json.dumps(bad_meta))
            recorder.run(
                "control-bad-ir",
                [observer, bad_ir, bad_meta_path, requests,
                 run_dir / "observe-bad"], run_dir, env,
                args.observer_timeout, expected_exit=1)
            control_runs.append({"name": "bad-ir-rejected", "detected": True})
            missing_requests = run_dir / "missing.tsv"
            missing_requests.write_text("definitely_missing\tf32\n")
            recorder.run(
                "control-missing-function",
                [observer, out / "preopt.ll", out / "float-metadata.json",
                 missing_requests, run_dir / "observe-missing"],
                run_dir, env, args.observer_timeout, expected_exit=1)
            control_runs.append({"name": "missing-function", "detected": True})
        panic_driver = run_driver(panic, "native", 0)
        panic_out = panic_driver["dir"] / "out"
        check_metadata(json.loads((panic_out / "float-metadata.json").read_text()),
                       panic["probes"])
        execute_program(recorder, "panic-control-program",
                        panic_out / "normal/program", 0, env,
                        args.command_timeout)
        panic_requests = panic_driver["dir"] / "panic-requests.tsv"
        panic_requests.write_text(request_text(panic["probes"]))
        observe_dir = panic_driver["dir"] / "panic-observe"
        recorder.run(
            "control-panic-observer",
            [observer, panic_out / "preopt.ll",
             panic_out / "float-metadata.json", panic_requests, observe_dir],
            panic_driver["dir"], env, args.observer_timeout, expected_exit=0)
        panic_bin = panic_driver["dir"] / "panic-observe-program"
        recorder.run(
            "control-panic-compile",
            [toolchain / "clang", "-O0", "-ffp-contract=off",
             observe_dir / "observe.ll",
             driver_bundle / "runtime/liblang_native_runtime.a",
             "-o", panic_bin], panic_driver["dir"], env,
            args.observer_timeout, expected_exit=0)
        record, result = recorder.run(
            "control-panic-execute", [panic_bin], panic_driver["dir"],
            env, args.command_timeout, expected_exit=101, binary=True)
        panic_ok = result["stderr"].startswith(b"LANG_NATIVE_PANIC:")
        control_runs.append({"name": "runtime-panic-101",
                             "detected": panic_ok,
                             "stderr": text_head(result["stderr"])})
        if not panic_ok:
            raise RuntimeError("panic control did not start with LANG_NATIVE_PANIC:")
        report["controls"] = control_runs
        required_control_names = {item["name"] for item in control_runs}
        required = {"wrong-expected-bits", "metadata-type-mismatch",
                    "bad-ir-rejected", "missing-function",
                    "runtime-panic-101"}
        if not required <= required_control_names or not all(
                item["detected"] for item in control_runs):
            raise RuntimeError(f"required controls failed: {control_runs}")

        after = {
            "driver_source_tree": wz_tree(root / "selfhost/src/wzcc"),
            "root_verify_ops": sha(
                root / "selfhost/src/wzcc/nativeverify/verify_ops.wz"),
            "stdlib_tree": tree_hashes(stdlib),
            "runtime_tree": tree_hashes(runtime_dir),
            "python_runtime_tree": tree_hashes(python_runtime),
            "fixtures": {fixture["id"]: sha(fixture["source"])
                         for fixture in fixtures + [panic]},
            "driver_binary": sha(driver_binary),
            "observer_binary": sha(observer),
            "candidate_binary": sha(candidate) if candidate else None,
            "tools": {name: sha(toolchain / name) for name in TOOLS},
        }
        after_checks = {
            "driver_source_tree": after["driver_source_tree"] ==
                report["driver"]["evidence"]["source_tree"],
            "root_verify_ops": after["root_verify_ops"] ==
                report["root_identity"]["verify_ops_sha256"],
            "stdlib_tree": after["stdlib_tree"] ==
                report["inputs"]["stdlib_tree"],
            "runtime_tree": after["runtime_tree"] ==
                report["inputs"]["runtime_tree"],
            "python_runtime_tree": after["python_runtime_tree"] ==
                report["inputs"]["python_runtime_tree"],
            "fixtures": after["fixtures"] == {
                key: value["source_sha256"]
                for key, value in report["inputs"]["fixtures"].items()},
            "driver_binary": after["driver_binary"] == report["driver"]["sha256"],
            "observer_binary": after["observer_binary"] == report["observer"]["sha256"],
            "candidate_binary": (after["candidate_binary"] ==
                                 report["candidate"]["candidate_sha256"])
                if candidate else None,
            "tools": after["tools"] == report["tool_sha256"],
        }
        report["hashes_after"] = after
        report["hash_stability_full"] = after_checks
        if any(value is False for value in after_checks.values()):
            raise RuntimeError(f"input/output hash changed: {after_checks}")

        # Final state only after all required controls and all runs pass.
        if report["failures"]:
            raise RuntimeError(f"{len(report['failures'])} runs failed")
        if not all(record["process_ok"] for record in recorder.records):
            raise RuntimeError("a command did not clean up")
        canonical = report["candidate"]["role"] == "canonical"
        report["status"] = "passed" if canonical else "component"
        if not canonical:
            report["not_run"].append(
                "canonical CLI proof/candidate identity did not match; "
                "provisional CLI is not relabelled")
        report["not_run"].extend([
            "Python supplemental 14 projects / 9 observations: not_run",
            "full 116-source canonical driver rebuild from frozen source: "
            "not_run (driver uses Root current 115)",
        ])
    except Exception as error:
        report["status"] = "failed"
        report["error"] = str(error)
        report["traceback"] = traceback.format_exc()
    finally:
        report["elapsed_s"] = round(time.monotonic() - started, 3)
        report["records"] = recorder.records
        report["process_cleanup_all"] = all(
            record["process_ok"] for record in recorder.records)
        report["hash_stability"] = {
            "driver_binary": (sha(Path(report["driver"]["binary"])) ==
                              report["driver"]["sha256"]) if "driver" in report else None,
            "observer_binary": (sha(Path(report["observer"]["binary"])) ==
                                report["observer"]["sha256"]) if "observer" in report else None,
            "candidate_binary": (sha(candidate) ==
                                 report["candidate"]["candidate_sha256"])
                if candidate and candidate.is_file() else None,
            "tools": {name: sha(toolchain / name) for name in TOOLS} ==
                report["tool_sha256"],
            "root_verify_ops": sha(root / "selfhost/src/wzcc/nativeverify/verify_ops.wz") ==
                report["root_identity"]["verify_ops_sha256"],
        }
        if any(value is False for value in report["hash_stability"].values()):
            report["status"] = "failed"
            report["error"] = "input/output hash changed"
        path = work / "results.json"
        path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
        print(json.dumps({"status": report["status"],
                          "error": report.get("error"),
                          "work_dir": str(work),
                          "elapsed_s": report["elapsed_s"],
                          "runs": len(report["runs"]),
                          "controls": len(report["controls"])}, indent=2))
        print(str(path))
    return 0 if report["status"] in ("passed", "component") else 1


if __name__ == "__main__":
    raise SystemExit(main())
