"""Small checks for bootstrap subprocess auditing, freezing and stage0 inputs."""

import argparse
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import time
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("bootstrap", ROOT / "selfhost/bootstrap.py")
bootstrap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bootstrap)

OLD_CLI_ARGUMENT = (
    "E_DRIVER_ARGUMENT: expected --help, --version, check, build, "
    "lsp or capabilities")
OLD_CLI_GUARD = (
    "import sys\n"
    "if sys.argv[1:] == ['--__resource-paths']:\n"
    f"    sys.stderr.write({OLD_CLI_ARGUMENT!r} + '\\n')\n"
    "    sys.exit(2)\n")


def check_trace_audit(work):
    trace = work / "exec.log"
    compiler = work / "stage1/woozyc"
    llvm = work / "toolchain/clang"
    allowed = {str(compiler), str(llvm)}
    trace.write_text(f'123 execve("{compiler}", ["woozyc"], 0x0) = 0\n'
                     f'124 execveat(AT_FDCWD, "{llvm}", ["clang"], 0x0, 0) = 0\n')
    assert bootstrap.trace_executables(trace, allowed) == sorted(allowed)
    for text in ('execve("./clang", ["clang"], 0x0) = 0\n',
                 'execveat(AT_FDCWD, "./clang", ["clang"], 0x0, 0) = 0\n',
                 'execve("/old/lang_native_helper", [], 0x0) = 0\n',
                 'execveat(3, "", [], 0x0, AT_EMPTY_PATH) = 0\n',
                 'execve(0x7fff, [""], 0x0) = 0\n',
                 'execveat(3, 0x7fff, [], 0x0, 0) = 0\n',
                 '+++ exited with 0 +++\n'):
        trace.write_text(text)
        try:
            bootstrap.trace_executables(trace, allowed)
        except RuntimeError:
            pass
        else:
            raise AssertionError(f"accepted unverified execution: {text}")


def check_stage0_resource_paths(work):
    seed = work / "resource-seed"
    seed.write_bytes(b"fixture")
    responses = [
        (2, "", OLD_CLI_ARGUMENT + "\n", None),
        (2, "", "E_TOOLCHAIN_MISSING: helper rejected E_DRIVER_ARGUMENT: "
            + OLD_CLI_ARGUMENT + "\n", RuntimeError),
        (2, '{"status":"error","diagnostics":[{"code":"E_DRIVER_ARGUMENT"}]}',
         "", RuntimeError),
        (1, "Traceback (most recent call last):\nE_DRIVER_ARGUMENT: "
            + OLD_CLI_ARGUMENT + "\n", "", RuntimeError),
        (2, "warning\n" + OLD_CLI_ARGUMENT + "\n", "", RuntimeError),
    ]
    with mock.patch.object(
            bootstrap, "open", mock.mock_open(read_data=b"\x7fELF fixture"),
            create=True):
        for code, stdout, stderr, expected in responses:
            completed = argparse.Namespace(
                returncode=code, stdout=stdout, stderr=stderr)
            with mock.patch.object(bootstrap.subprocess, "run",
                                   return_value=completed):
                if expected is None:
                    assert bootstrap._stage0_resource_paths(seed, {}) is None
                else:
                    try:
                        bootstrap._stage0_resource_paths(seed, {})
                    except expected:
                        pass
                    else:
                        raise AssertionError(
                            f"accepted stage0 resource failure: {stdout}{stderr}")


def check_source_copy(work):
    source = work / "source"
    source.mkdir()
    (source / "main.wz").write_text("package main; func main() {}\n")
    (source / "unrelated.txt").write_text("excluded\n")
    copied = work / "copy"
    hashes = bootstrap.copy_sources(source, copied)
    assert set(hashes) == {"main.wz"}
    assert (source / "main.wz").read_bytes() == (copied / "main.wz").read_bytes()
    assert not (copied / "unrelated.txt").exists()


def write_executable(path, body):
    path.write_text("#!" + str(Path(sys.executable).resolve()) + "\n" + body)
    path.chmod(0o700)
    return path


