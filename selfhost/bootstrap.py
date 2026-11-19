#!/usr/bin/env python3
"""Build the same frozen Woozy compiler sources through successive generations."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import resource
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SELFHOST = Path(__file__).resolve().parent
if str(SELFHOST) not in sys.path:
    sys.path.insert(0, str(SELFHOST))
from corpus import (  # noqa: E402
    DEFAULT_LIBXML, DEFAULT_LLVM, TOOL_NAMES,
    _run_process_impl, _timeout_number, parse_trace,
)

STAGE0_MACROS = (
    "LANG_STD_SOURCE_ROOT",
    "LANG_NATIVE_HELPER",
    "LANG_NATIVE_RUNTIME",
    "LANG_HOSTED_RUNTIME",
    "LANG_NATIVE_CLANG",
    "LANG_NATIVE_LLD",
)
RUNTIME_ARCHIVES = ("liblang_native_runtime.a", "liblang_hosted_runtime.a")
PYTHON_RUNTIME = ("langrt.py", "unicode18.py")
STAGE0_FILE_MACROS = (
    "LANG_NATIVE_HELPER",
    "LANG_NATIVE_RUNTIME",
    "LANG_HOSTED_RUNTIME",
    "LANG_NATIVE_CLANG",
    "LANG_NATIVE_LLD",
)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def copy_sources(source, destination):
    destination.mkdir(parents=True)
    records = {}
    for path in sorted(source.rglob("*.wz")):
        relative = path.relative_to(source)
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, target)
        records[str(relative)] = digest(target)
    if not records:
        raise RuntimeError(f"no Woozy sources in {source}")
    current = {str(p.relative_to(source)): digest(p) for p in sorted(source.rglob("*.wz"))}
    if current != records:
        raise RuntimeError(f"sources changed while copying {source}")
    return records


def trace_executables(path, allowed):
    observed = set(parse_trace(path))
    unexpected = observed - set(allowed)
    if unexpected:
        raise RuntimeError(f"unexpected compiler subprocesses: {sorted(unexpected)}")
    return sorted(observed)


def _tree_hashes(root):
    root = Path(root)
    if not root.is_dir():
        raise RuntimeError(f"frozen input directory is missing: {root}")
    return {str(path.relative_to(root)): digest(path)
            for path in sorted(root.rglob("*.wz")) if path.is_file()}


def _all_tree_hashes(root):
    root = Path(root)
    if not root.is_dir():
        raise RuntimeError(f"frozen input directory is missing: {root}")
    return {str(path.relative_to(root)): digest(path)
            for path in sorted(root.rglob("*")) if path.is_file()}


def _ninja_defines(path, marker):
    values = {}
    current = ""
    for line in path.read_text(encoding="utf-8", errors="surrogateescape").splitlines():
        if line.startswith("build "):
            current = line
        elif line.lstrip().startswith("DEFINES = "):
            if marker not in current:
                continue
            for match in re.finditer(
                    r'-D([A-Z0-9_]+)=\\"([^"\\]*)\\"', line):
                values.setdefault(match.group(1), set()).add(match.group(2))
    return values


def _single_macro(values, name, source):
    found = values.get(name)
    if not found or len(found) != 1:
        raise RuntimeError(
            f"cannot confirm {name} from {source}: {sorted(found or [])}")
    return next(iter(found))


def _required_file(path, label):
    path = Path(path)
    if not path.is_file():
        raise RuntimeError(f"{label} is missing: {path}")
    return path.resolve(strict=True)


def _stage0_resource_paths(seed, env):
    try:
        with open(seed, "rb") as source:
            if source.read(4) != b"\x7fELF":
                return None
    except OSError:
        return None
    try:
        completed = subprocess.run(
            [str(seed), "--__resource-paths"], env=env,
            capture_output=True, text=True, timeout=60, check=False)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise RuntimeError(
            f"cannot inspect stage0 resource paths: {error}") from error
    output = (completed.stdout + completed.stderr).strip()
    # Old C++ seeds do not implement the private query. Keep their strict
    # embedded-macro proof; a new seed that fails to resolve resources is an
    # error rather than a reason to fall back to stale build configuration.
    if output == (
            "E_DRIVER_ARGUMENT: expected --help, --version, check, build, "
            "lsp or capabilities"):
        return None
    if completed.returncode == 0:
        try:
            value = json.loads(completed.stdout)
        except json.JSONDecodeError as error:
            raise RuntimeError(
                f"stage0 resource paths are not JSON: {completed.stdout!r}"
            ) from error
        if not isinstance(value, dict):
            raise RuntimeError("stage0 resource paths are not an object")
        return value
    raise RuntimeError(f"stage0 resource paths failed: {output}")


def _stage0_route_paths(actual):
    try:
        toolchain = actual["toolchain"]
        package_tools = {
            name: toolchain[name] for name in ("opt", "llc")
            if name in toolchain
        }
        if "ninja" in actual:
            package_tools["ninja"] = actual["ninja"]
        return {
            "stdlib": actual["stdlib"],
            "helper": actual["helper"],
            "runtime": {
                "liblang_native_runtime.a": actual["runtime"]["native"],
                "liblang_hosted_runtime.a": actual["runtime"]["hosted"],
            },
            "tools": {
                "clang": toolchain["clang"],
                "ld.lld": toolchain["ld.lld"],
            },
            "package_tools": package_tools,
        }
    except (KeyError, TypeError) as error:
        raise RuntimeError(
            f"stage0 resource paths are incomplete: {actual}") from error


def _stage0_stored_route(stage0):
    try:
        return {
            "stdlib": stage0["stdlib"].get(
                "reported_path", stage0["stdlib"]["path"]),
            "helper": stage0["helper"].get(
                "reported_path", stage0["helper"]["path"]),
            "runtime": {
                name: stage0["runtime"][name].get(
                    "reported_path", stage0["runtime"][name]["path"])
                for name in RUNTIME_ARCHIVES
            },
            "tools": {
                name: stage0["tools"][name].get(
                    "reported_path", stage0["tools"][name]["path"])
                for name in ("clang", "ld.lld")
            },
            "package_tools": {
                name: item.get("reported_path", item["path"])
                for name, item in stage0.get("package_tools", {}).items()
            },
        }
    except KeyError as error:
        raise RuntimeError(
            f"stage0 stored resource paths are incomplete: {stage0}") from error


def inspect_stage0(seed, toolchain, package_objects=False, env=None):
    """Read the CMake build configuration that fixed the C++ seed's inputs."""
    seed = _required_file(seed, "stage0 seed")
    build_dir = seed.parent
    build_ninja = _required_file(build_dir / "build.ninja", "stage0 build.ninja")
    cmake_cache = _required_file(build_dir / "CMakeCache.txt", "stage0 CMakeCache.txt")
    cache_text = cmake_cache.read_text(encoding="utf-8", errors="surrogateescape")
    if not re.search(r"^LANG_ENABLE_LLVM:BOOL=ON$", cache_text, re.M):
        raise RuntimeError(f"stage0 was not built with LANG_ENABLE_LLVM=ON: {cmake_cache}")
    home = re.search(r"^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$", cache_text, re.M)
    if not home:
        raise RuntimeError(f"cannot confirm CMAKE_HOME_DIRECTORY in {cmake_cache}")

    all_defines = _ninja_defines(build_ninja, "")
    seed_defines = _ninja_defines(
        build_ninja, "CMakeFiles/woozyc.dir/src/driver/main.cpp.o")
    helper_defines = _ninja_defines(
        build_ninja, "lang_native_helper.dir/helper.cpp.o")
    if not seed_defines or not helper_defines:
        raise RuntimeError(f"cannot locate seed/helper compile rules in {build_ninja}")
    macros = {
        "LANG_STD_SOURCE_ROOT": _single_macro(
            all_defines, "LANG_STD_SOURCE_ROOT", build_ninja),
        "LANG_NATIVE_HELPER": _single_macro(
            seed_defines, "LANG_NATIVE_HELPER", build_ninja),
        "LANG_NATIVE_RUNTIME": _single_macro(
            helper_defines, "LANG_NATIVE_RUNTIME", build_ninja),
        "LANG_HOSTED_RUNTIME": _single_macro(
            helper_defines, "LANG_HOSTED_RUNTIME", build_ninja),
        "LANG_NATIVE_CLANG": _single_macro(
            helper_defines, "LANG_NATIVE_CLANG", build_ninja),
        "LANG_NATIVE_LLD": _single_macro(
            helper_defines, "LANG_NATIVE_LLD", build_ninja),
    }
    missing = [name for name in STAGE0_MACROS if name not in macros]
    if missing:
        raise RuntimeError(f"stage0 build configuration lacks {missing}: {build_ninja}")

    actual = _stage0_resource_paths(seed, env if env is not None else os.environ)
    path_source = "legacy_macros" if actual is None else "resource_paths"
    package_actual = None
    if actual is None:
        stdlib_reported = Path(macros["LANG_STD_SOURCE_ROOT"])
        helper_reported = Path(macros["LANG_NATIVE_HELPER"])
        runtime_reported = {
            "liblang_native_runtime.a": Path(macros["LANG_NATIVE_RUNTIME"]),
            "liblang_hosted_runtime.a": Path(macros["LANG_HOSTED_RUNTIME"]),
        }
        cpp_tools_reported = {
            "clang": Path(macros["LANG_NATIVE_CLANG"]),
            "ld.lld": Path(macros["LANG_NATIVE_LLD"]),
        }
    else:
        try:
            stdlib_reported = Path(actual["stdlib"])
            helper_reported = Path(actual["helper"])
            runtime_reported = {
                "liblang_native_runtime.a": Path(actual["runtime"]["native"]),
                "liblang_hosted_runtime.a": Path(actual["runtime"]["hosted"]),
            }
            cpp_tools_reported = {
                "clang": Path(actual["toolchain"]["clang"]),
                "ld.lld": Path(actual["toolchain"]["ld.lld"]),
            }
            package_actual = {
                "opt": Path(actual["toolchain"]["opt"]),
                "llc": Path(actual["toolchain"]["llc"]),
                "ninja": Path(actual["ninja"]),
            }
        except (KeyError, TypeError) as error:
            raise RuntimeError(
                f"stage0 resource paths are incomplete: {actual}") from error

    stdlib_macro = Path(macros["LANG_STD_SOURCE_ROOT"])
    helper_macro = Path(macros["LANG_NATIVE_HELPER"])
    runtime_macros = {
        "liblang_native_runtime.a": Path(macros["LANG_NATIVE_RUNTIME"]),
        "liblang_hosted_runtime.a": Path(macros["LANG_HOSTED_RUNTIME"]),
    }
    cpp_tool_macros = {
        "clang": Path(macros["LANG_NATIVE_CLANG"]),
        "ld.lld": Path(macros["LANG_NATIVE_LLD"]),
    }
    stdlib = stdlib_reported
    helper = helper_reported
    runtime = dict(runtime_reported)
    cpp_tools = dict(cpp_tools_reported)
    if not stdlib.is_dir():
        raise RuntimeError(f"stage0 stdlib input is missing: {stdlib}")
    helper = _required_file(helper, "stage0 helper")
    runtime = {name: _required_file(path, f"stage0 {name}")
               for name, path in runtime.items()}
    cpp_tools = {name: _required_file(path, f"stage0 {name}")
                 for name, path in cpp_tools.items()}
    selected_tools = {
        name: (toolchain / "bin" / name).resolve(strict=True)
        for name in ("clang", "ld.lld")
    }
    for name, actual_tool in cpp_tools.items():
        if actual_tool != selected_tools[name]:
            raise RuntimeError(
                f"stage0 {name} differs from --toolchain: {actual_tool} != "
                f"{selected_tools[name]}")
    package_tools = {}
    if package_objects:
        for name, macro in (("opt", "LANG_NATIVE_OPT"),
                            ("llc", "LANG_NATIVE_LLC"),
                            ("ninja", "LANG_NATIVE_NINJA")):
            macro_path = _single_macro(helper_defines, macro, build_ninja)
            reported = (package_actual[name] if package_actual is not None
                        else Path(macro_path))
            actual_tool = _required_file(
                reported if package_actual is not None else macro_path,
                f"stage0 {name}")
            if name != "ninja" and actual_tool != (toolchain / "bin" / name).resolve(strict=True):
                raise RuntimeError(f"stage0 {name} differs from --toolchain: {actual_tool}")
            package_tools[name] = {"path": str(actual_tool),
                                   "reported_path": str(reported),
                                   "macro_path": macro_path,
                                   "sha256": digest(actual_tool)}
    return {
        **({"package_tools": package_tools} if package_objects else {}),
        "path_source": path_source,
        "seed": {"path": str(seed), "sha256": digest(seed)},
        "helper": {"path": str(helper), "reported_path": str(helper_reported),
                   "macro_path": str(helper_macro), "sha256": digest(helper)},
        "stdlib": {"path": str(stdlib.resolve()),
                   "reported_path": str(stdlib_reported),
                   "macro_path": str(stdlib_macro),
                   "files": _tree_hashes(stdlib)},
        "runtime": {
            name: {"path": str(path), "reported_path": str(runtime_reported[name]),
                   "macro_path": str(runtime_macros[name]),
                   "sha256": digest(path)}
            for name, path in runtime.items()
        },
        "tools": {
            name: {"path": str(path), "reported_path": str(cpp_tools_reported[name]),
                   "macro_path": str(cpp_tool_macros[name]),
                   "sha256": digest(path)}
            for name, path in cpp_tools.items()
        },
        "build": {
            "build_ninja": str(build_ninja),
            "build_ninja_sha256": digest(build_ninja),
            "cmake_cache": str(cmake_cache),
            "cmake_cache_sha256": digest(cmake_cache),
            "cmake_home": home.group(1),
            "macros": macros,
        },
        "uses_copied_resources": False,
    }


