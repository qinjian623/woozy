#!/usr/bin/env python3
"""Pure controls for the automatic-GC reference/candidate public-object port.

No external Source, compiler, LLVM, observer, program or Python target is
started. The controls replace the bounded process transport with an in-memory
seam and call the real ``run_case`` branches, so argv, raw records and trace
allowlists are exercised without executing anything.

They are not GC evidence and do not claim the nine real fixtures pass.
"""
from __future__ import annotations

import argparse
import contextlib
import hashlib
import importlib.util
import io
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]

EXTERNAL_ATTEMPTS = []


def _blocked(name):
    def deny(*args, **kwargs):
        EXTERNAL_ATTEMPTS.append(name)
        raise AssertionError("external execution forbidden: " + name)

    return deny


for _name in ("Popen", "run", "call", "check_call", "check_output",
              "getoutput", "getstatusoutput"):
    if hasattr(subprocess, _name):
        setattr(subprocess, _name, _blocked("subprocess." + _name))
for _name in list(vars(os)):
    if (_name in ("system", "popen", "fork", "forkpty", "posix_spawn",
                  "posix_spawnp")
            or _name.startswith(("exec", "spawn"))):
        if callable(getattr(os, _name)):
            setattr(os, _name, _blocked("os." + _name))
for _name in ("socket", "create_connection", "socketpair", "fromfd",
              "create_server"):
    if hasattr(socket, _name):
        setattr(socket, _name, _blocked("socket." + _name))


def tearDownModule():
    if EXTERNAL_ATTEMPTS:
        raise AssertionError(
            "external execution was attempted: " + repr(EXTERNAL_ATTEMPTS))


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


RUNNER = load(HERE / "run_automatic_gc.py", "agc_public_controls_runner")
SELFTEST = load(HERE / "selftest_checks.py", "agc_public_controls_selftest")
CATALOG_DOCUMENT = json.loads((HERE / "cases.json").read_text())
CATALOG = CATALOG_DOCUMENT["cases"]
CASE_BY_ID = {case["id"]: case for case in CATALOG}

ORIGINAL_CASES_SHA = (
    "a8e2cb9c55e01a37d517cae7ae08e5301e58397dbf38efe3e26eaaa7a0c30f61"
)
ORIGINAL_OBSERVER_SHA = (
    "122b601464ad0dcd3b2d818fa89e6a0244c84580320e10dbb4ccd56c04d65e54"
)
ORIGINAL_COMMAND_PLAN = {
    "reference": {"runs": 18, "commands_per_run": 7, "case_commands": 126},
    "candidate": {"runs": 18, "commands_per_run": 6, "case_commands": 108},
}
ORIGINAL_FIXTURES = {
    "fixtures/control-empty/lang.toml": "ce4510889a8102a9f8996fe3d87b5ac34e3537b0ecb54f66df541e33151e58fb",
    "fixtures/control-empty/src/main.wz": "b3f6b27e8aa393aaf20c0a91a659dc3e5944109e41ea4655c57842f522bf3e69",
    "fixtures/control-acyclic/lang.toml": "ce4510889a8102a9f8996fe3d87b5ac34e3537b0ecb54f66df541e33151e58fb",
    "fixtures/control-acyclic/src/main.wz": "e70b8dc15a98539aa24bddade87b57ff56fff2128f5d8b730f32cfdb6ba56ada",
    "fixtures/self-cycle/lang.toml": "ce4510889a8102a9f8996fe3d87b5ac34e3537b0ecb54f66df541e33151e58fb",
    "fixtures/self-cycle/src/main.wz": "ff4c0455da9e3bc7653da15438761c6f31780e843dfcc732a0a0f8c20a1921de",
    "fixtures/duplicate-edges/lang.toml": "ce4510889a8102a9f8996fe3d87b5ac34e3537b0ecb54f66df541e33151e58fb",
    "fixtures/duplicate-edges/src/main.wz": "496f54874301dd98c7ced0b0e2b823bccd7c184874d15b3a4e740d61b84e661e",
    "fixtures/two-cycle/lang.toml": "ce4510889a8102a9f8996fe3d87b5ac34e3537b0ecb54f66df541e33151e58fb",
    "fixtures/two-cycle/src/main.wz": "1c296e2df6f877f048703cfc947312c4ff6832245a69126cfdb797450ec7a5f0",
    "fixtures/enum-nested-cycle/lang.toml": "ce4510889a8102a9f8996fe3d87b5ac34e3537b0ecb54f66df541e33151e58fb",
    "fixtures/enum-nested-cycle/src/main.wz": "0d4dd222bb81690093b15526e70c8906692026142ed433d81b19b394e3869d5f",
    "fixtures/dead-cycle-live/lang.toml": "ce4510889a8102a9f8996fe3d87b5ac34e3537b0ecb54f66df541e33151e58fb",
    "fixtures/dead-cycle-live/src/main.wz": "0168a6d0d970b3c960ef81506fd7c0c0e43038b42cde3f73ada7cb8c6c7f977e",
    "fixtures/loop-continue/lang.toml": "ce4510889a8102a9f8996fe3d87b5ac34e3537b0ecb54f66df541e33151e58fb",
    "fixtures/loop-continue/src/main.wz": "9888f69dbb195495da62561b39cd29f2fac66e60038ff9ca7885303140b22553",
    "fixtures/typed-throw-defer/lang.toml": "ce4510889a8102a9f8996fe3d87b5ac34e3537b0ecb54f66df541e33151e58fb",
    "fixtures/typed-throw-defer/src/main.wz": "9507c9d8b5e20140afc2986b6a36d4d2050c25fc2ad964a2323c75300ca97359",
}


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def digest(path):
    return sha256_bytes(Path(path).read_bytes())