def compiler_body(mutation=None):
    lines = [
        "import json",
        "import pathlib",
        "import sys",
        "argv = sys.argv",
        "out = pathlib.Path(argv[argv.index('--out-dir') + 1])",
        "base = out.parents[1]",
        "out.mkdir(parents=True, exist_ok=True)",
        "program = out / 'program'",
        "program.write_text('#!/bin/sh\\nexit 0\\n')",
        "program.chmod(0o755)",
    ]
    if mutation is not None:
        kind, target = mutation
        if kind == "output":
            lines.append(f"target = base / {target!r}")
            lines.append("target.write_bytes(target.read_bytes() + b'mutated\\n')")
        elif kind == "output-add":
            lines.append(f"target = base / {target!r}")
            lines.append("target.write_bytes(b'added\\n')")
        elif kind == "output-remove":
            lines.append(f"(base / {target!r}).unlink()")
        else:
            lines.append(f"target = pathlib.Path({str(target)!r})")
            lines.append("target.write_bytes(target.read_bytes() + b'mutated\\n')")
    lines += ["print(json.dumps({'status': 'ok'}), flush=True)"]
    return OLD_CLI_GUARD + "\n".join(lines) + "\n"


def compiler_body_with_actions(actions):
    marker = "print(json.dumps({'status': 'ok'}), flush=True)"
    return compiler_body().replace(marker, actions + "\n" + marker)


def make_fixture(work, name, seed_body):
    if "--__resource-paths" not in seed_body:
        seed_body = OLD_CLI_GUARD + seed_body
    fixture = work / name
    (fixture / "selfhost/src").mkdir(parents=True)
    (fixture / "selfhost/src/main.wz").write_text(
        "package main; func main() {}\n")
    (fixture / "selfhost/lang.toml").write_text("schema_version = 1\n")
    (fixture / "stdlib").mkdir()
    (fixture / "stdlib/std.wz").write_text("package std;\n")
    (fixture / "runtime/python").mkdir(parents=True)
    (fixture / "runtime/python/langrt.py").write_text("# runtime\n")
    (fixture / "runtime/python/unicode18.py").write_text("# unicode\n")
    runtime = fixture / "seed-build/native"
    runtime.mkdir(parents=True)
    for archive in bootstrap.RUNTIME_ARCHIVES:
        (runtime / archive).write_bytes(("archive " + archive).encode())
    helper = runtime / "lang_native_helper"
    helper.write_bytes(b"frozen helper\n")
    seed = write_executable(fixture / "seed-build/woozyc", seed_body)
    toolchain = fixture / "toolchain"
    real_tools = toolchain / "files"
    real_tools.mkdir(parents=True)
    for name in ("opt", "llc", "clang", "clang++", "ld.lld"):
        (real_tools / name).write_bytes(("tool " + name).encode())
        (toolchain / "bin").mkdir(parents=True, exist_ok=True)
        (toolchain / "bin" / name).symlink_to(real_tools / name)
    (fixture / "libxml").mkdir()
    defines = {
        "LANG_STD_SOURCE_ROOT": str(fixture / "stdlib"),
        "LANG_NATIVE_HELPER": str(helper),
        "LANG_NATIVE_RUNTIME": str(runtime / "liblang_native_runtime.a"),
        "LANG_HOSTED_RUNTIME": str(runtime / "liblang_hosted_runtime.a"),
        "LANG_NATIVE_CLANG": str(toolchain / "bin/clang"),
        "LANG_NATIVE_LLD": str(toolchain / "bin/ld.lld"),
    }
    seed_defines = '  DEFINES = -DLANG_NATIVE_HELPER=\\"%s\\"\n' % (
        defines["LANG_NATIVE_HELPER"])
    std_defines = '  DEFINES = -DLANG_STD_SOURCE_ROOT=\\"%s\\"\n' % (
        defines["LANG_STD_SOURCE_ROOT"])
    helper_defines = "  DEFINES = " + " ".join(
        '-D%s=\\"%s\\"' % (name, defines[name]) for name in (
            "LANG_HOSTED_RUNTIME", "LANG_NATIVE_CLANG", "LANG_NATIVE_LLD",
            "LANG_NATIVE_RUNTIME")) + "\n"
    (fixture / "seed-build/build.ninja").write_text(
        "build CMakeFiles/woozyc.dir/src/driver/main.cpp.o: CXX\n"
        + seed_defines
        + "build CMakeFiles/lang_modules.dir/src/modules/project.cpp.o: CXX\n"
        + std_defines
        + "build native/CMakeFiles/lang_native_helper.dir/helper.cpp.o: CXX\n"
        + helper_defines)
    (fixture / "seed-build/CMakeCache.txt").write_text(
        "LANG_ENABLE_LLVM:BOOL=ON\n"
        f"CMAKE_HOME_DIRECTORY:INTERNAL={fixture}\n")
    return {
        "fixture": fixture,
        "seed": seed,
        "helper": helper,
        "runtime": runtime,
        "toolchain": toolchain,
        "tool": real_tools / "clang",
        "stdlib": fixture / "stdlib/std.wz",
        "manifest": fixture / "selfhost/lang.toml",
        "compiler_source": fixture / "selfhost/src/main.wz",
        "python_runtime": fixture / "runtime/python/langrt.py",
    }