def _freeze_reference(source, destination):
    source = _required_file(source, "stage0 input")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)
    source_hash = digest(source)
    if digest(destination) != source_hash:
        raise RuntimeError(f"stage0 input changed while freezing: {source}")
    return source_hash


def _check_stage0_path(item, label):
    reported = Path(item.get("reported_path", item["path"]))
    if str(reported.resolve(strict=True)) != item["path"]:
        raise RuntimeError(f"stage0 {label} target changed: {reported}")


def _compare_stage0_inputs(stage0, resources):
    macros = stage0["build"]["macros"]
    source = stage0.get("path_source", "legacy_macros")
    if source == "legacy_macros":
        if str(Path(macros["LANG_STD_SOURCE_ROOT"]).resolve(strict=True)) != \
                stage0["stdlib"]["path"]:
            raise RuntimeError(
                f"stage0 stdlib target changed: {macros['LANG_STD_SOURCE_ROOT']}")
        if str(Path(macros["LANG_NATIVE_HELPER"]).resolve(strict=True)) != \
                stage0["helper"]["path"]:
            raise RuntimeError(
                f"stage0 helper target changed: {macros['LANG_NATIVE_HELPER']}")
        for name in RUNTIME_ARCHIVES:
            macro = ("LANG_NATIVE_RUNTIME" if name == "liblang_native_runtime.a"
                     else "LANG_HOSTED_RUNTIME")
            if str(Path(macros[macro]).resolve(strict=True)) != \
                    stage0["runtime"][name]["path"]:
                raise RuntimeError(
                    f"stage0 {name} target changed: {macros[macro]}")
        for name in ("clang", "ld.lld"):
            macro = "LANG_NATIVE_CLANG" if name == "clang" else "LANG_NATIVE_LLD"
            if str(Path(macros[macro]).resolve(strict=True)) != \
                    stage0["tools"][name]["path"]:
                raise RuntimeError(
                    f"stage0 {name} target changed: {macros[macro]}")
    else:
        _check_stage0_path(stage0["stdlib"], "stdlib")
        _check_stage0_path(stage0["helper"], "helper")
        for name in RUNTIME_ARCHIVES:
            _check_stage0_path(stage0["runtime"][name], name)
        for name in ("clang", "ld.lld"):
            _check_stage0_path(stage0["tools"][name], name)
    seed_entry = stage0["seed"].get("entry_path", stage0["seed"]["path"])
    if str(Path(seed_entry).resolve(strict=True)) != stage0["seed"]["path"]:
        raise RuntimeError(f"stage0 seed target changed: {seed_entry}")
    actual_stdlib = _tree_hashes(stage0["stdlib"]["path"])
    frozen_stdlib = _tree_hashes(resources / "stdlib")
    if actual_stdlib != frozen_stdlib:
        changed = sorted(set(actual_stdlib) ^ set(frozen_stdlib) |
                         {name for name in frozen_stdlib
                          if actual_stdlib.get(name) != frozen_stdlib[name]})
        raise RuntimeError(
            f"stage0 stdlib differs from frozen copy: {changed}")
    for name in RUNTIME_ARCHIVES:
        frozen = digest(resources / "runtime" / name)
        actual = digest(Path(stage0["runtime"][name]["path"]))
        if actual != frozen:
            raise RuntimeError(
                f"stage0 {name} differs from frozen copy: "
                f"{stage0['runtime'][name]['path']} {actual} != {frozen}")
    for name in PYTHON_RUNTIME:
        if not (resources / "runtime/python" / name).is_file():
            raise RuntimeError(f"frozen Python runtime is missing: {name}")
    frozen_helper = digest(resources / "stage0/lang_native_helper")
    actual_helper = digest(Path(stage0["helper"]["path"]))
    if actual_helper != frozen_helper:
        raise RuntimeError(
            f"stage0 helper differs from frozen copy: "
            f"{stage0['helper']['path']} {actual_helper} != {frozen_helper}")
    frozen_seed = digest(resources / "stage0/woozyc")
    seed_entry = stage0["seed"].get("entry_path", stage0["seed"]["path"])
    actual_seed = digest(Path(seed_entry))
    if actual_seed != frozen_seed:
        raise RuntimeError(
            f"stage0 seed differs from frozen copy: "
            f"{seed_entry} {actual_seed} != {frozen_seed}")
    for name in ("clang", "ld.lld"):
        actual = digest(Path(stage0["tools"][name]["path"]))
        frozen = digest(resources / "toolchain" / name)
        if actual != frozen:
            raise RuntimeError(
                f"stage0 {name} differs from frozen tool: "
                f"{stage0['tools'][name]['path']} {actual} != {frozen}")
    for name, item in stage0.get("package_tools", {}).items():
        _check_stage0_path(item, name)
        frozen = resources / ("stage0/ninja" if name == "ninja" else f"toolchain/{name}")
        if digest(Path(item["path"])) != digest(frozen):
            raise RuntimeError(f"stage0 {name} differs from frozen copy")