def result(exit_code=0, stdout="", stderr="", timed_out=False,
           cleanup_complete=True, abandoned_pipes=False):
    if isinstance(stdout, str):
        stdout = stdout.encode()
    if isinstance(stderr, str):
        stderr = stderr.encode()
    return {
        "exit": exit_code,
        "stdout": stdout,
        "stderr": stderr,
        "timed_out": timed_out,
        "cleanup_complete": cleanup_complete,
        "abandoned_pipes": abandoned_pipes,
    }


class FakeCorpus:
    """In-memory stand-in for selfhost/corpus.py's bounded transport."""

    class TraceError(Exception):
        pass

    def __init__(self, layout, scenario, case, observer_stream):
        self.layout = layout
        self.scenario = scenario
        self.case = case
        self.observer_stream = observer_stream
        self.commands = []
        self.timeouts = []
        self.version_flags = False
        self.process_root = None
        self.process_counts = []
        self.on_cli_build = None

    def _trace(self, path, extra=None):
        path.parent.mkdir(parents=True, exist_ok=True)
        lines = []
        for executable in extra or []:
            lines.append(
                f'execve("{executable}", ["{executable}"], 0x0) = 0\n'
            )
        path.write_text("".join(lines), encoding="utf-8")

    def audit_trace(self, path, allowed):
        text = Path(path).read_text(encoding="utf-8")
        observed = set(re.findall(r'execve\("([^"]+)"', text))
        unexpected = sorted(observed - set(allowed))
        if unexpected:
            raise self.TraceError(
                "unexpected compiler subprocesses: " + ", ".join(unexpected)
            )
        return {
            "status": "ok",
            "observed": sorted(observed),
            "allowed": sorted(allowed),
        }

    def _strip_trace(self, argv):
        if "--seccomp-bpf" in argv:
            index = argv.index("-o")
            return Path(argv[index + 1]), argv[index + 2:]
        return None, argv

    def _run_process_impl(self, argv, cwd, env, timeout):
        argv = [str(part) for part in argv]
        trace_path, actual = self._strip_trace(argv)
        if self.process_root is not None:
            self.process_counts.append(
                len(list(Path(self.process_root).rglob("process/*.json")))
            )
        self.commands.append(actual)
        self.timeouts.append(timeout)
        if trace_path is not None and self.scenario != "missing_trace":
            observed = [actual[0]]
            if actual[0] == self.layout["cli"]:
                toolchain = Path(self.layout["cli"]).parent / "toolchain"
                observed.extend(
                    str(toolchain / name)
                    for name in ("clang", "clang++", "ld.lld", "opt", "llc",
                                 "llvm-nm", "llvm-readelf")
                )
            if (actual[0] == str(Path(self.layout["clang"]).resolve())
                    and actual[1:] != ["--version"]):
                observed.append(str(Path(self.layout["lld"]).resolve()))
            if self.scenario == "unexpected_exec":
                observed.append("/bin/sh")
            self._trace(trace_path, observed)

        executable = actual[0]
        name = Path(executable).name
        if executable == self.layout["cli"]:
            out_dir = Path(actual[actual.index("--out-dir") + 1])
            out_dir.mkdir(parents=True, exist_ok=True)
            (out_dir / "program").write_bytes(
                f"program:{self.case['id']}:{self.case.get('opt', 0)}".encode()
            )
            if self.scenario != "missing_object" and self.scenario != "cli_fail":
                (out_dir / "program.o").write_bytes(
                    f"public-object:{self.case['id']}".encode()
                )
            if self.on_cli_build is not None:
                self.on_cli_build()
            if self.scenario == "cli_fail":
                return result(exit_code=1, stdout='{"status":"error"}\n')
            if self.scenario == "build_timeout":
                return result(exit_code=None, timed_out=True)
            if self.scenario == "wrong_flags":
                return result(exit_code=0, timed_out=0,
                              stdout='{"status":"ok"}\n')
            if self.scenario == "nan_build":
                return result(stdout='{"status":"ok","extra":NaN}\n')
            if self.scenario == "overflow_build":
                return result(stdout='{"status":"ok","extra":1e999}\n')
            return result(stdout='{"status":"ok","diagnostics":[]}\n')
        if executable == self.layout["readelf"]:
            if self.scenario == "static_elf_fail":
                return result(stdout="Program Headers:\n INTERP 0x1\n")
            return result(stdout="Program Headers:\n Type EXEC\n")
        if name == "program":
            stdout = "noise\n" if self.scenario == "program_output" else ""
            return result(exit_code=self.case["main_return"], stdout=stdout)
        if executable == self.layout["helper"]:
            object_path = Path(actual[actual.index("--out-object") + 1])
            llvm_path = Path(actual[actual.index("--out-llvm") + 1])
            object_path.parent.mkdir(parents=True, exist_ok=True)
            object_path.write_bytes(b"helper-object")
            llvm_path.write_text("; helper llvm\n", encoding="utf-8")
            return result(stdout='{"status":"passed"}\n')
        if executable == str(Path(self.layout["clang"]).resolve()):
            if actual[1:] == ["--version"]:
                if self.version_flags:
                    return result(
                        exit_code=False,
                        timed_out=False,
                        cleanup_complete=False,
                        abandoned_pipes=True,
                        stdout="clang version 22.1.8\n",
                    )
                return result(stdout="clang version 22.1.8\n")
            output = Path(actual[actual.index("-o") + 1])
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(b"observer")
            return result()
        if name == "observe":
            if self.scenario == "observer_exit":
                return result(exit_code=1)
            stream = self.observer_stream
            if self.scenario == "observer_leak":
                stream = leaked_self_cycle()
            return result(stderr=stream)
        raise AssertionError("unexpected fake transport command: " + repr(actual))