def output_dir(fixture):
    return fixture["fixture"].with_name(fixture["fixture"].name + "-out")


def bootstrap_args(work, fixture, timeout=10.0):
    return argparse.Namespace(
        stage0=fixture["seed"], out_dir=output_dir(fixture),
        stages=1, opt="0", toolchain=fixture["toolchain"],
        runtime_dir=fixture["runtime"], libxml_dir=fixture["fixture"] / "libxml",
        strace=None, timeout=timeout)


def read_report(fixture):
    return json.loads((output_dir(fixture) / "result.json").read_text())


def check_timeout_process_group(work):
    pidfile = work / "grandchild.pid"
    body = (
        "import pathlib\n"
        "import subprocess\n"
        "import sys\n"
        "import time\n"
        "child = subprocess.Popen([sys.executable, '-c', "
        "'import time; time.sleep(30)'])\n"
        f"pathlib.Path({str(pidfile)!r}).write_text(str(child.pid))\n"
        "time.sleep(30)\n")
    fixture = make_fixture(work, "timeout-fixture", body)
    bootstrap.ROOT = fixture["fixture"]
    try:
        bootstrap.run(bootstrap_args(work, fixture, timeout=0.3))
    except RuntimeError as error:
        assert "timed out" in str(error), error
    else:
        raise AssertionError("bootstrap accepted a timed out stage")
    report = read_report(fixture)
    assert report["status"] == "failed"
    assert report["failed_stage"] == "stage1"
    record = report["stages"][0]
    assert record["timed_out"] is True
    assert record["exit"] == -9
    assert record["stdout_sha256"] and record["stderr_sha256"]
    assert (output_dir(fixture) / "stage1/stdout.log").is_file()
    assert (output_dir(fixture) / "stage1/stderr.log").is_file()
    pid = int(pidfile.read_text())
    for _ in range(40):
        state = Path(f"/proc/{pid}/stat")
        if not state.exists() or state.read_text().split()[2] == "Z":
            break
        time.sleep(0.05)
    if state.exists() and state.read_text().split()[2] != "Z":
        os.kill(pid, 9)
        raise AssertionError(f"bootstrap left grandchild {pid} running")


def check_detached_pipe_is_bounded(work):
    pidfile = work / "detached.pid"
    body = (
        "import pathlib\n"
        "import subprocess\n"
        "import sys\n"
        "import time\n"
        "child = subprocess.Popen([sys.executable, '-c', "
        "'import time; time.sleep(5)'], start_new_session=True)\n"
        f"pathlib.Path({str(pidfile)!r}).write_text(str(child.pid))\n"
        "time.sleep(5)\n")
    fixture = make_fixture(work, "detached-fixture", body)
    bootstrap.ROOT = fixture["fixture"]
    started = time.monotonic()
    try:
        bootstrap.run(bootstrap_args(work, fixture, timeout=0.2))
    except RuntimeError as error:
        assert "cleanup was incomplete" in str(error), error
    else:
        raise AssertionError("bootstrap accepted abandoned pipes")
    elapsed = time.monotonic() - started
    assert elapsed < 2.0, elapsed
    report = read_report(fixture)
    record = report["stages"][0]
    assert record["timed_out"] is True
    assert record["cleanup_complete"] is False
    assert record["abandoned_pipes"] is True
    pid = int(pidfile.read_text())
    try:
        os.kill(pid, 9)
    except ProcessLookupError:
        pass
    for _ in range(40):
        state = Path(f"/proc/{pid}/stat")
        if not state.exists() or state.read_text().split()[2] == "Z":
            break
        time.sleep(0.05)


