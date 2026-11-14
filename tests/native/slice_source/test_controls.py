#!/usr/bin/env python3
"""Pure controls for run_slice_source.py; no external process is started.

These exercise the actual decision functions used by the runner: exact int
exit/flags, rejection classification, strict finite JSON, trace-loss handling,
four-file IR presence, weak observer evidence, and the non-empty-output guard.
They do not mirror the implementation with a second parser.
"""
from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]

# Install these guards before importing either module under test.  Any real
# spawn/exec/socket attempt is recorded and fails the attempted call.
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
    spec.loader.exec_module(module)
    return module


RUNNER = load(HERE / "run_slice_source.py", "slice_runner_controls")
BUILDER = load(HERE / "build_nativeobs_ed1.py", "slice_builder_controls")
CLEAN = {
    "status": "exited",
    "exit": 0,
    "timed_out": False,
    "cleanup_complete": True,
    "abandoned_pipes": False,
    "trace_required": False,
    "trace": {"status": "not_required"},
}


class Controls(unittest.TestCase):
    def test_exact_int_exit_and_flags(self):
        self.assertTrue(RUNNER.process_ok(dict(CLEAN)))
        for key, values in {
                "exit": [False, True, 0.0, None, 1],
                "timed_out": [0, 1, None, True],
                "cleanup_complete": [0, 1, None, False],
                "abandoned_pipes": [0, 1, None, True]}.items():
            for value in values:
                with self.subTest(key=key, value=value):
                    self.assertFalse(
                        RUNNER.process_ok(dict(CLEAN, **{key: value})))
            missing = dict(CLEAN)
            del missing[key]
            self.assertFalse(RUNNER.process_ok(missing))

    def test_trace_loss_is_not_clean(self):
        traced = dict(CLEAN, trace_required=True, trace={"status": "ok"})
        self.assertTrue(RUNNER.process_ok(traced))
        for status in ("violation", "not_required", None):
            with self.subTest(status=status):
                self.assertFalse(RUNNER.process_ok(
                    dict(CLEAN, trace_required=True, trace={"status": status})))
        self.assertFalse(RUNNER.process_ok(dict(CLEAN, trace_required=True,
                                                trace={})))
        self.assertFalse(RUNNER.process_ok(dict(CLEAN, trace_required=True)))

    def test_rejection_requires_normal_exit_one_and_strict_error_json(self):
        good = dict(CLEAN, exit=1,
                    stdout='{"status":"error","diagnostics":[{"code":"E_TYPE_MISMATCH"}]}',
                    stderr="")
        self.assertTrue(RUNNER.rejection_ok(good))
        self.assertEqual(RUNNER.parse_negative(good), ["E_TYPE_MISMATCH"])
        for change in ({"status": "signalled", "exit": -11},
                       {"timed_out": True},
                       {"status": "launch_failed", "exit": None},
                       {"exit": True},
                       {"exit": 0},
                       {"cleanup_complete": False}):
            with self.subTest(change=change):
                self.assertFalse(RUNNER.rejection_ok(dict(good, **change)))
        self.assertFalse(RUNNER.rejection_has_code(
            dict(good, stdout='{"status":"error","diagnostics":[]}'),
            "E_TYPE_MISMATCH"))
        self.assertTrue(RUNNER.rejection_has_code(good, "E_TYPE_MISMATCH"))
        for stdout in ("", "not json", '{"status":"ok","diagnostics":[]}',
                       '{"status":"error","diagnostics":[{}]}',
                       '{"status":"error","diagnostics":[{"code":NaN}]}'):
            with self.subTest(stdout=stdout):
                with self.assertRaises(RuntimeError):
                    RUNNER.parse_negative(dict(good, stdout=stdout))

    def test_strict_json_and_atomic_write_reject_nonfinite(self):
        self.assertEqual(RUNNER.strict_json_loads('{"x":1}'), {"x": 1})
        self.assertEqual(
            RUNNER.strict_json_loads('{"a":[0,-0.5,1.25,{"b":-3.0}]}'),
            {"a": [0, -0.5, 1.25, {"b": -3.0}]})
        for text in ('{"x":NaN}', '{"x":Infinity}', '{"x":-Infinity}',
                     '{"x":1e1000}', '{"a":[{"x":-1e1000}]}'):
            with self.subTest(text=text):
                with self.assertRaises(ValueError):
                    RUNNER.strict_json_loads(text)
                with self.assertRaises(ValueError):
                    BUILDER.strict_json_loads(text)
        with tempfile.TemporaryDirectory() as temp:
            target = Path(temp) / "record.json"
            with self.assertRaises(ValueError):
                RUNNER.atomic_write_json(target, {"x": float("nan")})
            self.assertFalse(target.exists())

    def test_missing_ir_files_fail(self):
        with tempfile.TemporaryDirectory() as temp:
            paths = RUNNER.ir_paths(temp, 2)
            for path in paths.values():
                path.write_text("content")
            self.assertTrue(RUNNER.four_ir_ok(paths))
            for key, path in paths.items():
                with self.subTest(key=key):
                    original = path.read_bytes()
                    path.unlink()
                    self.assertFalse(RUNNER.four_ir_ok(paths))
                    path.write_bytes(original)
            empty = paths["after.ll"]
            empty.write_bytes(b"")
            self.assertFalse(RUNNER.four_ir_ok(paths))

    def test_weak_observer_is_rejected(self):
        valid = {"allocations": 2, "frees": 2,
                 "events": {1: 1, 2: 1, 3: 1, 4: 1, 5: 1, 6: 1, 7: 1}}
        self.assertTrue(RUNNER.observer_valid(valid))
        self.assertFalse(RUNNER.observer_valid(
            dict(valid, allocations=2, frees=1)))
        for missing in (1, 2, 3, 5, 6, 7):
            events = dict(valid["events"])
            events[missing] = 0
            with self.subTest(missing=missing):
                self.assertFalse(RUNNER.observer_valid(
                    dict(valid, events=events)))
        with self.assertRaises(RuntimeError):
            RUNNER.parse_observer("VEC:1:object:0\nVEC:5:object:1\n")

    def test_default_duplicate_out_is_refused(self):
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp) / "out"
            RUNNER.prepare_out(out)
            self.assertTrue((out / "raw").is_dir())
            (out / "summary.json").write_text("{}")
            with self.assertRaises(RuntimeError):
                RUNNER.prepare_out(out)
            self.assertEqual((out / "summary.json").read_text(), "{}")

    def test_required_symbols_keep_core_and_owned_tokens(self):
        core = RUNNER.required_symbols("core")
        owned = RUNNER.required_symbols("owned")
        self.assertIn("lang_slice_sub_v1", core)
        self.assertIn("lang_vec_slice_v1", core)
        self.assertIn("lang_vec_slice_v1", owned)
        self.assertIn("lang_vec_set_v1", owned)
        self.assertIn("lang_slice_new_v1", owned)

    def test_invalid_timeouts_write_nothing_and_do_not_load_corpus(self):
        for option in ("--timeout", "--program-timeout", "--readelf-timeout"):
            for value in ("nan", "inf", "-inf", "0", "-1"):
                with self.subTest(option=option, value=value), \
                        tempfile.TemporaryDirectory() as temp:
                    out = Path(temp) / "out"
                    argv = ["--cli", temp, "--clang", temp, "--lld", temp,
                            "--runtime", temp, "--root", temp,
                            "--out", str(out), "--helper", temp,
                            option + "=" + value]
                    with patch.object(RUNNER, "load_corpus_support") as loader, \
                            contextlib.redirect_stderr(io.StringIO()):
                        with self.assertRaises(SystemExit) as raised:
                            RUNNER.main(argv)
                    self.assertEqual(raised.exception.code, 2)
                    loader.assert_not_called()
                    self.assertFalse(out.exists())

    def _legacy_argv(self, temp, out, extra=()):
        temp = Path(temp)
        for name in ("cli", "helper", "clang", "clang++", "runtime",
                     "readelf"):
            (temp / name).write_bytes(b"control fixture, not executed")
        return ["--cli", str(temp / "cli"), "--helper", str(temp / "helper"),
                "--clang", str(temp / "clang"), "--lld", str(temp / "clang"),
                "--runtime", str(temp / "runtime"), "--root", str(temp),
                "--readelf", str(temp / "readelf"), "--out", str(out),
                *extra]

    def test_legacy_main_repeat_creates_unique_child_and_keeps_old_records(self):
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp) / "slice-source-results"
            out.mkdir()
            (out / "summary.json").write_text('{"previous":true}\n')
            (out / "commands.json").write_text('[{"old":true}]\n')
            old = {path.name: path.read_bytes() for path in out.iterdir()}
            with patch.object(RUNNER, "load_corpus_support",
                              side_effect=RuntimeError("controlled stop")):
                with self.assertRaises(RuntimeError) as raised:
                    RUNNER.main(self._legacy_argv(temp, out))
            self.assertEqual(str(raised.exception), "controlled stop")
            self.assertEqual({path.name: path.read_bytes()
                              for path in out.iterdir() if path.is_file()}, old)
            runs = [path for path in out.iterdir() if path.is_dir()]
            self.assertEqual(len(runs), 1)
            self.assertTrue(runs[0].name.startswith("run-"))
            self.assertTrue((runs[0] / "raw").is_dir())
            self.assertTrue((runs[0] / "traces").is_dir())

    def test_explicit_fresh_main_still_refuses_nonempty_out(self):
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp) / "results"
            out.mkdir()
            (out / "summary.json").write_text('{"previous":true}\n')
            with self.assertRaises(RuntimeError) as raised:
                RUNNER.main(self._legacy_argv(temp, out, ("--fresh",)))
            self.assertIn("not empty", str(raised.exception))
            self.assertEqual((out / "summary.json").read_text(),
                             '{"previous":true}\n')

    def test_candidate_mode_forces_fresh_out(self):
        with tempfile.TemporaryDirectory() as temp:
            temp = Path(temp)
            out = temp / "candidate-results"
            out.mkdir()
            (out / "summary.json").write_text('{"previous":true}\n')
            argv = ["--cli", str(temp / "cli"),
                    "--wz-native-helper", str(temp / "driver"),
                    "--clang", str(temp / "clang"),
                    "--lld", str(temp / "lld"),
                    "--runtime", str(temp / "runtime"),
                    "--root", str(temp), "--out", str(out),
                    "--toolchains", str(temp), "--source-root", str(temp),
                    "--stdlib-source", str(temp),
                    "--build-report", str(temp / "build-report.json"),
                    "--candidate-ready", str(temp / "ready.json")]
            with patch.object(RUNNER, "validate_candidate_inputs",
                              return_value=({}, {})):
                with self.assertRaises(RuntimeError) as raised:
                    RUNNER.main(argv)
            self.assertIn("not empty", str(raised.exception))
            self.assertEqual((out / "summary.json").read_text(),
                             '{"previous":true}\n')

    def test_builder_process_record_has_timeout_and_raw_hashes(self):
        class Corpus:
            @staticmethod
            def _run_process_impl(argv, cwd, env, timeout):
                return {"exit": 7, "stdout": b"raw out\n",
                        "stderr": b"raw err\n", "timed_out": False,
                        "cleanup_complete": True, "abandoned_pipes": False}

        with tempfile.TemporaryDirectory() as temp:
            work = Path(temp)
            record = BUILDER.run_process(
                "build", ["/bin/true", "arg"], 12.5, work, Corpus(), {})
            self.assertEqual(record["timeout_s"], 12.5)
            self.assertEqual(record["cwd"], str(BUILDER.ROOT))
            self.assertEqual(record["argv"], ["/bin/true", "arg"])
            self.assertEqual(record["exit"], 7)
            self.assertIs(record["timed_out"], False)
            self.assertIs(record["cleanup_complete"], True)
            self.assertIs(record["abandoned_pipes"], False)
            self.assertEqual((work / "raw/build.stdout").read_bytes(),
                             b"raw out\n")
            self.assertEqual(record["stdout_sha256"],
                             BUILDER.sha256_bytes(b"raw out\n"))

    def _builder_control(self, after_raises=False,
                         stdout='{"status":"ok","diagnostics":[]}\n',
                         sealed_stdout=None, sealed_json_ok=None):
        with tempfile.TemporaryDirectory() as temp:
            t = Path(temp)
            source, stdlib, work = t / "source", t / "stdlib", t / "work"
            (source / "wzcc").mkdir(parents=True)
            stdlib.mkdir()
            entry, manifest = t / "main.control", t / "manifest.control"
            entry.write_text("control entry bytes\n")
            manifest.write_text("control manifest bytes\n")
            source_map = {"source-" + str(i): "0" * 64 for i in range(116)}
            std_map = {"std-" + str(i): "0" * 64 for i in range(37)}
            comp_map = {"component-" + str(i): "0" * 64 for i in range(117)}
            stable = {"component": comp_map, "stdlib": std_map, "engine": {}}
            calls = {"resources": 0, "transport": 0}

            def tree(path, suffix=None):
                path = Path(path)
                return (source_map if path == source else
                        std_map if path == stdlib else comp_map)

            def map_sha(value):
                if value == source_map:
                    return BUILDER.ED1_SOURCE_MAP_SHA
                if value == std_map:
                    return BUILDER.STDLIB37_MAP_SHA
                return "a" * 64

            def resources(*args):
                calls["resources"] += 1
                if after_raises and calls["resources"] == 2:
                    raise OSError("controlled after-resource read failure")
                return stable

            def transport(label, command, timeout, current_work, corpus, env):
                calls["transport"] += 1
                (current_work / "build").mkdir()
                (current_work / "build/program").write_bytes(
                    b"CONTROL NOT A PROGRAM")
                (current_work / "build/program.o").write_bytes(
                    b"CONTROL NOT AN OBJECT")
                return {"status": "exited", "exit": 0, "timed_out": False,
                        "cleanup_complete": True, "abandoned_pipes": False,
                        "stdout": stdout, "stderr": "", "elapsed_s": 1.0}

            argv = ["--seed", str(t / "seed"), "--source-root", str(source),
                    "--stdlib", str(stdlib), "--driver-main", str(entry),
                    "--manifest", str(manifest),
                    "--toolchain-root", str(t / "toolchains"),
                    "--strace", str(t / "strace"), "--work-dir", str(work)]
            with patch.object(BUILDER, "load_corpus_support",
                              return_value=types.SimpleNamespace()), \
                    patch.object(BUILDER, "load_nativeobs_support",
                                 return_value=types.SimpleNamespace()), \
                    patch.object(BUILDER, "tree_map", side_effect=tree), \
                    patch.object(BUILDER, "json_hash", side_effect=map_sha), \
                    patch.object(BUILDER, "resources", side_effect=resources), \
                    patch.object(BUILDER, "engine_resources",
                                 return_value={}), \
                    patch.object(BUILDER, "run_process",
                                 side_effect=transport), \
                    contextlib.redirect_stdout(io.StringIO()):
                code = BUILDER.main(argv)
                reuse_validation = None
                if sealed_stdout is not None:
                    report_path = work / "build-report.json"
                    sealed = json.loads(report_path.read_text())
                    sealed["process"]["stdout"] = sealed_stdout
                    if sealed_json_ok is None:
                        sealed.pop("build_json_ok", None)
                    else:
                        sealed["build_json_ok"] = sealed_json_ok
                    report_path.write_text(json.dumps(sealed))
                    before = report_path.read_bytes()
                    reuse_validation = BUILDER.validate_build(
                        report_path, work / "component", stdlib, t / "seed",
                        t / "toolchains", t / "strace", source)
                    self.assertEqual(report_path.read_bytes(), before)
            report = json.loads((work / "build-report.json").read_text())
            final = json.loads((work / "builder-report.json").read_text())
            self.assertEqual(calls["transport"], 1)
            return {"exit": code, "build_report_status": report["status"],
                    "process": report.get("process"),
                    "build_json_ok": report.get("build_json_ok"),
                    "builder_report_status": final["status"],
                    "failure_messages": final["failures"],
                    "reuse_validation": reuse_validation}

    def test_builder_rejects_malformed_error_nonfinite_and_fake_success(self):
        for stdout in ("not JSON\n",
                       '{"status":"error","diagnostics":[]}\n',
                       '{"status":"ok","diagnostics":[],"x":1e1000}\n',
                       '{"status":"ok"}\n',
                       '{"status":"ok","diagnostics":[{"severity":"error"}]}\n',
                       '{"status":"ok","diagnostics":{}}\n'):
            with self.subTest(stdout=stdout):
                result = self._builder_control(stdout=stdout)
                self.assertEqual(result["exit"], 1)
                self.assertEqual(result["build_report_status"], "failed")
                self.assertEqual(result["builder_report_status"], "failed")
                self.assertIsNotNone(result["process"])
                self.assertEqual(result["process"]["exit"], 0)
                self.assertIs(result["build_json_ok"], False)
        good = self._builder_control()
        self.assertEqual(good["exit"], 0)
        self.assertEqual(good["build_report_status"], "passed")
        self.assertEqual(good["builder_report_status"], "passed")
        self.assertIs(good["build_json_ok"], True)

    def test_sealed_build_rechecks_stdout_without_rewriting_report(self):
        good = self._builder_control(
            sealed_stdout='{"status":"ok","diagnostics":[]}')
        self.assertEqual(good["reuse_validation"]["status"], "passed")
        for stdout, flag in [
                ("not JSON\n", None), ("not JSON\n", True),
                ('{"status":"ok","diagnostics":[{"severity":"error"}]}', True),
                ('{"status":"error","diagnostics":[]}', True),
                ('{"status":"ok","diagnostics":[],"x":1e1000}', True)]:
            with self.subTest(stdout=stdout, flag=flag):
                result = self._builder_control(
                    sealed_stdout=stdout, sealed_json_ok=flag)
                validation = result["reuse_validation"]
                self.assertEqual(validation["status"], "failed")
                self.assertTrue(any("saved build stdout" in error
                                    for error in validation["failures"]))
                self.assertTrue(validation["build_report_not_rewritten"])

    def test_builder_saves_process_before_after_resource_error(self):
        result = self._builder_control(after_raises=True)
        self.assertEqual(result["exit"], 1)
        self.assertEqual(result["build_report_status"], "failed")
        self.assertEqual(result["builder_report_status"], "failed")
        self.assertIsNotNone(result["process"])
        self.assertEqual(result["process"]["exit"], 0)
        self.assertIs(result["process"]["timed_out"], False)
        self.assertIs(result["process"]["cleanup_complete"], True)
        self.assertIs(result["process"]["abandoned_pipes"], False)


if __name__ == "__main__":
    unittest.main()