def leaked_self_cycle():
    lines = SELFTEST.collected_small("self-cycle").splitlines()
    return "\n".join(
        line for line in lines
        if not line.startswith("VEC:3:0x00000000000000bb:0")
        and line != "VEC:5:0x00000000000000bb:0"
        and line != "FREE:0x00000000000000bb"
        and line != "VEC:3:0x00000000000000bb:1"
    ) + "\n"


def make_layout(base):
    layout = {}
    for name in ("cli", "helper", "clang", "readelf", "lld", "strace"):
        path = base / name
        path.write_bytes(("fake-" + name).encode())
        layout[name] = str(path)
    runtime_dir = base / "runtime"
    runtime_dir.mkdir(parents=True)
    runtime = runtime_dir / "liblang_native_runtime.a"
    runtime.write_bytes(b"fake-runtime")
    layout["runtime"] = str(runtime)
    (base / "stdlib").mkdir()
    (base / "stdlib/test.wz").write_text("fake stdlib", encoding="utf-8")
    # Reference tool pinning requires the supplied helper path to be embedded
    # in the CLI bytes; candidate mode never reads this marker.
    Path(layout["cli"]).write_bytes(
        b"fake-cli:" + layout["helper"].encode()
    )
    return layout


def make_args(base, layout, role):
    return argparse.Namespace(
        cli=Path(layout["cli"]),
        helper=Path(layout["helper"]),
        runtime=Path(layout["runtime"]),
        clang=Path(layout["clang"]),
        readelf=Path(layout["readelf"]),
        lld=Path(layout["lld"]),
        strace=Path(layout["strace"]),
        libxml2_lib=None,
        root=ROOT,
        out=base / "out",
        timeout=60,
        opt=[0],
        case=None,
        baseline_only=False,
        raw_output_limit=65536,
        compact_out=None,
        expect_cli_sha256=None,
        expect_helper_sha256=None,
        expect_runtime_sha256=None,
        role=role,
    )