def check_stable_control(work):
    fixture = make_fixture(work, "stable-fixture", compiler_body())
    bootstrap.ROOT = fixture["fixture"]
    bootstrap.run(bootstrap_args(work, fixture))
    report = read_report(fixture)
    assert report["status"] == "ok", report
    assert report["corpus_parity_checked"] is False
    assert "stage23_binary_equal" not in report
    assert report["stage0"]["uses_copied_resources"] is False
    assert report["stage0"]["seed"]["path"] == str(fixture["seed"])
    assert report["stage0"]["helper"]["path"] == str(fixture["helper"])
    assert report["stages"][0]["status"] == "ok"
    assert report["stages"][0]["frozen_inputs_sha256"]


def check_input_mutations(work):
    mutations = {
        "compiler-source": "compiler_sources/main.wz",
        "manifest": "compiler_manifest",
        "stdlib": "stage0/stdlib/std.wz",
        "stdlib-added": "stdlib_sources/new.wz",
        "stdlib-frozen": "stdlib_sources/std.wz",
        "compiler-source-removed": "compiler_sources/main.wz",
        "native-runtime": "stage0/runtime/liblang_native_runtime.a",
        "native-runtime-frozen": "runtime_archives/liblang_native_runtime.a",
        "hosted-runtime": "stage0/runtime/liblang_hosted_runtime.a",
        "python-runtime": "python_runtime/langrt.py",
        "runtime-extra-archive": "runtime_files/extra.a",
        "runtime-extra-python": "runtime_files/python/extra.py",
        "runtime-missing-archive": "runtime_archives/liblang_native_runtime.a",
        "tool": "stage0/tools/clang",
        "seed": "stage0/seed",
    }
    for name, expected_key in mutations.items():
        case_work = work / ("mutation-" + name)
        case_work.mkdir()
        fixture = make_fixture(case_work, "fixture", compiler_body())
        targets = {
            "compiler-source": ("output", "source/src/main.wz"),
            "manifest": ("output", "source/lang.toml"),
            "stdlib": ("absolute", fixture["stdlib"]),
            "stdlib-added": ("output-add", "resources/stdlib/new.wz"),
            "stdlib-frozen": ("output", "resources/stdlib/std.wz"),
            "compiler-source-removed": ("output-remove", "source/src/main.wz"),
            "native-runtime": (
                "absolute", fixture["runtime"] / "liblang_native_runtime.a"),
            "native-runtime-frozen": (
                "output", "resources/runtime/liblang_native_runtime.a"),
            "hosted-runtime": (
                "absolute", fixture["runtime"] / "liblang_hosted_runtime.a"),
            "python-runtime": (
                "output", "resources/runtime/python/langrt.py"),
            "runtime-extra-archive": (
                "output-add", "resources/runtime/extra.a"),
            "runtime-extra-python": (
                "output-add", "resources/runtime/python/extra.py"),
            "runtime-missing-archive": (
                "output-remove", "resources/runtime/liblang_native_runtime.a"),
            "tool": ("absolute", fixture["tool"]),
            "seed": ("absolute", fixture["seed"]),
        }
        write_executable(fixture["seed"], compiler_body(targets[name]))
        bootstrap.ROOT = fixture["fixture"]
        try:
            bootstrap.run(bootstrap_args(case_work, fixture))
        except RuntimeError as error:
            assert "frozen inputs changed" in str(error), (name, error)
        else:
            raise AssertionError(f"bootstrap accepted changed {name} input")
        report = read_report(fixture)
        assert report["failed_stage"] == "stage1", (name, report)
        changes = report["stages"][0]["input_changes"]
        assert f"files/{expected_key}" in changes, (name, sorted(changes))
        assert report["stage0"]["seed"]["sha256"]