def _add_tree(snapshot, prefix, root, strict=True, all_files=False):
    root = Path(root)
    try:
        files = (_all_tree_hashes(root) if all_files else _tree_hashes(root))
        target = str(root.resolve(strict=True))
    except (OSError, RuntimeError) as error:
        if strict:
            raise RuntimeError(f"{prefix} is missing: {root}") from error
        return
    for name, value in files.items():
        snapshot["files"][f"{prefix}/{name}"] = value
    snapshot["paths"][prefix] = target


def _add_file(snapshot, key, path, strict=True):
    path = Path(path)
    try:
        target = path.resolve(strict=True)
        if not target.is_file():
            raise FileNotFoundError(target)
        value = digest(target)
    except (OSError, RuntimeError) as error:
        if strict:
            raise RuntimeError(f"{key} is missing: {path}") from error
        return
    snapshot["files"][key] = value
    snapshot["paths"][key] = str(target)


def _frozen_inputs(output, stage0, toolchain, strict=True, source_make_tools=None,
                   env=None):
    snapshot = {"files": {}, "paths": {}}
    source = stage0.get("path_source", "legacy_macros")
    route = None
    if source == "resource_paths":
        try:
            actual = _stage0_resource_paths(
                stage0["seed"]["path"], env if env is not None else os.environ)
        except RuntimeError:
            if env is not None:
                raise
            actual = None
        if actual is not None:
            route = _stage0_route_paths(actual)
        elif env is None:
            route = _stage0_stored_route(stage0)
    unavailable = source == "resource_paths" and route is None
    _add_tree(snapshot, "compiler_sources", output / "source/src", strict)
    _add_file(snapshot, "compiler_manifest", output / "source/lang.toml", strict)
    _add_tree(snapshot, "stdlib_sources", output / "resources/stdlib", strict)
    _add_tree(snapshot, "runtime_files", output / "resources/runtime", strict,
              all_files=True)
    for name in RUNTIME_ARCHIVES:
        _add_file(snapshot, f"runtime_archives/{name}",
                  output / "resources/runtime" / name, strict)
    for name in PYTHON_RUNTIME:
        _add_file(snapshot, f"python_runtime/{name}",
                  output / "resources/runtime/python" / name, strict)
    for name in TOOL_NAMES:
        _add_file(snapshot, f"tools/{name}",
                  output / "resources/toolchain" / name, strict)
    entry = stage0["seed"].get("entry_path")
    if entry is not None:
        _add_file(snapshot, "stage0/seed_entry", entry, strict)
    _add_file(snapshot, "stage0/seed", stage0["seed"]["path"], strict)
    if not unavailable:
        _add_file(snapshot, "stage0/helper",
                  route["helper"] if route is not None
                  else stage0["helper"]["path"], strict)
    _add_file(snapshot, "stage0_frozen/seed", output / "resources/stage0/woozyc",
              strict)
    _add_file(snapshot, "stage0_frozen/helper",
              output / "resources/stage0/lang_native_helper", strict)
    _add_file(snapshot, "stage0/build_ninja", stage0["build"]["build_ninja"],
              strict)
    _add_file(snapshot, "stage0/cmake_cache", stage0["build"]["cmake_cache"],
              strict)
    macros = stage0["build"]["macros"]
    if source == "legacy_macros":
        _add_tree(snapshot, "stage0_macro/LANG_STD_SOURCE_ROOT",
                  macros["LANG_STD_SOURCE_ROOT"], strict)
        for macro in STAGE0_FILE_MACROS:
            _add_file(snapshot, f"stage0_macro/{macro}", macros[macro], strict)
    if not unavailable:
        _add_tree(snapshot, "stage0/stdlib",
                  route["stdlib"] if route is not None
                  else stage0["stdlib"]["path"], strict)
        for name in RUNTIME_ARCHIVES:
            _add_file(snapshot, f"stage0/runtime/{name}",
                      route["runtime"][name] if route is not None
                      else stage0["runtime"][name]["path"], strict)
        for name in ("clang", "ld.lld"):
            _add_file(snapshot, f"stage0/tools/{name}",
                      route["tools"][name] if route is not None
                      else stage0["tools"][name]["path"], strict)
    for name, item in stage0.get("package_tools", {}).items():
        if not unavailable:
            path = (route["package_tools"].get(name) if route is not None
                    else item["path"])
            if path is not None:
                _add_file(snapshot, f"stage0/package_tools/{name}", path,
                          strict)
        if name == "ninja":
            _add_file(snapshot, "stage0_frozen/ninja", output / "resources/stage0/ninja", strict)
    if route is not None:
        for key in ("stdlib", "helper"):
            snapshot["paths"][f"stage0_route/{key}"] = str(route[key])
        for name in RUNTIME_ARCHIVES:
            snapshot["paths"][f"stage0_route/runtime/{name}"] = str(
                route["runtime"][name])
        for name in ("clang", "ld.lld"):
            snapshot["paths"][f"stage0_route/tools/{name}"] = str(
                route["tools"][name])
        for name in stage0.get("package_tools", {}):
            if name in route["package_tools"]:
                snapshot["paths"][f"stage0_route/package_tools/{name}"] = str(
                    route["package_tools"][name])
    for name, path in (source_make_tools or {}).items():
        _add_file(snapshot, f"source_make_tools/{name}", path, strict)
    snapshot["paths"]["selected_toolchain"] = str(toolchain.resolve())
    return snapshot


