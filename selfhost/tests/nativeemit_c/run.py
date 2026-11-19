#!/usr/bin/env python3
"""C-layer (SH08c) checks for the Wz native text emitter.

Four independent stages, each recorded separately:

1. `syntax`  - the real stage0 frontend parses every production C file, the Wz
               driver, and the branch driver.
2. `oracle`  - every source fixture in ./corpus is compiled and executed by the
               existing C++ native backend (the differential oracle) at O0 and
               O2.  The pre-optimization IR is captured with
               `lang_native_helper --out-llvm-before` and must contain the
               C-owned helper symbols the fixture is written to exercise.
3. `wz`      - assemble a project from --source-root with every real app
               dependency (model/types/hir/nativeir/valueops/nativelayout) and
               the whole nativeemit package; B may be overlaid from the accepted
               --b-source.  The Wz driver prints the cache/Vec/Take module and
               it must verify with both opt pass sequences.
4. `branches` - emit real Wz modules for noncapturing/dynamic-capture Fn,
               bound ordinary/move-Self, Weak, dyn TraitObject/TraitCall and
               thread spawn.  It exercises both direct C/B/D/E helpers and the
               formal raw NativeIR dispatch (`emit_instruction`).  Every branch
               is emitted at Settings 0 and 2 (texts must match), verified at
               O0/O2, compiled and linked into two real executables, and both
               executables are run.

The default run requests every stage.  A requested stage that is blocked,
not_run or failed makes the overall result non-passing and non-zero; it is
never reported as a complete C pass.  `--only syntax` or `--only oracle` may
report that requested subset as passed, but still records `C_complete=false`
and names the stages that were not requested.

The merged public-Verifier/emit differential over the whole Native O0/O2 corpus
is a separate Root task after all groups land; raw fixture success here does
not make `C_complete` or a complete language backend true.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE.parent))
from wzcc_closure import complete_import_closure  # noqa: E402

C_FILES = ("c_values.wz", "c_weak.wz", "c_instructions.wz")
APP_PACKAGES = ("model", "types", "hir", "nativeir", "valueops", "nativelayout")
C_STAGES = ("syntax", "oracle", "wz", "branches")
BRANCHES = ("fn", "weak", "dyn", "thread", "ops")
BRANCH_NEEDLES = {
    "fn": ["lang.thunk.200", "lang.bound.201.", "lang.bound.202.",
           "lang.env.copy.", "lang.env.drop.", "fn.env",
           "llvm.memcpy.p0.p0.i64", "lang_gc_program_register_v1",
           "lang.gc.trace."],
    "weak": ["weak.fn.immortal", "weak.up.some", "weak.up.none",
             "lang_weak_downgrade_v1", "lang_weak_upgrade_v1"],
    "dyn": ["lang.trait.vtable.", "lang.trait.clone.", "lang.trait.drop.",
            "lang.trait.method.", "lang_trait_box_clone_v1",
            "lang_trait_box_release_v1"],
    "thread": ["lang.thread.entry", "lang.thread.copy", "lang.thread.drop",
               "lang_thread_spawn_v1"],
    "ops": ["lang.trait.vtable.", "lang.trait.method.", "lang_trait_box_clone_v1",
            "lang_weak_downgrade_v1", "weak.up.some", "weak.up.none",
            "lang.thread.entry", "lang.thread.copy", "lang.thread.drop",
            "lang_thread_spawn_v1"],
}

# Symbols the C layer must produce for each oracle fixture.
ORACLE_NEEDLES = {
    "values": ["lang.copy.", "lang.drop.", "lang.vec.retain", "lang.vec.release",
               "cmpxchg ptr"],
    "references": ["lang.thunk.", "lang.env.copy.", "lang.env.drop.",
                   "lang.bound.", "lang.copy.", "lang.drop."],
    "weak": ["lang_weak_downgrade_v1", "lang_weak_upgrade_v1", "weak.up.some",
             "weak.up.none", "lang.env.copy."],
    "traits": ["lang.trait.clone.", "lang.trait.drop.", "lang.trait.method.",
               "lang.trait.vtable.", "lang_trait_box_clone_v1",
               "lang_trait_box_release_v1"],
    "thread": ["lang.thread.entry", "lang.thread.copy", "lang.thread.drop",
               "lang_thread_spawn_v1"],
    "ops": ["lang.trait.vtable.", "lang.trait.method.", "lang_trait_box_clone_v1",
            "lang_weak_downgrade_v1", "weak.up.some", "weak.up.none",
            "lang.thread.entry", "lang.thread.copy", "lang.thread.drop",
            "lang_thread_spawn_v1"],
}

# Functions published by the other owners of this package.  A driver-stage
# check that fails only on these names is a missing dependency, not a C defect.
CROSS_GROUP = {
    # B: scalar.wz
    "type_of", "aggregate_declaration", "aggregate_fields", "field_index",
    "variant_index", "lower_type", "type_key", "signature",
    "find_hir_function", "source_function", "call_target", "value",
    "value_type", "constant_value", "storage", "stride", "element_storage",
    "guard", "checked_index", "vec_memory", "vec_field", "call", "runtime",
    # D: async_gc.wz
    "scan_c_resource_flags", "type_owns_c_resource", "promise_type",
    "coroutine_signature", "declare_async", "async_begin",
    "copy_async_ordinary_parameters", "build_async_frame_gc",
    "async_publish_and_suspend", "emit_async_terminator", "async_finish",
    "emit_async_main", "has_strong_edge", "gc_trace_value", "gc_register_vec",
    "gc_register_fields", "gc_poll", "gc_loop_safepoints",
    "gc_collect_loop_exit", "gc_enter_critical", "gc_leave_critical",
    "gc_enter_safe", "gc_leave_safe",
    # E: system.wz / c_ffi.wz
    "c_ref_type", "c_lower_type", "to_c_storage", "from_c_storage",
    "resource_operation", "callback_bridge", "callback_trampoline",
    "registration_dispatcher", "borrow_registration",
    "end_registration_borrow", "c_foreign_call", "emit_c_instruction",
    "system_call", "value_address", "text_binary", "stdlib_variant",
    "finalize_c_adapter", "registration_callback", "registration_close",
    # Root: text.wz helpers used by the driver
    "declare_runtime_types", "new_emitter", "default_options",
}
CROSS_PACKAGES = ("app/wzcc/valueops", "app/wzcc/nativelayout",
                  "app/wzcc/nativeverify", "app/wzcc/hirverify")
# Short names for a resolve-stage filesystem error about an absent package.
CROSS_PACKAGE_NAMES = tuple(Path(package).name for package in CROSS_PACKAGES)

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--woozyc", default=str(ROOT / "build/woozy-stdlib-native/woozyc"))
parser.add_argument("--helper",
                    default=str(ROOT / "build/woozy-stdlib-native/native/lang_native_helper"))
parser.add_argument("--toolchain-root", default=str(ROOT.parent / "toolchains"))
parser.add_argument("--source-root", default=str(ROOT / "selfhost/src/wzcc"))
parser.add_argument("--nativeemit-source", default=None,
                    help="override only the nativeemit package")
parser.add_argument("--work-dir", default=None)
parser.add_argument("--timeout", type=float, default=600)
parser.add_argument("--only", default=None,
                    help="comma-separated stages: syntax,oracle,wz,branches")
parser.add_argument("--b-source", default=None,
                    help="accepted B worktree or its selfhost/src/wzcc/nativeemit; "
                         "b_*.wz are copied from here instead of --source-root")
parser.add_argument("--runtime", default=None,
                    help="published native runtime archive (default: next to "
                         "--woozyc in native/liblang_native_runtime.a)")
args = parser.parse_args()
if args.timeout <= 0:
    parser.error("--timeout must be positive")
STAGES = set((args.only or ",".join(C_STAGES)).split(","))
KNOWN_STAGES = set(C_STAGES)
if not STAGES or not STAGES <= KNOWN_STAGES:
    parser.error("--only stages must be a non-empty subset of " +
                 ",".join(C_STAGES))

WORK_PARENT = Path(args.work_dir).expanduser().resolve() if args.work_dir else None
if WORK_PARENT:
    WORK_PARENT.mkdir(parents=True, exist_ok=True)
WORK = Path(tempfile.mkdtemp(prefix="woozy-nativeemit-c-", dir=WORK_PARENT))
SOURCE_ROOT = Path(args.source_root).expanduser().resolve()
NATIVEEMIT = (Path(args.nativeemit_source).expanduser().resolve()
              if args.nativeemit_source else SOURCE_ROOT / "nativeemit")
TOOLS = Path(args.toolchain_root).expanduser().resolve() / "llvm-22.1.8/bin"
STAGE0 = Path(args.woozyc).expanduser().resolve()
HELPER = Path(args.helper).expanduser().resolve()
B_SOURCE = None
if args.b_source:
    candidate = Path(args.b_source).expanduser().resolve()
    if (candidate / "selfhost/src/wzcc/nativeemit").is_dir():
        candidate = candidate / "selfhost/src/wzcc/nativeemit"
    B_SOURCE = candidate
RUNTIME = (Path(args.runtime).expanduser().resolve() if args.runtime
           else STAGE0.parent / "native/liblang_native_runtime.a")
ENV = dict(os.environ)
compat = str(Path(args.toolchain_root).expanduser().resolve()
             / "libxml2-legacy-2.13.9/usr/lib")
ENV["LD_LIBRARY_PATH"] = compat + (":" + ENV["LD_LIBRARY_PATH"]
                                   if ENV.get("LD_LIBRARY_PATH") else "")
if not ENV.get("TMPDIR"):
    ENV["TMPDIR"] = str(WORK)

records = []
not_run = []


class Blocked(RuntimeError):
    def __init__(self, message, reasons):
        super().__init__(message)
        self.reasons = list(reasons)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def command(label, argv, expected=0, allow_failure=False):
    result = subprocess.run([str(x) for x in argv], env=ENV, capture_output=True,
                            timeout=args.timeout)
    (WORK / (label + ".stdout")).write_bytes(result.stdout)
    (WORK / (label + ".stderr")).write_bytes(result.stderr)
    records.append({"label": label, "argv": [str(x) for x in argv],
                    "exit": result.returncode, "expected_exit": expected,
                    "stdout_bytes": len(result.stdout),
                    "stdout_sha256": hashlib.sha256(result.stdout).hexdigest(),
                    "stderr_bytes": len(result.stderr)})
    if result.returncode != expected and not allow_failure:
        raise RuntimeError(f"{label}: exit {result.returncode}: "
                           f"{result.stderr.decode(errors='replace')[:2000]}")
    return result


def stage_syntax():
    results = {}
    for name in C_FILES:
        path = NATIVEEMIT / name
        out = command("syntax-" + name[:-3],
                      [STAGE0, "check", "--syntax-only", "--format", "json", path])
        text = out.stdout.decode()
        if '"status":"ok"' not in text.replace(" ", ""):
            raise RuntimeError(f"syntax-{name}: {text[:2000]}")
        results[name] = {"sha256": sha(path), "status": "passed"}
    for name in ("main.wz", "driver.wz", "branch_driver.wz"):
        path = HERE / name
        out = command("syntax-" + name[:-3],
                      [STAGE0, "check", "--syntax-only", "--format", "json", path])
        if '"status":"ok"' not in out.stdout.decode().replace(" ", ""):
            raise RuntimeError(f"syntax-{name}: " + out.stdout.decode()[:2000])
        results[name] = {"sha256": sha(path), "status": "passed"}
    return results


def stage_oracle():
    results = {}
    corpus = HERE / "corpus"
    for case in sorted(p.name for p in corpus.iterdir() if p.is_dir()):
        manifest = corpus / case / "lang.toml"
        needles = ORACLE_NEEDLES.get(case)
        if needles is None:
            raise RuntimeError(f"oracle: no expectation recorded for {case}")
        case_result = {"manifest_sha256": sha(manifest),
                       "source_sha256": sha(corpus / case / "src/main.wz"),
                       "levels": {}}
        for level in (0, 2):
            out_dir = WORK / "oracle" / case / f"O{level}"
            out_dir.mkdir(parents=True, exist_ok=True)
            build = command(f"oracle-{case}-build-O{level}",
                            [STAGE0, "build", "--project", manifest,
                             "--backend", "native", "--opt", str(level),
                             "--out-dir", out_dir, "--format", "json"])
            if json.loads(build.stdout.decode()).get("status") != "ok":
                raise RuntimeError(f"oracle-{case}-O{level}: {build.stdout[:2000]}")
            program = out_dir / "program"
            execute = command(f"oracle-{case}-run-O{level}", [program])
            if execute.stdout or execute.stderr:
                raise RuntimeError(f"oracle-{case}-O{level} printed output")
            ir_dir = WORK / "oracle" / case / "ir"
            ir_dir.mkdir(parents=True, exist_ok=True)
            ir_path = ir_dir / f"O{level}.before.ll"
            command(f"oracle-{case}-helper-O{level}",
                    [HELPER, "--project", manifest, "--opt", str(level),
                     "--out-object", ir_dir / f"O{level}.o",
                     "--out-llvm-before", ir_path, "--format", "json"])
            text = ir_path.read_text(encoding="utf-8")
            missing = [needle for needle in needles if needle not in text]
            if missing:
                raise RuntimeError(f"oracle-{case}-O{level} missing {missing}")
            case_result["levels"][f"O{level}"] = {
                "exit": execute.returncode,
                "ir_bytes": len(text),
                "ir_sha256": hashlib.sha256(text.encode()).hexdigest(),
                "needles": needles,
            }
        results[case] = case_result
    return results


def scratch_project():
    """Copy every real app dependency and the C package into a project tree.

    The manifest lists each copied file with its source and SHA-256.  A missing
    real dependency is blocked; there is no stand-in fallback.  When --b-source
    is given, b_*.wz come from that accepted worktree and all other packages
    come from --source-root.
    """
    project = WORK / "wz"
    target = project / "src/wzcc"
    target.mkdir(parents=True, exist_ok=True)
    (project / "lang.toml").write_text(
        'schema_version=1\n[package]\nname="nativeemit_c_check"\n'
        'source_root="src"\nentry="app"\n[build]\nbackend="native"\n'
        "word_bits=64\n")
    copied = []
    missing = []
    for package in APP_PACKAGES:
        source_directory = SOURCE_ROOT / package
        if not source_directory.is_dir():
            missing.append(package)
            continue
        directory = target / package
        directory.mkdir(exist_ok=True)
        for source in sorted(source_directory.rglob("*.wz")):
            relative = source.relative_to(source_directory)
            destination = directory / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(source.read_bytes())
            copied.append({"package": package, "source": str(source),
                           "target": str(destination), "sha256": sha(source)})
    if not NATIVEEMIT.is_dir():
        missing.append("nativeemit")
    else:
        emitter_dir = target / "nativeemit"
        emitter_dir.mkdir(exist_ok=True)
        for source in sorted(NATIVEEMIT.glob("*.wz")):
            if B_SOURCE is not None and source.name.startswith("b_"):
                continue
            destination = emitter_dir / source.name
            destination.write_bytes(source.read_bytes())
            copied.append({"package": "nativeemit", "source": str(source),
                           "target": str(destination), "sha256": sha(source)})
        if B_SOURCE is not None:
            if not B_SOURCE.is_dir():
                missing.append("b_source")
            else:
                found = 0
                for source in sorted(B_SOURCE.glob("b_*.wz")):
                    destination = emitter_dir / source.name
                    destination.write_bytes(source.read_bytes())
                    copied.append({"package": "b_source",
                                   "source": str(source),
                                   "target": str(destination),
                                   "sha256": sha(source)})
                    found += 1
                if found == 0:
                    missing.append("b_source")
        # The C entry points stay package-private, so the drivers join the
        # package.  Record them with the dependency manifest as well.
        for name, target_name in (("driver.wz", "c_driver_test.wz"),
                                  ("branch_driver.wz", "c_branch_test.wz")):
            source = HERE / name
            destination = emitter_dir / target_name
            destination.write_bytes(source.read_bytes())
            copied.append({"package": "test", "source": str(source),
                           "target": str(destination), "sha256": sha(source)})
    (project / "src/main.wz").write_bytes((HERE / "main.wz").read_bytes())
    copied.append({"package": "test", "source": str(HERE / "main.wz"),
                   "target": str(project / "src/main.wz"),
                   "sha256": sha(HERE / "main.wz")})
    # SH31 added interface/hirverify/nativeverify dependencies to the real
    # nativeemit package.  Copy the import closure rather than a stale list.
    try:
        copied.extend(complete_import_closure(project / "src", SOURCE_ROOT))
    except FileNotFoundError as error:
        missing.append(str(error))
    if missing:
        raise Blocked("missing real app dependencies: " + ", ".join(missing),
                      [f"missing {SOURCE_ROOT / name}" for name in missing])
    return project, copied, missing


def classify_diagnostics(diagnostics):
    """Return (status, reasons) for a driver-stage project check."""
    blocked = []
    for diagnostic in diagnostics:
        code = diagnostic.get("code", "")
        message = diagnostic.get("message", "")
        if code == "E_INPUT_IO":
            match = re.search(r"\[([^\]]+)\]", message)
            if match and Path(match.group(1)).name in CROSS_PACKAGE_NAMES:
                blocked.append(f"{code}: {message}")
                continue
        if code == "E_IMPORT_NOT_FOUND":
            if any(package in message for package in CROSS_PACKAGES):
                blocked.append(f"{code}: {message}")
                continue
        if code == "E_NAME_NOT_FOUND":
            match = re.search(r"unknown name: (\w+)", message)
            if match and match.group(1) in CROSS_GROUP:
                blocked.append(f"{code}: {message}")
                continue
        if code in ("E_CAPABILITY_UNAVAILABLE", "E_EFFECT_UNHANDLED"):
            if any(name in message for name in CROSS_GROUP):
                blocked.append(f"{code}: {message}")
                continue
        return "failed", [f"{code}: {message}", *blocked]
    return "blocked", blocked


def run_driver(project, prefix, program_args=None):
    check = command(prefix + "-check",
                    [STAGE0, "check", "--project", project / "lang.toml",
                     "--backend", "native", "--format", "json"],
                    allow_failure=True)
    report = json.loads(check.stdout.decode())
    if report.get("status") != "ok" or check.returncode != 0:
        raise RuntimeError(prefix + "-check: " + json.dumps(
            report.get("diagnostics", [])[:12], indent=2))
    out_dir = WORK / (prefix + "-out")
    out_dir.mkdir(parents=True, exist_ok=True)
    build = command(prefix + "-build",
                    [STAGE0, "build", "--project", project / "lang.toml",
                     "--backend", "native", "--opt", "0",
                     "--out-dir", out_dir, "--format", "json"])
    if json.loads(build.stdout.decode()).get("status") != "ok":
        raise RuntimeError(prefix + "-build: " + build.stdout.decode()[:2000])
    execute = command(prefix + "-run",
                      [out_dir / "program", *(program_args or [])])
    module_text = execute.stdout.decode()
    if not module_text.strip():
        raise RuntimeError(prefix + "-run produced no LLVM text")
    module_path = WORK / (prefix + "-module.ll")
    module_path.write_text(module_text)
    for label, passes in ((prefix + "-opt-O0", "verify"),
                          (prefix + "-opt-O2", "verify,default<O2>,verify")):
        command(label, [TOOLS / "opt", "-S", "-passes=" + passes,
                        "-verify-each", "-mtriple=x86_64-unknown-linux-gnu",
                        "-mcpu=generic", module_path, "-o", WORK / (label + ".ll")])
    for needle in ("lang.vec.retain", "lang.env.copy.", "extern_weak global i8",
                   "store i1 false, ptr", "cmpxchg ptr"):
        if needle not in module_text:
            raise RuntimeError(prefix + "-module missing " + needle)
    return {"status": "passed", "llvm_bytes": len(module_text),
            "llvm_sha256": hashlib.sha256(module_text.encode()).hexdigest(),
            "module": str(module_path)}


def stage_branches():
    """Emit, verify, link and execute the four real C branch fixtures."""
    try:
        project, copied, missing = scratch_project()
    except Blocked as error:
        reasons = error.reasons
        not_run.append({"stage": "branches",
                        "reason": "real app dependencies are missing",
                        "reasons": reasons, "work_dir": str(WORK)})
        return {"status": "blocked", "reasons": reasons}
    check = command("branches-check",
                    [STAGE0, "check", "--project", project / "lang.toml",
                     "--backend", "native", "--format", "json"],
                    allow_failure=True)
    report = json.loads(check.stdout.decode())
    diagnostics = report.get("diagnostics", [])
    if report.get("status") != "ok" or check.returncode != 0:
        status, reasons = classify_diagnostics(diagnostics)
        detail = {"status": status, "diagnostics": diagnostics[:12],
                  "reasons": reasons[:12], "dependencies": copied}
        if status == "blocked":
            not_run.append({"stage": "branches",
                            "reason": "the branch project misses a shared "
                                      "package or published symbol",
                            "reasons": reasons[:12], "work_dir": str(WORK)})
            return detail
        raise RuntimeError("branches-check: " + json.dumps(
            diagnostics[:12], indent=2))
    out_dir = WORK / "branches-out"
    out_dir.mkdir(parents=True, exist_ok=True)
    reused = WORK / "wz-out" / "program"
    if reused.is_file():
        # The default run already built this exact Wz project in stage_wz.
        # Reuse that real binary; the build command and hashes stay in the wz
        # records above.
        program = reused
        build_note = "reused wz-out/program from the wz stage"
    else:
        build = command("branches-build",
                        [STAGE0, "build", "--project", project / "lang.toml",
                         "--backend", "native", "--opt", "0",
                         "--out-dir", out_dir, "--format", "json"])
        if json.loads(build.stdout.decode()).get("status") != "ok":
            raise RuntimeError("branches-build: " + build.stdout.decode()[:2000])
        program = out_dir / "program"
        build_note = "branches-build"
    if not RUNTIME.is_file():
        raise RuntimeError("native runtime archive is missing: " + str(RUNTIME))
    results = {}
    for branch in BRANCHES:
        text0 = command("branch-" + branch + "-driver-O0",
                        [program, branch, "0"]).stdout.decode()
        text2 = command("branch-" + branch + "-driver-O2",
                        [program, branch, "2"]).stdout.decode()
        if text0 != text2:
            raise RuntimeError(branch + ": O0/O2 driver text differs")
        if not text0.strip():
            raise RuntimeError(branch + ": empty LLVM module")
        missing_needles = [needle for needle in BRANCH_NEEDLES[branch]
                           if needle not in text0]
        if missing_needles:
            raise RuntimeError(branch + ": missing " + str(missing_needles))
        module_path = WORK / ("branch-" + branch + ".ll")
        module_path.write_text(text0)
        verified = {}
        for label, passes in (("O0", "verify"),
                              ("O2", "verify,default<O2>,verify")):
            output = WORK / ("branch-" + branch + "-" + label + ".ll")
            command("branch-" + branch + "-opt-" + label,
                    [TOOLS / "opt", "-S", "-passes=" + passes,
                     "-verify-each", "-mtriple=x86_64-unknown-linux-gnu",
                     "-mcpu=generic", module_path, "-o", output])
            verified[label] = output
        programs = {}
        for level in ("O0", "O2"):
            obj = WORK / ("branch-" + branch + "-" + level + ".o")
            command("branch-" + branch + "-" + level + "-llc",
                    [TOOLS / "llc", "-filetype=obj", "-O=" + level[1],
                     "-mtriple=x86_64-unknown-linux-gnu", "-mcpu=generic",
                     "-relocation-model=pic", "-code-model=small",
                     verified[level], "-o", obj])
            executable = WORK / ("branch-" + branch + "-" + level + ".program")
            command("branch-" + branch + "-" + level + "-link",
                    [TOOLS / "clang", "--target=x86_64-unknown-linux-gnu",
                     "-nostdlib", "-static",
                     "--ld-path=" + str(TOOLS / "ld.lld"), obj, RUNTIME,
                     "-o", executable])
            execution = command("branch-" + branch + "-" + level + "-run",
                                [executable])
            if execution.stdout or execution.stderr:
                raise RuntimeError(branch + " " + level +
                                   ": program printed output")
            programs[level] = {"program": str(executable),
                               "program_exit": execution.returncode}
        results[branch] = {
            "llvm_bytes": len(text0),
            "llvm_sha256": hashlib.sha256(text0.encode()).hexdigest(),
            "module": str(module_path),
            "opt": ["verify", "verify,default<O2>,verify"],
            "programs": programs,
            "needles": BRANCH_NEEDLES[branch],
        }
    return {"status": "passed", "dependencies": copied,
            "build": build_note, "branches": results}


def stage_wz():
    try:
        project, copied, missing = scratch_project()
    except Blocked as error:
        reasons = error.reasons
        not_run.append({"stage": "wz",
                        "reason": "real app dependencies are missing; no "
                                  "stand-in was substituted",
                        "reasons": reasons, "work_dir": str(WORK)})
        return {"status": "blocked", "reasons": reasons}
    check = command("wz-check",
                    [STAGE0, "check", "--project", project / "lang.toml",
                     "--backend", "native", "--format", "json"],
                    allow_failure=True)
    report = json.loads(check.stdout.decode())
    diagnostics = report.get("diagnostics", [])
    if report.get("status") != "ok" or check.returncode != 0:
        status, reasons = classify_diagnostics(diagnostics)
        detail = {"status": status, "diagnostics": diagnostics[:12],
                  "reasons": reasons[:12], "dependencies": copied}
        if status == "blocked":
            not_run.append({
                "stage": "wz",
                "reason": "the nativeemit package still misses files owned by "
                          "other workers or the shared valueops/nativelayout/"
                          "nativeverify packages; the merged check is Root's",
                "reasons": reasons[:12],
                "work_dir": str(WORK),
            })
            return detail
        raise RuntimeError("wz-check: " + json.dumps(diagnostics[:12], indent=2))
    try:
        result = run_driver(project, "wz")
    except RuntimeError as error:
        message = str(error)
        if ("expected metadata operand" in message
                and "lang.vec.storage" in message):
            reason = ("known B b_define_vec_tbaa metadata text is not accepted "
                      "by opt; C's Vec module is emitted but cannot be verified "
                      "until the accepted B fix lands")
            not_run.append({"stage": "wz", "reason": reason,
                            "reasons": [message[:2000]],
                            "dependencies": copied, "work_dir": str(WORK)})
            return {"status": "blocked", "reasons": [reason],
                    "dependencies": copied,
                    "blocked_by": "b_define_vec_tbaa"}
        raise
    result["dependencies"] = copied
    return result


def main():
    inputs = []
    for path in [*(NATIVEEMIT / name for name in C_FILES), HERE / "main.wz",
                 HERE / "driver.wz", HERE / "branch_driver.wz",
                 *(HERE / "corpus").glob("*/src/main.wz")]:
        inputs.append({"source": str(path), "sha256": sha(path)})
    (WORK / "inputs.json").write_text(json.dumps(inputs, indent=2))
    requested = [stage for stage in C_STAGES if stage in STAGES]
    summary = {"work_dir": str(WORK), "inputs": inputs,
               "requested_stages": requested,
               "source_root": str(SOURCE_ROOT),
               "nativeemit_source": str(NATIVEEMIT),
               "b_source": str(B_SOURCE) if B_SOURCE else None,
               "C_complete": False}
    stage_status = {}
    stages = {
        "syntax": stage_syntax,
        "oracle": stage_oracle,
        "wz": stage_wz,
        "branches": stage_branches,
    }
    try:
        for stage in requested:
            result = stages[stage]()
            summary[stage] = result
            stage_status[stage] = result.get("status", "passed")
    except Exception as error:  # noqa: BLE001 - report the failing stage verbatim
        stage_status[stage] = "failed"
        summary["status"] = "failed"
        summary["stage_status"] = stage_status
        summary["error"] = str(error)
        summary["not_run"] = not_run
        summary["records"] = records
        (WORK / "results.json").write_text(json.dumps(summary, indent=2))
        print(json.dumps({k: v for k, v in summary.items()
                          if k not in ("records", "inputs")}, indent=2))
        return 1
    for stage in C_STAGES:
        if stage not in STAGES:
            not_run.append({"stage": stage,
                            "reason": "not requested by --only " +
                                      (args.only or ""),
                            "work_dir": str(WORK)})
    summary["stage_status"] = stage_status
    summary["not_run"] = not_run
    summary["records"] = records
    incomplete = [stage for stage, status in stage_status.items()
                  if status != "passed"]
    if incomplete:
        summary["status"] = ("blocked" if "wz" in stage_status
                             and stage_status["wz"] != "passed"
                             else "partial")
        (WORK / "results.json").write_text(json.dumps(summary, indent=2))
        print(json.dumps({k: v for k, v in summary.items()
                          if k not in ("records", "inputs")}, indent=2))
        return 1
    full = requested == list(C_STAGES)
    summary["status"] = "passed" if full else "partial"
    summary["C_complete"] = False
    summary["C_test_stages_passed"] = full
    summary["language_backend_complete"] = False
    if full:
        summary["scope"] = ("raw C helper and raw NativeIR op tests passed; "
                            "public NativeVerifier->emit and the full corpus "
                            "are not run")
    else:
        summary["scope"] = ("only the requested stages passed; this is not a "
                            "complete C check")
    (WORK / "results.json").write_text(json.dumps(summary, indent=2))
    print(json.dumps({k: v for k, v in summary.items()
                      if k not in ("records", "inputs")}, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