def check_stage0_macro_retargets(work):
    def helper_retarget(fixture):
        alias = fixture["fixture"] / "seed-build/helper-entry"
        alias.symlink_to(fixture["helper"])
        replacement = fixture["fixture"] / "seed-build/replacement-helper"
        replacement.write_bytes(b"replacement helper\n")
        ninja = fixture["fixture"] / "seed-build/build.ninja"
        ninja.write_text(ninja.read_text().replace(
            str(fixture["helper"]), str(alias)))
        return (f"target = pathlib.Path({str(alias)!r})\n"
                "target.unlink()\n"
                f"target.symlink_to({str(replacement)!r})"), \
            "files/stage0_macro/LANG_NATIVE_HELPER"

    def stdlib_retarget(fixture):
        alias = fixture["fixture"] / "stdlib-entry"
        alias.symlink_to(fixture["fixture"] / "stdlib", target_is_directory=True)
        replacement = fixture["fixture"] / "replacement-stdlib"
        replacement.mkdir()
        (replacement / "std.wz").write_text("package different;\n")
        ninja = fixture["fixture"] / "seed-build/build.ninja"
        ninja.write_text(ninja.read_text().replace(
            str(fixture["fixture"] / "stdlib"), str(alias)))
        return (f"target = pathlib.Path({str(alias)!r})\n"
                "target.unlink()\n"
                f"target.symlink_to({str(replacement)!r}, "
                "target_is_directory=True)"), \
            "files/stage0_macro/LANG_STD_SOURCE_ROOT/std.wz"

    def clang_retarget(fixture):
        alias = fixture["toolchain"] / "bin/clang"
        replacement = fixture["toolchain"] / "files/replacement-clang"
        replacement.write_bytes(b"replacement clang\n")
        return (f"target = pathlib.Path({str(alias)!r})\n"
                "target.unlink()\n"
                f"target.symlink_to({str(replacement)!r})"), \
            "files/stage0_macro/LANG_NATIVE_CLANG"

    cases = [
        ("helper-alias", helper_retarget),
        ("stdlib-alias", stdlib_retarget),
        ("clang-alias", clang_retarget),
    ]
    for name, prepare in cases:
        case_work = work / ("retarget-" + name)
        case_work.mkdir()
        fixture = make_fixture(case_work, "fixture", compiler_body())
        actions, expected_key = prepare(fixture)
        write_executable(fixture["seed"], compiler_body_with_actions(actions))
        bootstrap.ROOT = fixture["fixture"]
        try:
            bootstrap.run(bootstrap_args(case_work, fixture))
        except RuntimeError as error:
            assert "frozen inputs changed" in str(error), (name, error)
        else:
            raise AssertionError(f"bootstrap accepted retargeted {name} macro")
        report = read_report(fixture)
        assert report["status"] == "failed", (name, report["status"])
        assert report["failed_stage"] == "stage1", (name, report)
        record = report["stages"][0]
        assert record["status"] == "failed", (name, record["status"])
        assert expected_key in record["input_changes"], (
            name, sorted(record["input_changes"]))


def check_stage0_route_rechecks(work):
    def prepare(work, name, action):
        case_work = work / ("route-" + name)
        case_work.mkdir()
        fixture = make_fixture(case_work, "fixture", compiler_body())
        alias = fixture["fixture"] / "seed-build/helper-entry"
        alias.symlink_to(fixture["helper"])
        route_file = fixture["fixture"] / "seed-build/resource-paths.json"
        route = {
            "stdlib": str(fixture["fixture"] / "stdlib"),
            "helper": str(alias),
            "runtime": {
                "native": str(fixture["runtime"]
                              / "liblang_native_runtime.a"),
                "hosted": str(fixture["runtime"]
                              / "liblang_hosted_runtime.a"),
            },
            "toolchain": {
                "opt": str(fixture["toolchain"] / "bin/opt"),
                "llc": str(fixture["toolchain"] / "bin/llc"),
                "clang": str(fixture["toolchain"] / "bin/clang"),
                "ld.lld": str(fixture["toolchain"] / "bin/ld.lld"),
            },
            "ninja": str(fixture["toolchain"] / "bin/clang"),
        }
        route_file.write_text(json.dumps(route))
        write_executable(fixture["seed"], compiler_body_with_actions(action(
            fixture, alias, route_file, route)))
        bootstrap.ROOT = fixture["fixture"]
        with mock.patch.object(
                bootstrap, "_stage0_resource_paths",
                side_effect=lambda seed, env: json.loads(route_file.read_text())):
            try:
                bootstrap.run(bootstrap_args(case_work, fixture))
            except RuntimeError as error:
                assert "frozen inputs changed" in str(error), (name, error)
            else:
                raise AssertionError(f"bootstrap accepted route change {name}")
        report = read_report(fixture)
        assert report["failed_stage"] == "stage1", (name, report)
        return report["stages"][0]["input_changes"]

    def retarget(fixture, alias, route_file, route):
        replacement = fixture["fixture"] / "seed-build/replacement-helper"
        replacement.write_bytes(b"replacement helper\n")
        return (f"target = pathlib.Path({str(alias)!r})\n"
                "target.unlink()\n"
                f"target.symlink_to({str(replacement)!r})\n")

    def mismatch(fixture, alias, route_file, route):
        replacement = fixture["fixture"] / "seed-build/replacement-helper"
        replacement.write_bytes(b"replacement helper\n")
        changed = dict(route, helper=str(replacement))
        return (f"pathlib.Path({str(route_file)!r}).write_text("
                f"{json.dumps(changed)!r})\n")

    assert "files/stage0/helper" in prepare(work, "retarget", retarget)
    changes = prepare(work, "mismatch", mismatch)
    assert "files/stage0/helper" in changes, sorted(changes)
    assert "paths/stage0_route/helper" in changes, sorted(changes)