class PublicPortControls(unittest.TestCase):
    def run_fake(self, role="candidate", scenario="ok",
                 case_id="control-acyclic", observer_stream=None,
                 raw_output_limit=65536, mutate_layout=None,
                 track_process=False):
        temp = Path(tempfile.mkdtemp(prefix="agc-public-controls-"))
        layout = make_layout(temp)
        if mutate_layout is not None:
            mutate_layout(temp, layout)
        out = temp / "out"
        RUNNER.prepare_out(out)
        args = make_args(temp, layout, role)
        args.raw_output_limit = raw_output_limit
        case = dict(CASE_BY_ID[case_id])
        case["opt"] = 0
        if observer_stream is None:
            observer_stream = SELFTEST.collected_small(case_id)
        fake = FakeCorpus(layout, scenario, case, observer_stream)
        if track_process:
            fake.process_root = out
        old = RUNNER.CORPUS
        RUNNER.CORPUS = fake
        try:
            report = {"cases": []}
            row = RUNNER.run_case(case, 0, args, {}, report)
        finally:
            RUNNER.CORPUS = old
        return row, fake, out, temp

    def run_main_fake(self, temp, layout, role="candidate", fake=None,
                      extra_argv=None, root=None, out=None):
        out = Path(out or (temp / "out"))
        case = dict(CASE_BY_ID["control-acyclic"])
        case["opt"] = 0
        if fake is None:
            fake = FakeCorpus(layout, "ok", case,
                              SELFTEST.collected_small("control-acyclic"))
        old_corpus = RUNNER.CORPUS
        old_argv = sys.argv
        argv = [
            "run_automatic_gc.py",
            "--cli", layout["cli"],
            "--helper", layout["helper"],
            "--runtime", layout["runtime"],
            "--clang", layout["clang"],
            "--readelf", layout["readelf"],
            "--out", str(out),
            "--case", "control-acyclic",
            "--opt", "0",
        ]
        if role is not None:
            argv += ["--role", role]
        if role == "candidate":
            argv += ["--strace", layout["strace"], "--lld", layout["lld"]]
        if root is not None:
            argv += ["--root", str(root)]
        if extra_argv:
            argv += extra_argv
        RUNNER.CORPUS = fake
        try:
            with patch.object(RUNNER, "set_corpus", lambda value: fake):
                sys.argv = argv
                with contextlib.redirect_stdout(io.StringIO()):
                    code = RUNNER.main()
        finally:
            sys.argv = old_argv
            RUNNER.CORPUS = old_corpus
        summary = None
        actual_out = None
        for path in Path(out).rglob("summary.json"):
            try:
                candidate = json.loads(path.read_text())
            except (OSError, ValueError):
                continue
            if candidate.get("operation_id"):
                summary = candidate
                actual_out = Path(candidate["output_dir"])
                break
        return code, summary, actual_out, fake

    def test_catalog_fixtures_and_budget_unchanged(self):
        self.assertEqual(digest(HERE / "cases.json"), ORIGINAL_CASES_SHA)
        self.assertEqual(digest(HERE / "observe_gc.cpp"), ORIGINAL_OBSERVER_SHA)
        self.assertEqual([case["id"] for case in CATALOG], [
            "control-empty", "control-acyclic", "self-cycle",
            "duplicate-edges", "two-cycle", "enum-nested-cycle",
            "dead-cycle-live", "loop-continue", "typed-throw-defer"])
        actual_fixtures = {
            str(path.relative_to(HERE)): digest(path)
            for path in sorted((HERE / "fixtures").rglob("*"))
            if path.is_file()
        }
        self.assertEqual(actual_fixtures, ORIGINAL_FIXTURES)
        for case in CATALOG:
            self.assertEqual(case["baseline"]["header_bytes"] in (None, 80), True)
            self.assertIn("target", case)
        self.assertEqual(CASE_BY_ID["dead-cycle-live"]["timeout_seconds"], 120)
        self.assertEqual(CASE_BY_ID["loop-continue"]["timeout_seconds"], 120)
        self.assertEqual(CASE_BY_ID["dead-cycle-live"]["payload_budget_bytes"],
                         2 * 1024 * 1024)
        self.assertEqual(CASE_BY_ID["loop-continue"]["payload_budget_bytes"],
                         2 * 1024 * 1024)
        self.assertEqual(
            set(RUNNER.CHECKS),
            {"cycle_reclaimed", "pressure_budget", "root_values_preserved",
             "dead_cycle_live_edge_release"})

    def test_command_plan_is_exact_for_both_roles(self):
        for role, expected in ORIGINAL_COMMAND_PLAN.items():
            plan = RUNNER.planned_commands(CATALOG, [0, 2], role)
            self.assertEqual(plan["runs"], expected["runs"])
            self.assertEqual(plan["commands_per_run"],
                             expected["commands_per_run"])
            self.assertEqual(plan["case_commands_expected"],
                             expected["case_commands"])
            self.assertEqual(plan["clang_version_commands"], 1)
            self.assertEqual(plan["total_expected"],
                             expected["case_commands"] + 1)

    def test_all_nine_cases_keep_exact_role_command_counts(self):
        for role, per_run in (("reference", 7), ("candidate", 6)):
            temp = Path(tempfile.mkdtemp(prefix="agc-public-all-"))
            layout = make_layout(temp)
            out = temp / "out"
            RUNNER.prepare_out(out)
            args = make_args(temp, layout, role)
            fake = FakeCorpus(layout, "ok", dict(CATALOG[0]), "")
            old = RUNNER.CORPUS
            RUNNER.CORPUS = fake
            rows = []
            try:
                report = {"cases": []}
                for case in CATALOG:
                    for opt in (0, 2):
                        current = dict(case)
                        current["opt"] = opt
                        fake.case = current
                        fake.observer_stream = (
                            "" if case["id"] in ("dead-cycle-live",
                                                 "loop-continue")
                            else SELFTEST.collected_small(case["id"])
                        )
                        rows.append(RUNNER.run_case(
                            current, opt, args, {}, report))
            finally:
                RUNNER.CORPUS = old
            self.assertEqual(len(rows), 18)
            self.assertEqual(sum(len(row["commands"]) for row in rows),
                             18 * per_run)
            for row in rows:
                self.assertNotEqual(row["status"], "harness_error", row)
                self.assertEqual(len(row["commands"]), per_run)
                self.assertEqual(row["commands"][0]["name"], "cli-build")
                self.assertEqual(row["commands"][-1]["name"], "observe-execute")
                self.assertEqual(len(row["source_hashes"]), 2)
                if role == "candidate":
                    self.assertIn("cli_program_object", row)
                    self.assertNotIn("helper-emit-llvm",
                                     [command["name"] for command in row["commands"]])
                    self.assertTrue(all(command["trace"]["status"] == "ok"
                                        for command in row["commands"]))
                else:
                    self.assertNotIn("cli_program_object", row)
                    self.assertEqual(row["commands"][3]["name"],
                                     "helper-emit-llvm")

    def test_per_case_timeout_budget_is_unchanged(self):
        row, fake, out, temp = self.run_fake()
        self.assertTrue(all(value == 60 for value in fake.timeouts))
        row, fake, out, temp = self.run_fake(case_id="dead-cycle-live",
                                             observer_stream="")
        self.assertEqual(row["status"], "target_failed")
        self.assertTrue(all(value == 120 for value in fake.timeouts))
        self.assertEqual(len(fake.timeouts), 6)

    def test_candidate_uses_public_object_and_never_helper(self):
        row, fake, out, temp = self.run_fake()
        self.assertEqual(row["status"], "target_passed", row)
        self.assertEqual(row["role"], "candidate")
        self.assertEqual(
            [command["name"] for command in row["commands"]],
            ["cli-build", "readelf-real-program", "real-execute",
             "observer-link", "readelf-observer", "observe-execute"])
        program_object = out / "control-acyclic/O0/cli/program.o"
        self.assertEqual(row["cli_program_object"]["path"], str(program_object))
        self.assertEqual(row["cli_program_object"]["sha256"],
                         digest(program_object))
        link = row["commands"][3]
        self.assertIn(str(program_object), link["argv"])
        self.assertIn(str(HERE / "observe_gc.cpp"), link["argv"])
        self.assertIn("--ld-path=" + str(Path(fake.layout["lld"]).resolve()),
                      link["argv"])
        self.assertNotIn(str(fake.layout["helper"]), link["argv"])
        for command in fake.commands:
            self.assertNotIn(fake.layout["helper"], command, command)
        self.assertNotIn(str(fake.layout["helper"]),
                         json.dumps(row["commands"]))
        self.assertEqual(row["observer_link"]["program_object_sha256"],
                         digest(program_object))
        self.assertEqual(row["metrics"]["vec_allocations"], 4)
        self.assertEqual(row["metrics"]["vec_frees"], 4)
        for command in row["commands"]:
            self.assertEqual(command["trace"]["status"], "ok")
            self.assertTrue(Path(command["stdout_path"]).is_file())
            self.assertTrue(Path(command["stderr_path"]).is_file())
            self.assertTrue(Path(command["trace_path"]).is_file())
        self.assertTrue((out / "control-acyclic/O0/traces").is_dir())

    def test_large_observer_stream_uses_saved_raw_for_parsing(self):
        row, fake, out, temp = self.run_fake(raw_output_limit=1)
        self.assertEqual(row["status"], "target_passed", row)
        observe_command = row["commands"][-1]
        self.assertEqual(observe_command["name"], "observe-execute")
        self.assertTrue(observe_command["stderr"].startswith("<stored:"))
        self.assertTrue(Path(observe_command["stderr_file"]).is_file())
        self.assertEqual(row["metrics"]["vec_allocations"], 4)
        self.assertEqual(row["metrics"]["vec_frees"], 4)

    def test_candidate_cli_failure_stops_before_observer(self):
        row, fake, out, temp = self.run_fake(scenario="cli_fail")
        self.assertEqual(row["status"], "harness_error")
        self.assertEqual(row["error_kind"], "build")
        self.assertEqual(len(row["commands"]), 1)
        self.assertEqual([command["name"] for command in row["commands"]],
                         ["cli-build"])
        self.assertEqual(len(fake.commands), 1)
        self.assertTrue(Path(row["commands"][0]["stdout_path"]).is_file())
        self.assertTrue(Path(row["commands"][0]["trace_path"]).is_file())
        self.assertFalse((out / "control-acyclic/O0/observe").exists())

    def test_candidate_missing_object_stops_before_readelf(self):
        row, fake, out, temp = self.run_fake(scenario="missing_object")
        self.assertEqual(row["status"], "harness_error")
        self.assertEqual(row["error_kind"], "build")
        self.assertEqual([command["name"] for command in row["commands"]],
                         ["cli-build"])
        self.assertIn("program.o", row["failure"])
        self.assertNotIn("cli_program_object", row)

    def test_wrong_process_flags_are_rejected(self):
        base = {
            "status": "exited",
            "exit": 0,
            "timed_out": False,
            "cleanup_complete": True,
            "abandoned_pipes": False,
            "trace_required": False,
            "trace": {"status": "not_required"},
        }
        self.assertTrue(RUNNER.process_clean(base))
        for key, values in {
            "exit": [False, True, 0.0, None],
            "timed_out": [0, 1, None, True],
            "cleanup_complete": [0, 1, None, False],
            "abandoned_pipes": [0, 1, None, True],
        }.items():
            for value in values:
                with self.subTest(key=key, value=value):
                    self.assertFalse(
                        RUNNER.process_clean(dict(base, **{key: value})))
            missing = dict(base)
            del missing[key]
            self.assertFalse(RUNNER.process_clean(missing))
        row, fake, out, temp = self.run_fake(scenario="wrong_flags")
        self.assertEqual(row["status"], "harness_error")
        self.assertEqual(row["error_kind"], "harness")
        self.assertEqual(len(row["commands"]), 1)

    def test_unexpected_exec_is_rejected(self):
        row, fake, out, temp = self.run_fake(scenario="unexpected_exec")
        self.assertEqual(row["status"], "harness_error")
        self.assertEqual(row["error_kind"], "harness")
        self.assertEqual(len(row["commands"]), 1)
        self.assertEqual(row["commands"][0]["trace"]["status"], "violation")
        self.assertIn("/bin/sh", row["commands"][0]["trace"]["error"])
        self.assertIn("exec trace", row["failure"])

    def test_missing_trace_is_rejected(self):
        row, fake, out, temp = self.run_fake(scenario="missing_trace")
        self.assertEqual(row["status"], "harness_error")
        self.assertEqual(row["error_kind"], "harness")
        self.assertEqual(len(row["commands"]), 1)
        self.assertEqual(row["commands"][0]["trace"]["status"], "violation")
        self.assertIn("trace", row["failure"])

    def test_static_elf_program_output_and_observer_failures_stop(self):
        row, fake, out, temp = self.run_fake(scenario="static_elf_fail")
        self.assertEqual(row["status"], "harness_error")
        self.assertEqual(row["error_kind"], "static_elf")
        self.assertEqual([command["name"] for command in row["commands"]],
                         ["cli-build", "readelf-real-program"])

        row, fake, out, temp = self.run_fake(scenario="program_output")
        self.assertEqual(row["status"], "behavior_error")
        self.assertEqual([command["name"] for command in row["commands"]],
                         ["cli-build", "readelf-real-program", "real-execute"])

        row, fake, out, temp = self.run_fake(scenario="observer_exit")
        self.assertEqual(row["status"], "behavior_error")
        self.assertEqual(len(row["commands"]), 6)

        row, fake, out, temp = self.run_fake(scenario="observer_leak",
                                             case_id="self-cycle")
        self.assertEqual(row["status"], "target_failed")
        self.assertTrue(row["target"]["failures"])

    def test_reference_keeps_helper_llvm_link(self):
        row, fake, out, temp = self.run_fake(role="reference")
        self.assertEqual(row["status"], "target_passed", row)
        self.assertEqual(
            [command["name"] for command in row["commands"]],
            ["cli-build", "readelf-real-program", "real-execute",
             "helper-emit-llvm", "observer-link", "readelf-observer",
             "observe-execute"])
        helper_command = row["commands"][3]
        self.assertEqual(helper_command["argv"][0], fake.layout["helper"])
        link = row["commands"][4]
        self.assertIn(str(out / "control-acyclic/O0/observe/program.ll"),
                      link["argv"])
        self.assertNotIn("--ld-path=" + str(Path(fake.layout["lld"]).resolve()),
                         link["argv"])
        self.assertNotIn("cli_program_object", row)
        self.assertEqual(fake.commands.count([str(fake.layout["helper"]),
                          "--project", str(HERE / "fixtures/control-acyclic/lang.toml"),
                          "--out-object", str(out / "control-acyclic/O0/observe/program.o"),
                          "--out-llvm", str(out / "control-acyclic/O0/observe/program.ll"),
                          "--opt", "0"]), 1)
        self.assertFalse(any(command.get("trace_path")
                             and Path(command["trace_path"]).exists()
                             for command in row["commands"]))

    def test_role_argument_rules(self):
        base = ["--cli", "/tmp/cli", "--runtime", "/tmp/runtime",
                "--clang", "/tmp/clang", "--readelf", "/tmp/readelf",
                "--out", "/tmp/out"]
        _, args = RUNNER.parse_args(base + ["--helper", "/tmp/helper"])
        self.assertEqual(args.role, "reference")
        self.assertEqual(args.opt, [0, 2])

        _, args = RUNNER.parse_args(base + [
            "--role", "candidate", "--strace", "/tmp/strace",
            "--lld", "/tmp/ld.lld"])
        self.assertEqual(args.role, "candidate")
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                RUNNER.parse_args(base + [
                    "--role", "candidate", "--strace", "/tmp/strace",
                    "--lld", "/tmp/ld.lld",
                    "--expect-helper-sha256", "deadbeef"])
            with self.assertRaises(SystemExit):
                RUNNER.parse_args(base + [
                    "--role", "candidate", "--lld", "/tmp/ld.lld"])
            with self.assertRaises(SystemExit):
                RUNNER.parse_args(base + [
                    "--role", "candidate", "--strace", "/tmp/strace"])
            with self.assertRaises(SystemExit):
                RUNNER.parse_args(base + ["--role", "reference"])

    def test_candidate_main_writes_serializable_plan(self):
        temp = Path(tempfile.mkdtemp(prefix="agc-public-main-"))
        layout = make_layout(temp)
        out = temp / "out"
        case = dict(CASE_BY_ID["control-acyclic"])
        case["opt"] = 0
        fake = FakeCorpus(layout, "ok", case,
                          SELFTEST.collected_small("control-acyclic"))
        old_corpus = RUNNER.CORPUS
        old_argv = sys.argv
        argv = [
            "run_automatic_gc.py",
            "--cli", layout["cli"],
            "--helper", layout["helper"],
            "--runtime", layout["runtime"],
            "--clang", layout["clang"],
            "--readelf", layout["readelf"],
            "--role", "candidate",
            "--strace", layout["strace"],
            "--lld", layout["lld"],
            "--out", str(out),
            "--case", "control-acyclic",
            "--opt", "0",
            "--compact-out", str(temp / "compact.json"),
        ]
        RUNNER.CORPUS = fake
        try:
            with patch.object(RUNNER, "set_corpus", lambda root: fake):
                sys.argv = argv
                with contextlib.redirect_stdout(io.StringIO()):
                    code = RUNNER.main()
        finally:
            sys.argv = old_argv
            RUNNER.CORPUS = old_corpus
        self.assertEqual(code, 0)
        summary = json.loads((out / "summary.json").read_text())
        self.assertEqual(summary["role"], "candidate")
        self.assertEqual(summary["command_plan"]["case_commands_expected"], 6)
        self.assertEqual(summary["command_plan"]["case_commands_actual"], 6)
        self.assertEqual(summary["command_plan"]["clang_version_commands_actual"], 1)
        self.assertEqual(summary["command_plan"]["total_actual"], 7)
        self.assertTrue(summary["command_plan"]["complete"])
        self.assertEqual(summary["cases"][0]["status"], "target_passed")
        self.assertNotIn("helper", summary["tool_pin"]["paths"])
        self.assertNotIn("helper", summary["tool_pin"]["expected"])
        compact = json.loads((temp / "compact.json").read_text())
        self.assertEqual(compact["role"], "candidate")
        self.assertEqual(compact["command_plan"]["runs"], 1)

    def test_reference_main_default_role_uses_helper(self):
        temp = Path(tempfile.mkdtemp(prefix="agc-public-reference-main-"))
        layout = make_layout(temp)
        out = temp / "out"
        case = dict(CASE_BY_ID["control-acyclic"])
        case["opt"] = 0
        fake = FakeCorpus(layout, "ok", case,
                          SELFTEST.collected_small("control-acyclic"))
        old_corpus = RUNNER.CORPUS
        old_argv = sys.argv
        argv = [
            "run_automatic_gc.py",
            "--cli", layout["cli"],
            "--helper", layout["helper"],
            "--runtime", layout["runtime"],
            "--clang", layout["clang"],
            "--readelf", layout["readelf"],
            "--out", str(out),
            "--case", "control-acyclic",
            "--opt", "0",
            "--compact-out", str(temp / "compact.json"),
        ]
        RUNNER.CORPUS = fake
        try:
            with patch.object(RUNNER, "set_corpus", lambda root: fake):
                sys.argv = argv
                with contextlib.redirect_stdout(io.StringIO()):
                    code = RUNNER.main()
        finally:
            sys.argv = old_argv
            RUNNER.CORPUS = old_corpus
        self.assertEqual(code, 0)
        summary = json.loads((out / "summary.json").read_text())
        self.assertEqual(summary["role"], "reference")
        self.assertEqual(summary["command_plan"]["commands_per_run"], 7)
        self.assertEqual(summary["command_plan"]["case_commands_expected"], 7)
        self.assertEqual(summary["command_plan"]["total_actual"], 8)
        self.assertTrue(summary["tool_pin"]["helper_used"])
        self.assertTrue(summary["tool_pin"]["helper_path_embedded_in_cli"])
        self.assertIn("helper", summary["tool_pin"]["paths"])
        self.assertEqual(
            [command["name"] for command in summary["cases"][0]["commands"]][3],
            "helper-emit-llvm")

    def test_candidate_output_directory_is_not_overwritten(self):
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp) / "out"
            RUNNER.prepare_out(out)
            sentinel = out / "summary.json"
            sentinel.write_text("old evidence", encoding="utf-8")
            sentinel.chmod(0o640)
            with self.assertRaises(RuntimeError):
                RUNNER.prepare_out(out)
            before = sorted(path.name for path in out.iterdir())
            with contextlib.redirect_stderr(io.StringIO()):
                with patch.object(sys, "argv", [
                        "run_automatic_gc.py", "--cli", "/tmp/cli",
                        "--helper", "/tmp/helper", "--runtime", "/tmp/runtime",
                        "--clang", "/tmp/clang", "--readelf", "/tmp/readelf",
                        "--role", "candidate", "--strace", "/tmp/strace",
                        "--lld", "/tmp/lld", "--out", str(out)]):
                    with self.assertRaises(SystemExit) as raised:
                        RUNNER.main()
            self.assertEqual(raised.exception.code, 2)
            self.assertEqual(sentinel.read_text(encoding="utf-8"),
                             "old evidence")
            self.assertEqual(sentinel.stat().st_mode & 0o777, 0o640)
            self.assertEqual(sorted(path.name for path in out.iterdir()),
                             before)
            self.assertFalse(any(path.name.startswith("run-")
                                 for path in out.iterdir()))

    def test_reference_output_directory_uses_unique_child(self):
        temp = Path(tempfile.mkdtemp(prefix="agc-public-legacy-out-"))
        layout = make_layout(temp)
        out = temp / "out"
        out.mkdir()
        sentinel = out / "summary.json"
        sentinel.write_text("old evidence", encoding="utf-8")
        sentinel.chmod(0o640)
        old_compact = out / "compact.json"
        old_compact.write_text("old compact", encoding="utf-8")
        old_compact.chmod(0o600)
        code, summary, actual_out, fake = self.run_main_fake(
            temp, layout, role=None, out=out,
            extra_argv=["--compact-out", str(old_compact)])
        self.assertEqual(code, 0, summary)
        self.assertEqual(summary["requested_out"], str(out))
        self.assertNotEqual(summary["output_dir"], str(out))
        self.assertEqual(Path(summary["output_dir"]).parent, out)
        self.assertTrue((Path(summary["output_dir"]) / "summary.json").is_file())
        self.assertEqual(actual_out, Path(summary["output_dir"]))
        self.assertEqual(sentinel.read_text(encoding="utf-8"), "old evidence")
        self.assertEqual(sentinel.stat().st_mode & 0o777, 0o640)
        self.assertEqual(old_compact.read_text(encoding="utf-8"),
                         "old compact")
        self.assertEqual(old_compact.stat().st_mode & 0o777, 0o600)
        self.assertTrue(any(path.name.startswith("run-")
                            for path in out.iterdir()))
        new_compact = Path(summary["compact_out"])
        self.assertEqual(new_compact.parent, Path(summary["output_dir"]))
        self.assertTrue(new_compact.is_file())
        self.assertEqual(summary["compact_out_requested"],
                         str(old_compact))

    def test_candidate_lld_alias_is_preserved(self):
        def mutate(temp, layout):
            target = temp / "lld-generic"
            target.write_bytes(b"generic lld")
            link = Path(layout["lld"])
            link.unlink()
            link.symlink_to(target)

        row, fake, out, temp = self.run_fake(mutate_layout=mutate)
        self.assertEqual(row["status"], "target_passed", row)
        link = next(command for command in row["commands"]
                    if command["name"] == "observer-link")
        alias = str(Path(fake.layout["lld"]).absolute())
        canonical = str(Path(fake.layout["lld"]).resolve())
        self.assertIn("--ld-path=" + alias, link["argv"])
        self.assertNotIn("--ld-path=" + canonical, link["argv"])
        self.assertEqual(link["trace"]["status"], "ok")

    def test_version_bad_flags_are_rejected_and_recorded(self):
        temp = Path(tempfile.mkdtemp(prefix="agc-public-version-"))
        layout = make_layout(temp)
        case = dict(CASE_BY_ID["control-acyclic"])
        case["opt"] = 0
        fake = FakeCorpus(layout, "ok", case,
                          SELFTEST.collected_small("control-acyclic"))
        fake.version_flags = True
        code, summary, actual_out, fake = self.run_main_fake(
            temp, layout, role="candidate", fake=fake)
        self.assertEqual(code, 2)
        self.assertEqual(summary["status"], "inconclusive")
        version = summary["tool_pin"]["clang_version"]
        self.assertIs(version["exit"], False)
        self.assertIs(version["timed_out"], False)
        self.assertIs(version["cleanup_complete"], False)
        self.assertIs(version["abandoned_pipes"], True)
        process = Path(summary["output_dir"]) / "tool/process/clang-version.json"
        self.assertTrue(process.is_file())
        saved = json.loads(process.read_text())
        self.assertIs(saved["exit"], False)
        self.assertTrue((Path(summary["output_dir"]) /
                         "tool/raw/clang-version.stdout").is_file())

    def test_nonfinite_build_and_report_json_are_rejected(self):
        for scenario in ("nan_build", "overflow_build"):
            with self.subTest(scenario=scenario):
                row, fake, out, temp = self.run_fake(scenario=scenario)
                self.assertEqual(row["status"], "harness_error")
                self.assertEqual(row["error_kind"], "build")
                self.assertIn("finite", row["failure"])
                process = out / "control-acyclic/O0/process/cli-build.json"
                self.assertTrue(process.is_file())
                json.loads(process.read_text())
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "record.json"
            for value in (
                {"value": float("nan")},
                {"value": float("inf")},
                {"nested": [{"value": float("-inf")}]},
                {"value": 1e999},
            ):
                with self.subTest(value=value):
                    with self.assertRaises(ValueError):
                        RUNNER.write_json(path, value)
                    self.assertFalse(path.exists())
            for text in ('{"x":NaN}', '{"x":Infinity}',
                         '{"x":-Infinity}', '{"x":1e999}'):
                with self.subTest(text=text):
                    with self.assertRaises(ValueError):
                        RUNNER.strict_json_loads(text)

    def test_resource_change_makes_main_inconclusive(self):
        temp = Path(tempfile.mkdtemp(prefix="agc-public-resource-"))
        layout = make_layout(temp)
        fake_root = temp / "repo"
        (fake_root / "stdlib").mkdir(parents=True)
        (fake_root / "stdlib/test.wz").write_text("before", encoding="utf-8")
        vec = fake_root / "runtime/native/vec.hpp"
        vec.parent.mkdir(parents=True)
        vec.write_text("// fake vec header\n", encoding="utf-8")
        case = dict(CASE_BY_ID["control-acyclic"])
        case["opt"] = 0
        fake = FakeCorpus(layout, "ok", case,
                          SELFTEST.collected_small("control-acyclic"))
        fake.on_cli_build = lambda: (
            fake_root / "stdlib/test.wz"
        ).write_text("after", encoding="utf-8")
        code, summary, actual_out, fake = self.run_main_fake(
            temp, layout, role="candidate", fake=fake, root=fake_root)
        self.assertEqual(code, 2)
        self.assertEqual(summary["status"], "inconclusive")
        self.assertIn("root_stdlib", summary["inputs"]["changed"])
        self.assertFalse(summary["resources"]["stdlib_tree_unchanged"])
        for key in ("runtime_after", "observer_source_after",
                    "vec_header_after", "cli_runtime_after"):
            self.assertIn(key, summary["resources"])
        self.assertTrue((Path(summary["output_dir"]) /
                         "summary.json").is_file())

    def test_missing_required_input_is_not_silently_skipped(self):
        temp = Path(tempfile.mkdtemp(prefix="agc-public-missing-input-"))
        layout = make_layout(temp)
        fake_root = temp / "repo"
        (fake_root / "stdlib").mkdir(parents=True)
        (fake_root / "stdlib/test.wz").write_text("stdlib", encoding="utf-8")
        other_header = fake_root / "runtime/native/vec.hpp"
        other_header.parent.mkdir(parents=True)
        other_header.write_text("// separate --root header\n", encoding="utf-8")
        actual_header = RUNNER.HERE.parents[2] / "runtime/native/vec.hpp"
        original_is_file = Path.is_file

        def missing_actual_header(path):
            if Path(path).absolute() == actual_header.absolute():
                return False
            return original_is_file(path)

        with patch.object(Path, "is_file", missing_actual_header):
            code, summary, actual_out, fake = self.run_main_fake(
                temp, layout, role="candidate", root=fake_root)
        self.assertEqual(code, 2)
        self.assertEqual(summary["status"], "inconclusive")
        self.assertEqual(summary["resources"]["vec_header"]["path"],
                         str(actual_header))
        self.assertIsNone(summary["inputs"]["before"]["vec_header"])
        self.assertIn("vec_header",
                      summary["inputs"]["required_missing_before"])
        self.assertFalse(summary["inputs"]["unchanged"])
        self.assertEqual(other_header.read_text(encoding="utf-8"),
                         "// separate --root header\n")

    def test_process_json_is_saved_before_next_transport(self):
        row, fake, out, temp = self.run_fake(track_process=True)
        self.assertEqual(row["status"], "target_passed", row)
        self.assertEqual(fake.process_counts, [0, 1, 2, 3, 4, 5])
        process_files = sorted(out.rglob("process/*.json"))
        self.assertEqual(len(process_files), 6)
        for path in process_files:
            record = json.loads(path.read_text())
            self.assertNotIn(True, [key.startswith("_")
                                    for key in record])
            self.assertIs(type(record["exit"]), int)
            self.assertIs(record["timed_out"], False)
            self.assertIs(record["cleanup_complete"], True)
            self.assertIs(record["abandoned_pipes"], False)
            self.assertTrue(Path(record["stdout_path"]).is_file())
            self.assertTrue(Path(record["stderr_path"]).is_file())
            self.assertTrue(Path(record["trace_path"]).is_file())


if __name__ == "__main__":
    unittest.main(verbosity=2)
