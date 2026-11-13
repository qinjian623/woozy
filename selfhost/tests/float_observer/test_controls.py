"""Small runner controls; fixture files and the fake builder test metadata only.

Run with python3 -B selfhost/tests/float_observer/test_controls.py -v.
No compiler, driver or numeric corpus is built by these tests.
"""
import copy
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("floating_observer_run", HERE / "run.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class Controls(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="woozy-float-controls-")
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name)
        self.corpus = runner.validation_module("corpus")

    def write(self, name, data=b"fixture"):
        path = self.work / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def source_root(self):
        root = self.work / "root"
        for index in range(114):
            self.write(f"root/selfhost/src/wzcc/p{index}.wz", b"package fixture;\n")
        self.write("root/selfhost/src/wzcc/nativeverify/verify_ops.wz", b"fixture verify\n")
        return root

    def proof_fixture(self):
        """Real bootstrap file collector over an isolated synthetic BOOT tree."""
        boot = runner.validation_module("bootstrap")
        root = self.source_root()
        output = self.work / "boot"
        runner.copy_tree(root / "selfhost/src/wzcc", output / "source/src/wzcc")
        self.write("boot/source/src/main.wz", b"package app; func main() {}\n")
        self.write("boot/source/lang.toml", b"schema_version=1\n")
        self.write("boot/resources/stdlib/std.wz", b"package fixture;\n")
        stdlib = self.write("stage0/stdlib/std.wz", b"package fixture;\n").parent
        for name in boot.RUNTIME_ARCHIVES:
            self.write("boot/resources/runtime/" + name)
        for name in boot.PYTHON_RUNTIME:
            self.write("boot/resources/runtime/python/" + name)
        for name in boot.TOOL_NAMES:
            self.write("boot/resources/toolchain/" + name)
        self.write("boot/resources/stage0/woozyc")
        self.write("boot/resources/stage0/lang_native_helper")
        seed = self.write("stage0/woozyc")
        macros = {"LANG_STD_SOURCE_ROOT": str(stdlib)}
        for name in boot.STAGE0_FILE_MACROS:
            macros[name] = str(self.write("stage0/macros/" + name))
        stage0 = {
            "seed": {"path": str(seed), "entry_path": str(seed)},
            "helper": {"path": str(self.write("stage0/helper"))},
            "stdlib": {"path": str(stdlib)},
            "runtime": {name: {"path": str(self.write("stage0/runtime/" + name))}
                        for name in boot.RUNTIME_ARCHIVES},
            "tools": {name: {"path": str(self.write("stage0/tools/" + name))}
                      for name in ("clang", "ld.lld")},
            "build": {
                "build_ninja": str(self.write("stage0/build.ninja")),
                "cmake_cache": str(self.write("stage0/CMakeCache.txt")),
                "macros": macros,
            },
        }
        toolchain = self.work / "toolchain"
        toolchain.mkdir()
        snapshot = boot._frozen_inputs(output, stage0, toolchain, strict=True)
        (output / "result.json").write_text(json.dumps({
            "stage0": stage0, "frozen_inputs": snapshot}))
        candidate = self.write("canonical-candidate", b"hash fixture only; not executable")
        proof = {
            "validation": "accepted",
            "canonical_stage1": str(candidate),
            "canonical_stage1_sha256": runner.sha(candidate),
            "canonical_source": str(output / "source"),
            "canonical_input_digest": boot._snapshot_digest(snapshot),
            "production_sha256": runner.sha(
                root / "selfhost/src/wzcc/nativeverify/verify_ops.wz"),
        }
        path = self.work / "proof.json"
        path.write_text(json.dumps(proof))
        return root, output, candidate, proof, path

    def test_proof_digest_source_and_resources(self):
        root, output, candidate, proof, path = self.proof_fixture()
        self.assertEqual(runner.canonical_identity(candidate, path, root)["role"], "canonical")
        bad = dict(proof, canonical_input_digest="0" * 64)
        path.write_text(json.dumps(bad))
        self.assertEqual(runner.canonical_identity(candidate, path, root)["role"], "provisional")
        path.write_text(json.dumps(proof))
        for changed in (output / "source/src/main.wz",
                        output / "resources/runtime/liblang_native_runtime.a"):
            before = changed.read_bytes()
            changed.write_bytes(before + b"changed")
            result = runner.canonical_identity(candidate, path, root)
            self.assertEqual(result["role"], "provisional")
            self.assertFalse(result["checks"]["proof_frozen_inputs_match"])
            changed.write_bytes(before)
        self.assertEqual(runner.canonical_identity(candidate, path, root)["role"], "canonical")

    def test_actual_writer_to_validator_and_legacy_status(self):
        root = self.source_root()
        compiler = self.write("builder/woozyc", (
            "#!" + sys.executable + "\n"
            "import pathlib,sys\n"
            "out=pathlib.Path(sys.argv[sys.argv.index('--out-dir')+1])\n"
            "out.mkdir(parents=True)\n"
            "(out/'program').write_bytes(b'controlled tool output')\n"
            "print('{\"status\":\"ok\"}')\n").encode())
        compiler.chmod(0o755)
        for name in ("liblang_native_runtime.a", "liblang_hosted_runtime.a",
                     "lang_native_helper"):
            self.write("builder/native/" + name)
        work = self.work / "build"
        work.mkdir()
        recorder = runner.Recorder(work)
        recorder.run_process = self.corpus._run_process_impl
        binary, evidence, _ = runner.build_driver(
            work, root, compiler, recorder, dict(os.environ), 5)
        path = work / "driver-build-evidence.json"
        self.assertEqual(evidence["status"], "passed")
        self.assertTrue(all(runner.validate_driver_evidence(
            binary, path, root, compiler)["validated_checks"].values()))
        original = json.loads(path.read_text())
        legacy = dict(original, status="exited")
        path.write_text(json.dumps(legacy))
        runner.validate_driver_evidence(binary, path, root, compiler)
        for field in ("helper_sha256", "runtime_sha256"):
            bad = dict(original)
            del bad[field]
            path.write_text(json.dumps(bad))
            with self.subTest(missing=field), self.assertRaisesRegex(RuntimeError, field):
                runner.validate_driver_evidence(binary, path, root, compiler)
        for field, value in (("exit", False), ("exit", 0.0),
                             ("timed_out", True), ("cleanup_complete", False),
                             ("abandoned_pipes", True)):
            bad = dict(original, **{field: value})
            path.write_text(json.dumps(bad))
            with self.assertRaises(RuntimeError):
                runner.validate_driver_evidence(binary, path, root, compiler)
        bad = copy.deepcopy(original)
        bad["argv"][bad["argv"].index("--backend") + 1] = "python"
        path.write_text(json.dumps(bad))
        with self.assertRaises(RuntimeError):
            runner.validate_driver_evidence(binary, path, root, compiler)

    def test_python_counts_types_and_both_signatures(self):
        probes = [{"function": "f", "arguments": [],
                   "result": {"type": "bool", "class": "bool", "value": False}}]
        good = [{"function": "f", "outcome": "return",
                 "signature": {"result": "bool", "parameters": []},
                 "result": dict(probes[0]["result"])}]
        runner.validate_python_observations(probes, good, good)
        for bad in ([], good + good):
            with self.assertRaises(RuntimeError):
                runner.validate_python_observations(probes, bad, bad)
        for kind in ("both-bool-int", "cli-bool-int", "both-parameters",
                     "cli-signature", "both-result-type"):
            left, right = copy.deepcopy(good), copy.deepcopy(good)
            if kind == "both-bool-int":
                left[0]["result"]["value"] = right[0]["result"]["value"] = 0
            elif kind == "cli-bool-int":
                right[0]["result"]["value"] = 0
            elif kind == "both-parameters":
                left[0]["signature"]["parameters"] = ["f64"]
                right[0]["signature"]["parameters"] = ["f64"]
            elif kind == "cli-signature":
                right[0]["signature"] = {"result": "f64", "parameters": ["f64"]}
            else:
                left[0]["result"]["type"] = right[0]["result"]["type"] = "f64"
            with self.subTest(kind=kind), self.assertRaises(RuntimeError):
                runner.validate_python_observations(probes, left, right)

    def test_own_dirty_child_and_signal_timeout(self):
        recorder = runner.Recorder(self.work)
        recorder.run_process = self.corpus._run_process_impl
        detected, record = runner.dirty_cleanup_control(
            recorder, self.work, Path(sys.executable), dict(os.environ), 1)
        self.assertTrue(detected)
        self.assertEqual(record["exit"], 0)
        self.assertFalse(record["cleanup_complete"])
        self.assertTrue(record["abandoned_pipes"])
        pid = int((self.work / "dirty-cleanup/child.pid").read_text())
        state = Path(f"/proc/{pid}/stat")
        self.assertTrue(not state.exists() or state.read_text().split()[2] == "Z")
        for argv, timeout in ((["/bin/sh", "-c", "sleep 2"], 0.05),
                              (["/bin/sh", "-c", "kill -TERM $$"], 2)):
            with self.assertRaises(runner.CommandError):
                recorder.run("must-fail", argv, self.work,
                             dict(os.environ), timeout)

    def test_timeout_arguments_rejected_before_work_creation(self):
        work = self.work / "must-not-exist"
        for option in ("build", "command", "observer"):
            for value in ("nan", "inf", "-inf", "0", "-1"):
                result = self.corpus._run_process_impl([
                    sys.executable, "-B", HERE / "run.py",
                    f"--{option}-timeout={value}", "--work-dir", work],
                    self.work, dict(os.environ), 5)
                self.assertEqual(result["exit"], 2)
                self.assertIs(result["timed_out"], False)
                self.assertIs(result["cleanup_complete"], True)
                self.assertIs(result["abandoned_pipes"], False)
                self.assertIn(b"finite and positive", result["stderr"])
                self.assertFalse(work.exists())


if __name__ == "__main__":
    unittest.main()