def check_stage_exception_status(work):
    fixture = make_fixture(
        work, "missing-program-fixture",
        "import json\n"
        "import pathlib\n"
        "import sys\n"
        "argv = sys.argv\n"
        "out = pathlib.Path(argv[argv.index('--out-dir') + 1])\n"
        "out.mkdir(parents=True, exist_ok=True)\n"
        "print(json.dumps({'status': 'ok'}), flush=True)\n")
    bootstrap.ROOT = fixture["fixture"]
    try:
        bootstrap.run(bootstrap_args(work, fixture))
    except RuntimeError as error:
        assert "did not create an executable" in str(error), error
    else:
        raise AssertionError("bootstrap accepted a missing stage program")
    report = read_report(fixture)
    assert report["status"] == "failed"
    assert report["failed_stage"] == "stage1"
    assert report["stages"][0]["status"] == "failed"


def check_unconfirmed_seed(work):
    fixture = make_fixture(work, "missing-cache-fixture", compiler_body())
    (fixture["fixture"] / "seed-build/CMakeCache.txt").unlink()
    bootstrap.ROOT = fixture["fixture"]
    try:
        bootstrap.run(bootstrap_args(work, fixture))
    except RuntimeError as error:
        assert "CMakeCache" in str(error), error
    else:
        raise AssertionError("bootstrap accepted a seed without CMake inputs")
    report = read_report(fixture)
    assert report["failed_stage"] == "stage0"


def check_stack_limit():
    r = bootstrap.resource
    argv = ["bootstrap.py", "--out-dir", "unused", "--stages", "2", "--opt", "0"]
    minimum = 16 * 1024 * 1024
    for limit in (minimum // 2, minimum * 2, r.RLIM_INFINITY):
        with (mock.patch.object(sys, "argv", argv),
              mock.patch.object(bootstrap, "run") as run,
              mock.patch.object(r, "getrlimit", return_value=(limit, r.RLIM_INFINITY)),
              mock.patch.object(r, "setrlimit") as set_limit):
            assert bootstrap.main() == 0
            run.assert_called_once()
            if limit == minimum // 2:
                set_limit.assert_called_once_with(r.RLIMIT_STACK, (minimum, r.RLIM_INFINITY))
            else:
                set_limit.assert_not_called()


with tempfile.TemporaryDirectory(prefix="woozy-bootstrap-check-") as directory:
    work = Path(directory)
    check_trace_audit(work)
    check_source_copy(work)
    check_timeout_process_group(work)
    check_detached_pipe_is_bounded(work)
    check_stage0_resource_paths(work)
    check_stable_control(work)
    check_input_mutations(work)
    check_stage0_macro_retargets(work)
    check_stage0_route_rechecks(work)
    check_stage_exception_status(work)
    check_unconfirmed_seed(work)
    check_stack_limit()

print("PASS: bootstrap execution audit, bounded timeout, frozen inputs, "
      "stage0 CMake inputs, and synthetic stage control")