def _input_changes(expected, actual):
    changes = {}
    for kind in ("files", "paths"):
        old = expected[kind]
        new = actual[kind]
        for key in sorted(set(old) | set(new)):
            if old.get(key) != new.get(key):
                changes[f"{kind}/{key}"] = {
                    "before": old.get(key), "after": new.get(key)}
    return changes


def _snapshot_digest(snapshot):
    return hashlib.sha256(
        json.dumps(snapshot, sort_keys=True, ensure_ascii=False).encode("utf-8")
    ).hexdigest()


def run(args):
    if _timeout_number(args.timeout) is None:
        raise ValueError("timeout must be a positive finite number")
    output = args.out_dir.resolve()
    if output.exists() and any(output.iterdir()):
        raise RuntimeError(f"output directory must be empty: {output}")
    source = ROOT / "selfhost/src"
    if output.is_relative_to(source):
        raise RuntimeError("bootstrap output cannot be inside compiler source")
    output.mkdir(parents=True, exist_ok=True)
    report = {"scope": "compiler_build_stages", "stages": [],
              "corpus_parity_checked": False, "status": "running"}
    report_path = output / "result.json"
    stage_label = "setup"
    current_record = None
    try:
        strace = None
        if args.stages > 1:
            # Keep './name' distinct from a bare PATH lookup and resolve it
            # before launching any compiler in the frozen source directory.
            requested = args.strace if args.strace is not None else "strace"
            found = shutil.which(requested)
            if not found:
                raise RuntimeError(f"executable strace is required for later stages: {requested}")
            strace = Path(found).absolute()
        stage0_entry = Path(args.stage0).absolute()
        stage0_path = args.stage0.resolve(strict=True)
        toolchain = args.toolchain.resolve(strict=True)
        resources = output / "resources"
        resources.mkdir()
        report["compiler_sources"] = copy_sources(source, output / "source/src")
        shutil.copyfile(ROOT / "selfhost/lang.toml", output / "source/lang.toml")
        report["manifest_sha256"] = digest(output / "source/lang.toml")
        report["stdlib_sources"] = copy_sources(ROOT / "stdlib", resources / "stdlib")
        runtime = resources / "runtime"
        runtime.mkdir()
        for name in RUNTIME_ARCHIVES:
            shutil.copyfile(args.runtime_dir / name, runtime / name)
        (runtime / "python").mkdir()
        for name in PYTHON_RUNTIME:
            shutil.copyfile(ROOT / "runtime/python" / name, runtime / "python" / name)
        tools = resources / "toolchain"
        tools.mkdir()
        allowed_tools = set()
        for name in TOOL_NAMES:
            executable = (toolchain / "bin" / name).resolve(strict=True)
            (tools / name).symlink_to(executable)
            allowed_tools.add(str(executable))
        environment = dict(os.environ)
        environment.pop("WOOZY_LLVM_BIN", None)
        source_make_tools = {}
        if getattr(args, "source_make", False) and args.stages > 1:
            source_make_tools = {name: Path(path) for name, path in {
                "make": "/usr/bin/make", "shell": "/bin/sh",
                "mkdir": "/usr/bin/mkdir"}.items()}
            for name, path in source_make_tools.items():
                _required_file(path, f"source-make {name}")
            # Do not inherit outer Make options, jobserver settings, extra
            # makefiles, or command-line variable overrides.
            for name in ("GNUMAKEFLAGS", "MAKEFILES", "MAKEOVERRIDES", "MFLAGS",
                         "MAKELEVEL", "MAKE_RESTARTS"):
                environment.pop(name, None)
            environment["MAKEFLAGS"] = f"-j{getattr(args, 'jobs', 1)}"
            version = subprocess.run([str(source_make_tools["make"]), "--version"],
                                     env=environment, capture_output=True, text=True, check=False)
            if version.returncode or not version.stdout.startswith("GNU Make "):
                raise RuntimeError("source-make requires GNU Make")
            environment["WOOZY_MAKE"] = str(source_make_tools["make"])
            environment["PATH"] = "/usr/bin:/bin"
            allowed_tools.update(str(path.resolve()) for path in source_make_tools.values())
            report["source_make_tools"] = {name: str(path)
                                           for name, path in source_make_tools.items()}
        libraries = [str(toolchain / "lib"), str(args.libxml_dir.resolve())]
        if environment.get("LD_LIBRARY_PATH"):
            libraries.append(environment["LD_LIBRARY_PATH"])
        environment["LD_LIBRARY_PATH"] = ":".join(libraries)

        stage_label = "stage0"
        stage0 = inspect_stage0(stage0_path, toolchain,
                                package_objects=getattr(args, "source_make", False),
                                env=environment)
        stage0["seed"]["entry_path"] = str(stage0_entry)
        frozen_stage0 = resources / "stage0"
        frozen_stage0.mkdir()
        _freeze_reference(stage0["seed"]["path"], frozen_stage0 / "woozyc")
        _freeze_reference(stage0["helper"]["path"],
                          frozen_stage0 / "lang_native_helper")
        if "ninja" in stage0.get("package_tools", {}):
            _freeze_reference(stage0["package_tools"]["ninja"]["path"],
                              frozen_stage0 / "ninja")
        _compare_stage0_inputs(stage0, resources)
        report["stage0"] = stage0
        expected_inputs = _frozen_inputs(output, stage0, toolchain, strict=True,
                                         source_make_tools=source_make_tools,
                                         env=environment)
        _compare_stage0_inputs(stage0, resources)
        report["frozen_inputs"] = expected_inputs
        expected_digest = _snapshot_digest(expected_inputs)

        compiler = stage0_path
        compiler_sha256 = stage0["seed"]["sha256"]
        for generation in range(1, args.stages + 1):
            stage_label = f"stage{generation}"
            before = _frozen_inputs(output, stage0, toolchain, strict=False,
                                    source_make_tools=source_make_tools,
                                    env=environment)
            if before != expected_inputs:
                raise RuntimeError(
                    f"frozen inputs changed before {stage_label}: "
                    f"{_input_changes(expected_inputs, before)}")
            if digest(compiler) != compiler_sha256:
                raise RuntimeError(
                    f"compiler changed before {stage_label}: {compiler}")
            stage = output / stage_label
            stage.mkdir()
            for name in ("stdlib", "runtime", "toolchain"):
                (stage / name).symlink_to(Path("..") / "resources" / name, target_is_directory=True)
            command = [str(compiler), "build", "--project",
                       str(output / "source/lang.toml"), "--backend", "native",
                       "--opt", args.opt, "--out-dir", str(stage / "build"),
                       "--format", "json"]
            if getattr(args, "source_make", False):
                command.insert(4, "--package-objects" if generation == 1 else "--source-make")
            trace = stage / "exec.log"
            traced = command
            if generation > 1:
                traced = [str(strace), "--seccomp-bpf", "-f", "-qq", "-s", "16384",
                          "-e", "trace=execve,execveat", "-o", str(trace), *command]
            print(f"building {stage_label} with {compiler}", flush=True)
            record = {"generation": generation, "compiler": str(compiler),
                      "compiler_sha256": compiler_sha256,
                      "command": command, "status": "running",
                      "frozen_inputs_sha256": expected_digest}
            report["stages"].append(record)
            current_record = record
            try:
                process = _run_process_impl(
                    traced, output / "source", environment, args.timeout)
                stdout = process["stdout"]
                stderr = process["stderr"]
                (stage / "stdout.log").write_bytes(stdout)
                (stage / "stderr.log").write_bytes(stderr)
                record.update({
                    "exit": process["exit"],
                    "timed_out": process["timed_out"],
                    "cleanup_complete": process["cleanup_complete"],
                    "abandoned_pipes": process["abandoned_pipes"],
                    "stdout_sha256": hashlib.sha256(stdout).hexdigest(),
                    "stderr_sha256": hashlib.sha256(stderr).hexdigest(),
                })

                failures = []
                after = _frozen_inputs(output, stage0, toolchain, strict=False,
                                    source_make_tools=source_make_tools,
                                    env=environment)
                if after != expected_inputs:
                    record["input_changes"] = _input_changes(
                        expected_inputs, after)
                    failures.append("frozen inputs changed during build")
                after_compiler_sha256 = digest(compiler)
                record["compiler_after_sha256"] = after_compiler_sha256
                if after_compiler_sha256 != compiler_sha256:
                    failures.append("compiler identity changed during build")
                if process["timed_out"]:
                    failures.append(f"{stage_label} timed out and was terminated")
                if not process["cleanup_complete"]:
                    failures.append(
                        f"{stage_label} process cleanup was incomplete; "
                        "pipes were abandoned")
                trace_error = None
                if generation > 1:
                    try:
                        record["executables"] = trace_executables(
                            trace, allowed_tools | {str(compiler.resolve())})
                    except RuntimeError as error:
                        trace_error = str(error)
                        record["trace_error"] = trace_error
                        if not process["timed_out"]:
                            failures.append(trace_error)
                if process["exit"]:
                    failures.append(f"{stage_label} failed; see {stage}")
                if failures:
                    raise RuntimeError(
                        f"{stage_label} failed: " + "; ".join(failures))
                status = json.loads(stdout)
                if not isinstance(status, dict) or status.get("status") != "ok":
                    raise RuntimeError(
                        f"{stage_label} did not report a successful build")
                binary = stage / "build/program"
                if not binary.is_file() or not os.access(binary, os.X_OK):
                    raise RuntimeError(
                        f"{stage_label} did not create an executable")
                compiler = stage / "woozyc"
                shutil.copy2(binary, compiler)
                compiler_sha256 = digest(compiler)
                record["sha256"] = compiler_sha256
                record["status"] = "ok"
                current_record = None
            except Exception:
                record["status"] = "failed"
                raise
        if args.stages == 3:
            report["stage23_binary_equal"] = digest(output / "stage2/woozyc") == \
                digest(output / "stage3/woozyc")
        if args.stages == 3 and not report["stage23_binary_equal"]:
            raise RuntimeError("stage2 and stage3 binaries differ; inspect the generated IR")
        report["status"] = "ok"
    except Exception as error:
        if current_record is not None and current_record.get(
                "status") == "running":
            current_record["status"] = "failed"
        report["status"] = "failed"
        report["failed_stage"] = stage_label
        report["error"] = str(error)
        raise
    finally:
        report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
        print(f"bootstrap result: {report_path}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage0", type=Path,
                        default=ROOT / "build/woozy-stdlib-native/woozyc")
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--stages", type=int, choices=(1, 2, 3), default=3)
    parser.add_argument("--opt", choices=("0", "2"), default="2")
    parser.add_argument("--jobs", type=int, default=1,
                        help="parallel source Make workers (default: 1)")
    parser.add_argument("--source-make", action="store_true",
                        help="use package objects for stage 1 and source Make for stages 2 and 3")
    parser.add_argument("--toolchain", type=Path, default=DEFAULT_LLVM)
    parser.add_argument("--runtime-dir", type=Path,
                        default=ROOT / "build/woozy-stdlib-native/native")
    parser.add_argument("--libxml-dir", type=Path, default=DEFAULT_LIBXML)
    parser.add_argument("--strace", help="strace executable (default: find strace on PATH)")
    parser.add_argument("--timeout", type=float, default=7200)
    args = parser.parse_args()
    if args.jobs <= 0:
        parser.error("jobs must be positive")
    if _timeout_number(args.timeout) is None:
        parser.error("timeout must be a positive finite number")
    try:
        if args.stages > 1 and args.opt == "0":
            # O0 compiler workers can exhaust Linux's 8 MiB default stack.
            minimum = 16 * 1024 * 1024
            soft, hard = resource.getrlimit(resource.RLIMIT_STACK)
            if soft != resource.RLIM_INFINITY and soft < minimum:
                if hard != resource.RLIM_INFINITY and hard < minimum:
                    raise RuntimeError("O0 self-hosting requires a stack limit of at least 16 MiB")
                resource.setrlimit(resource.RLIMIT_STACK, (minimum, hard))
        run(args)
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
