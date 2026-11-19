#!/usr/bin/env python3
"""Run the pure-Wz SH05 manifest loader tests from a copied project.

The stage0 compiler only builds the copied Wz project; no C++ compiler or
toml++ parser is rebuilt. Each opt level runs the same real-filesystem checks:
temporary manifests, a manifest/source-root symlink, dependency config cases,
whole-original-byte diagnostics, integer limits, and every repository lang.toml
as an existing legal dependency manifest. The only intentionally unchecked
diagnostic text is the TOML syntax wording: the Woozy reader reports its own
message instead of toml++'s description. The code, Environment failure and
whole-source span are compared exactly.
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import importlib.util
import shutil
import subprocess
import sys
import tomllib

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
DEFAULT_STAGE0 = ROOT / "build" / "woozy-stdlib-native" / "woozyc"
DEFAULT_WORK = ROOT / "build" / "manifest-tests"
DEFAULT_TIMEOUT = 180.0
MODULES = ("model", "source", "input", "paths", "toml", "manifest")
COUNT_RE = re.compile(r"MANIFEST_COUNTS passed=(\d+) failed=(\d+)")
# Fixed test sub-roots.  Never enumerate the whole repository: untracked build
# output and change-only snapshots are not independent projects.
CORPUS_ROOTS = ("examples", "selfhost", "tests")
PARENTS = {
    "selfhost/tests/cffi_forged/lang.toml":
        "selfhost/tests/cffi_forged/run.py",
    "selfhost/tests/foreign_signature/lang.toml":
        "selfhost/tests/foreign_signature/run.py",
    "selfhost/tests/hirdump/lang.toml":
        "selfhost/tests/hirdump/run.py",
    "selfhost/tests/hirverify/cache/needs_drop/lang.toml":
        "selfhost/tests/hirverify/cache/needs_drop/run.py",
    "selfhost/tests/nativelower/helpers/lang.toml":
        "selfhost/tests/nativelower/run.py",
    "selfhost/tests/nativeobs/lang.toml":
        "selfhost/tests/nativeobs/run.py",
    "selfhost/tests/nativeverify/callidentity/lang.toml":
        "selfhost/tests/nativeverify/callidentity/run.py",
    "selfhost/tests/nativeverify/driver/lang.toml":
        "selfhost/tests/nativeverify/driver/run.py",
    "selfhost/tests/nativeverify/layouttest/lang.toml":
        "selfhost/tests/nativeverify/layouttest/run.py",
    "selfhost/tests/nativeverify/obligationstest/lang.toml":
        "selfhost/tests/nativeverify/obligationstest/run.py",
    "selfhost/tests/phase-timing/lang.toml":
        "selfhost/tests/phase-timing/run.py",
    "selfhost/tests/pythonemit/lang.toml":
        "selfhost/tests/pythonemit/run.py",
    "tests/incremental/fixtures/06-package-set-errors/variants/"
    "remove-package-stale/new/lang.toml":
        "tests/incremental/fixtures/run_reference.py",
    "tests/incremental/fixtures/06-package-set-errors/variants/"
    "remove-package-stale/old/lang.toml":
        "tests/incremental/fixtures/run_reference.py",
}


def _sha(path: pathlib.Path) -> str:
    import hashlib
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _load(root: pathlib.Path, relative: str, name: str):
    path = root / relative
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load parent runner: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.path.insert(0, str(root / "selfhost"))
    sys.path.insert(0, str(path.parent))
    spec.loader.exec_module(module)
    return module


def _gate(module, source_root: pathlib.Path, expected_name: str) -> dict:
    expected = getattr(module, expected_name, None)
    if not expected:
        return {"status": "not_applicable"}
    actual = module.json_hash(module.tree_map(source_root, ".wz"))
    return {"status": "satisfied" if actual == expected else
            "not_satisfied_current_source",
            "source": str(source_root), "actual": actual, "expected": expected}


def _source_root(manifest: pathlib.Path) -> pathlib.Path:
    document = tomllib.loads(manifest.read_text(encoding="utf-8"))
    return manifest.parent / document["package"]["source_root"]


def _stage_one(root: pathlib.Path, relative: str, work: pathlib.Path) -> dict:
    runner = PARENTS[relative]
    module = _load(root, runner, "manifest_parent_" + relative.replace("/", "_"))
    source_gate = {"status": "not_applicable"}
    method = ""
    if runner.endswith("cffi_forged/run.py"):
        source_root = module.DEFAULT_SOURCE_ROOT
        component = work / "component"
        (component / "src").mkdir(parents=True)
        shutil.copytree(source_root / "wzcc", component / "src/wzcc")
        shutil.copy2(module.HERE / "main.wz", component / "src/main.wz")
        shutil.copy2(module.HERE / "lang.toml", component / "lang.toml")
        source_gate = _gate(module, source_root, "EXPECTED_SOURCE116")
        method = "inline copy lines from cffi_forged/run.py main()"
        project = component
    elif runner.endswith("foreign_signature/run.py"):
        project = work / "project"
        module.copy_modules(project)
        method = "foreign_signature.copy_modules"
    elif runner.endswith("hirdump/run.py"):
        project = module.copy_project(work)
        method = "hirdump.copy_project"
    elif runner.endswith("hirverify/cache/needs_drop/run.py"):
        project = module.stage_project(work)
        method = "needs_drop.stage_project"
    elif runner.endswith("nativelower/run.py"):
        layout = (root / "selfhost/src/wzcc/nativelayout/nativelayout.wz")
        valueops = (root / "selfhost/src/wzcc/valueops/valueops.wz")
        project = module.stage_helper_project(work, "helper", layout, valueops)
        method = "nativelower.stage_helper_project"
    elif runner.endswith("nativeobs/run.py"):
        source_root = module.DEFAULT_SOURCE_ROOT
        source_gate = _gate(module, source_root, "SOURCE116_MAP_SHA")
        if source_gate.get("status") != "satisfied":
            raise RuntimeError(
                f"nativeobs parent source gate failed: {source_gate}")
        component = work / "component"
        (component / "src").mkdir(parents=True)
        shutil.copytree(source_root / "wzcc", component / "src/wzcc")
        shutil.copy2(module.HERE / "main.wz", component / "src/main.wz")
        shutil.copy2(module.HERE / "lang.toml", component / "lang.toml")
        method = "inline copy lines from nativeobs/run.py main()"
        project = component
    elif runner.endswith("nativeverify/callidentity/run.py"):
        project = module.stage_project(work, root / "selfhost/src")
        method = "callidentity.stage_project"
    elif runner.endswith("nativeverify/driver/run.py"):
        project = module.stage_project(work)
        method = "driver.stage_project"
    elif runner.endswith("nativeverify/layouttest/run.py"):
        project = module.stage_project(work)
        method = "layouttest.stage_project"
    elif runner.endswith("nativeverify/obligationstest/run.py"):
        project = work / "project"
        (project / "src/wzcc").mkdir(parents=True)
        sources = {}
        for package in module.PACKAGES:
            path = module.find_package(package, [])
            if path is None:
                raise RuntimeError(f"obligation parent package missing: {package}")
            sources[package] = path
            shutil.copytree(path, project / "src/wzcc" / package)
            if package == "nativeverify":
                shutil.copy2(module.HERE / "trace.wz",
                             project / "src/wzcc/nativeverify/trace.wz")
                if "hirverify" not in sources:
                    (project / "src/wzcc/nativeverify/nativeverify.wz").unlink()
        shutil.copy2(module.HERE / "main.wz", project / "src/main.wz")
        shutil.copy2(module.HERE / "lang.toml", project / "lang.toml")
        method = "inline package copy from obligationstest/run.py main()"
    elif runner.endswith("phase-timing/run.py"):
        source_root = module.DEFAULT_SOURCE_ROOT
        component = work / "component"
        (component / "src").mkdir(parents=True)
        shutil.copytree(source_root / "wzcc", component / "src/wzcc")
        shutil.copy2(module.HERE / "main.wz", component / "src/main.wz")
        shutil.copy2(module.HERE / "lang.toml", component / "lang.toml")
        source_gate = _gate(module, source_root, "EXPECTED_SOURCE116")
        method = "inline copy lines from phase-timing/run.py main()"
        project = component
    elif runner.endswith("pythonemit/run.py"):
        project = module.copy_pure_project(work)
        method = "pythonemit.copy_pure_project"
    elif runner.endswith("run_reference.py"):
        fixture = root / "tests/incremental/fixtures/06-package-set-errors"
        mutation_path = (fixture / "variants/remove-package-stale/mutation.json")
        mutation = json.loads(mutation_path.read_text(encoding="utf-8"))
        destination = work / ("old_project" if relative.endswith("old/lang.toml")
                              else "new_project")
        applied, _ = module.materialize(
            fixture, destination,
            None if relative.endswith("old/lang.toml") else mutation)
        source_gate = {"status": "materialized", "applied": applied,
                       "expect": mutation.get("expect")}
        method = "incremental.run_reference.materialize"
        project = destination
    else:
        raise RuntimeError(f"unhandled parent runner: {runner}")

    manifest = project / "lang.toml"
    if not manifest.is_file():
        raise RuntimeError(f"parent staging did not produce {manifest}")
    return {
        "original": relative,
        "parent_runner": runner,
        "project": str(project),
        "manifest": str(manifest),
        "manifest_sha256": _sha(manifest),
        "staging_method": method,
        "source_root": str(_source_root(manifest)),
        "source_root_exists": _source_root(manifest).is_dir(),
        "source_gate": source_gate,
    }


def stage_parent_projects(root: pathlib.Path, work: pathlib.Path) -> list[dict]:
    records = []
    for relative in sorted(PARENTS):
        slug = relative.replace("/", "__").removesuffix("__lang.toml")
        project_work = work / slug
        project_work.mkdir(parents=True, exist_ok=True)
        record = _stage_one(root, relative, project_work)
        original = root / relative
        if record["manifest_sha256"] != _sha(original):
            raise RuntimeError(
                f"staged manifest differs from requested fragment: {relative}")
        if not record["source_root_exists"]:
            raise RuntimeError(
                f"staged parent has no source root: {record['source_root']}")
        source_root = pathlib.Path(record["source_root"])
        if not any(source_root.rglob("*.wz")):
            raise RuntimeError(
                f"staged parent source root has no Wz files: {source_root}")
        records.append(record)
    return records


# These manifests are staged by their parent runners: they deliberately have
# no source_root directory in the repository.  They are tested below as
# parent-project fixtures (the parent supplies src), never as complete
# projects in the legal corpus.  The tuple is intentionally explicit so a new
# unrelated directory cannot be silently excluded.
FRAGMENTS = PARENTS
# The source harness is already documented as staged by its own runner.
STAGED_MANIFESTS = {
    "selfhost/tests/source/lang.toml":
        "source/run.py creates the project src tree before loading this manifest",
}


def source_root_dir(manifest: pathlib.Path) -> pathlib.Path:
    document = tomllib.loads(manifest.read_text(encoding="utf-8"))
    return manifest.parent / document["package"]["source_root"]


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def environment() -> dict[str, str]:
    env = dict(os.environ)
    parts = [
        str(ROOT.parent / "toolchains" / "llvm-22.1.8" / "lib"),
        str(ROOT.parent / "toolchains" / "libxml2-legacy-2.13.9" / "usr" / "lib"),
    ]
    previous = env.get("LD_LIBRARY_PATH")
    if previous:
        parts.append(previous)
    env["LD_LIBRARY_PATH"] = ":".join(parts)
    return env


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Check, build and execute the Wz manifest loader tests.")
    parser.add_argument(
        "--woozyc", default=str(DEFAULT_STAGE0),
        help=f"absolute stage0 compiler (default: {DEFAULT_STAGE0})")
    parser.add_argument(
        "--work-dir", default=str(DEFAULT_WORK),
        help=f"output/copy directory (default: {DEFAULT_WORK})")
    parser.add_argument(
        "--timeout", type=float, default=DEFAULT_TIMEOUT,
        help=f"timeout in seconds for each command (default: {DEFAULT_TIMEOUT:g})")
    return parser.parse_args(argv)


def run(command: list[str], label: str, work: pathlib.Path,
        timeout: float) -> subprocess.CompletedProcess[str]:
    print("+", " ".join(command), flush=True)
    try:
        return subprocess.run(
            command, cwd=work, env=environment(), text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
            timeout=timeout)
    except subprocess.TimeoutExpired:
        fail(f"{label}: timed out after {timeout:g}s")
    except OSError as error:
        fail(f"{label}: {error}")


def copy_project(work: pathlib.Path) -> pathlib.Path:
    project = work / "project"
    if project.exists():
        shutil.rmtree(project)
    (project / "src").mkdir(parents=True)
    for module in MODULES:
        shutil.copytree(ROOT / "selfhost" / "src" / "wzcc" / module,
                        project / "src" / "wzcc" / module)
    shutil.copy2(HERE / "src" / "main.wz", project / "src" / "main.wz")
    shutil.copy2(HERE / "lang.toml", project / "lang.toml")
    return project


def corpus_list(work: pathlib.Path) -> pathlib.Path:
    complete = []
    seen = set()
    for root_name in CORPUS_ROOTS:
        root = ROOT / root_name
        if not root.is_dir():
            fail(f"fixed corpus root is missing: {root}")
        for path in sorted(root.rglob("lang.toml")):
            relative = path.relative_to(ROOT).as_posix()
            if "build" in pathlib.PurePosixPath(relative).parts:
                continue
            if relative in seen:
                fail(f"duplicate corpus manifest: {relative}")
            seen.add(relative)
            if relative in STAGED_MANIFESTS:
                continue
            if relative in FRAGMENTS:
                continue
            source_root = source_root_dir(path)
            if not source_root.is_dir():
                fail(f"unclassified manifest without source root: {relative} "
                     f"(expected {source_root})")
            complete.append(path.resolve())

    # Stage each fragment through its real parent runner (copy/symlink or
    # materialize/mutation steps), then load the resulting project manifest.
    # No empty-src synthetic project is used here.
    parents_root = work / "parent-projects"
    parent_records = stage_parent_projects(ROOT, parents_root)
    if len(parent_records) != len(PARENTS):
        fail(f"staged {len(parent_records)} parent projects, "
             f"want {len(PARENTS)}")
    parent_json = work / "parent-projects.json"
    parent_json.write_text(
        json.dumps(parent_records, indent=2) + "\n", encoding="utf-8")
    parent_paths = [pathlib.Path(record["manifest"])
                    for record in parent_records]

    complete.sort()
    manifests = [str(path) for path in complete]
    manifests.extend(str(path) for path in parent_paths)
    manifests.sort()
    listing = work / "manifest-corpus.txt"
    listing.write_text("\n".join(manifests) + "\n", encoding="utf-8")
    print(f"corpus: {len(complete)} complete projects + "
          f"{len(parent_records)} real parent projects")
    print(f"staged manifests excluded: {len(STAGED_MANIFESTS)}; "
          f"parent provenance: {parent_json}")
    return listing


def expect_ok(result: subprocess.CompletedProcess[str], label: str) -> None:
    if result.returncode != 0:
        fail(f"{label}: exit {result.returncode}\nstdout:\n{result.stdout}"
             f"stderr:\n{result.stderr}")
    print(f"PASS: {label}")


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    stage0 = pathlib.Path(args.woozyc).expanduser()
    if not stage0.is_absolute():
        fail(f"--woozyc must be absolute: {stage0}")
    if not stage0.is_file():
        fail(f"stage0 compiler missing: {stage0}")
    if args.timeout <= 0:
        fail(f"--timeout must be positive, got {args.timeout:g}")

    work = pathlib.Path(args.work_dir).expanduser().resolve()
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    print(f"stage0: {stage0}")
    print(f"work-dir: {work}")
    project = copy_project(work)
    manifest = str(project / "lang.toml")
    listing = corpus_list(work)

    checked = run([str(stage0), "check", "--project", manifest,
                   "--backend", "native"], "check", work, args.timeout)
    expect_ok(checked, "stage0 check")

    for opt in ("0", "2"):
        out = work / f"opt{opt}"
        built = run([str(stage0), "build", "--project", manifest,
                     "--backend", "native", "--opt", opt,
                     "--out-dir", str(out)], f"build O{opt}", work,
                    args.timeout)
        expect_ok(built, f"stage0 build O{opt}")
        program = out / "program"
        if not program.is_file():
            fail(f"O{opt}: missing {program}")
        case_dir = work / f"cases-opt{opt}"
        case_dir.mkdir()
        values = run([str(program), str(case_dir), str(listing)],
                     f"manifest tests O{opt}", work, args.timeout)
        expect_ok(values, f"manifest tests O{opt}")
        match = COUNT_RE.search(values.stdout)
        if match is None:
            fail(f"O{opt}: missing MANIFEST_COUNTS line")
        passed, failed = (int(value) for value in match.groups())
        if failed != 0 or passed == 0:
            fail(f"O{opt}: passed={passed} failed={failed}")
        print(f"O{opt} counts: passed={passed} failed={failed}")
    print("PASS: manifest loader tests (check, O0, O2, real temp dirs, "
          "symlinks, dependency configs, integer limits, full legal corpus)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
