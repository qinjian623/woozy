"""Self-checks for the shared Source O2 artifact builder.

These use a synthetic Release build tree and command recorder.  They execute no
compiler and no generated algorithm program.
"""
from pathlib import Path
import json
import os
import struct
import tempfile
import unittest
from unittest.mock import patch

import formal_native_programs as formal
import formal_sort_programs as sorts


def elf_header(elf_type):
    data = bytearray(64)
    data[:4] = b"\x7fELF"
    data[4], data[5], data[6] = 2, 1, 1
    data[16:18] = elf_type.to_bytes(2, "little")
    data[18:20] = (62).to_bytes(2, "little")
    return bytes(data)


class FakeCommands:
    def __init__(self, *, helper_object=None, on_command=None):
        self.object = elf_header(1)
        self.program = elf_header(2)
        self.helper_object = self.object if helper_object is None else helper_object
        self.calls = []
        self.on_command = on_command

    def __call__(self, argv, *, timeout, env, cpu=None):
        argv = [str(value) for value in argv]
        self.calls.append(argv)
        result = {"argv": argv, "status": "exited", "exit": 0,
                  "stdout": "", "stderr": "", "elapsed_ns": 1,
                  "timeout_s": timeout, "cpu": cpu}
        if self.on_command is not None:
            self.on_command(argv, result)
        if "check" in argv:
            result["stdout"] = '{"status":"ok"}\n'
        elif "build" in argv:
            directory = Path(argv[argv.index("--out-dir") + 1])
            directory.mkdir(parents=True, exist_ok=True)
            (directory / "program.o").write_bytes(self.object)
            (directory / "program").write_bytes(self.program)
            result["stdout"] = '{"status":"ok"}\n'
        elif "--out-object" in argv:
            Path(argv[argv.index("--out-object") + 1]).write_bytes(self.helper_object)
            Path(argv[argv.index("--out-llvm") + 1]).write_text("; optimized\n")
            Path(argv[argv.index("--out-llvm-before") + 1]).write_text("; before\n")
            Path(argv[argv.index("--out-native") + 1]).write_text("native\n")
            result["stdout"] = json.dumps({
                "status": "passed", "llvm_verified_before": True,
                "llvm_verified_after": True}) + "\n"
        elif argv[0].endswith("llvm-readelf"):
            if "-l" in argv:
                result["stdout"] = "Program Headers:\n  LOAD 0x0\n"
            else:
                result["stdout"] = "There is no dynamic section in this file.\n"
        elif argv[0].endswith("llvm-nm"):
            result["stdout"] = ""
        else:
            result["stdout"] = "fake version\n" if "--version" in argv else ""
        return result


class NativeProgramTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.fixture.cleanup)
        cls.oracle = Path(cls.fixture.name) / "oracle"
        cls.oracle.mkdir()
        state, raw = sorts.SEED, []
        for _ in range(65536):
            state = (state * 1664525 + 1013904223) & 0xffffffff
            raw.append(state)
        for name, values in (("input", raw), ("expected", sorted(raw))):
            (cls.oracle / f"sort-n65536-{name}.u32le").write_bytes(
                struct.pack("<65536I", *values))

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.build = self.root / "build"
        self.llvm = self.build / "llvm"
        for path in (self.build / "native", self.llvm):
            path.mkdir(parents=True)
        self.langc = self.build / "woozyc"
        self.helper = self.build / "native" / "lang_native_helper"
        self.runtime = self.build / "native" / "liblang_native_runtime.a"
        for path in (self.langc, self.helper, self.runtime,
                     *[self.llvm / name for name in formal.LLVM_TOOLS]):
            path.write_bytes(f"tool {path.name}".encode())
        self.write_build_tree(self.helper)
        self.builder = sorts.SortPrograms(
            "heap", 65536, self.oracle, self.root / "out", self.llvm, os.environ)

    def write_build_tree(self, embedded_helper):
        helper = str(embedded_helper)
        clang, lld = str(self.llvm / "clang"), str(self.llvm / "ld.lld")
        runtime = str(self.runtime)
        self.build.joinpath("build.ninja").write_text(
            "build CMakeFiles/woozyc.dir/src/driver/main.cpp.o: CXX_COMPILER__woozyc_unscanned_Release /x\n"
            f'  DEFINES = -DLANG_NATIVE_HELPER=\\"{helper}\\"\n\n'
            "build native/CMakeFiles/lang_native_helper.dir/helper.cpp.o: CXX_COMPILER__lang_native_helper_unscanned_Release /x\n"
            f'  DEFINES = -DLANG_NATIVE_CLANG=\\"{clang}\\" -DLANG_NATIVE_LLD=\\"{lld}\\" '
            f'-DLANG_NATIVE_RUNTIME=\\"{runtime}\\"\n')
        self.build.joinpath("CMakeCache.txt").write_text(
            "CMAKE_BUILD_TYPE:STRING=Release\n")

    def build_source(self, commands, rounds=1, **changes):
        values = {"langc": self.langc, "helper": self.helper,
                  "runtime": self.runtime, "llvm_bin": self.llvm,
                  "env": os.environ}
        values.update(changes)
        with patch.object(formal.common, "command", commands):
            return formal.build_source(self.builder, rounds, **values)

    def sidecar(self, rounds=1):
        return json.loads((self.builder.out / f"r{rounds}" /
                           "source-O2" / "build.json").read_text())

    def test_correct_commands_object_and_static_elf(self):
        commands = FakeCommands()
        program = self.build_source(commands)
        directory = self.builder.out / "r1" / "source-O2"
        self.assertEqual(program, (directory / "langc" / "program").resolve())
        self.assertEqual((directory / "langc" / "program.o").read_bytes(),
                         (directory / "llvm" / "program.o").read_bytes())
        self.assertEqual(program.read_bytes(), elf_header(2))
        row = self.sidecar()
        self.assertEqual(row["status"], "built")
        self.assertTrue(row["object_match"])
        self.assertTrue(row["static_elf"]["source_static_ok"])
        self.assertEqual([item["kind"] for item in row["verify"]],
                         ["before", "after"])
        self.assertEqual(row["actual_work_review"], "pending")
        self.assertFalse(row["formal_timing"])
        self.assertTrue(row["hashes"]["unchanged"])
        manifest = row["source_case"]["manifest"]["path"]
        check = next(argv for argv in commands.calls
                     if len(argv) > 1 and argv[1] == "check")
        self.assertEqual(check[:5], [str(self.langc), "check", "--project",
                                     manifest, "--backend"])
        build = next(argv for argv in commands.calls
                     if len(argv) > 1 and argv[1] == "build")
        self.assertEqual(build[1:7], ["build", "--project", manifest,
                                      "--backend", "native", "--opt"])
        self.assertEqual(build[7], "2")
        helper = next(argv for argv in commands.calls
                      if "--out-object" in argv)
        self.assertIn("--out-object", helper)
        self.assertIn("--out-llvm", helper)
        self.assertIn("--out-llvm-before", helper)
        self.assertIn("--out-native", helper)
        self.assertEqual(helper[-2:], ["--format", "json"])
        verifies = [argv for argv in commands.calls if argv[1:2] == ["-passes=verify"]]
        self.assertEqual(len(verifies), 2)
        self.assertTrue(all("-disable-output" in argv for argv in verifies))
        self.assertEqual(
            sorted(path.name for path in (directory / "llvm").iterdir()),
            ["after.ll", "before.ll", "program.nir", "program.o"])

    def test_existing_round_directory_is_never_overwritten(self):
        self.build_source(FakeCommands())
        sidecar = self.sidecar()
        with self.assertRaisesRegex(formal.common.PerfError, "refusing to overwrite"):
            self.build_source(FakeCommands())
        self.assertEqual(self.sidecar(), sidecar)

    def test_wrong_tool_combination_is_rejected_and_recorded(self):
        other = self.root / "other-helper"
        other.write_bytes(b"wrong helper")
        with self.assertRaisesRegex(formal.common.PerfError, "LANG_NATIVE_HELPER"):
            self.build_source(FakeCommands(), helper=other)
        row = self.sidecar()
        self.assertEqual(row["status"], "failed")
        self.assertIn("LANG_NATIVE_HELPER", row["failure"]["message"])
        self.assertNotIn("object_match", row)

    def test_object_mismatch_preserves_failure_and_commands(self):
        commands = FakeCommands(helper_object=elf_header(1) + b"different")
        with self.assertRaisesRegex(formal.common.PerfError, "helper object differ"):
            self.build_source(commands)
        row = self.sidecar()
        self.assertEqual(row["status"], "failed")
        self.assertFalse(row["object_match"])
        self.assertIn("helper object differ", row["failure"]["message"])
        self.assertEqual(len(row["commands"]), 3)

    def test_changed_tool_and_source_are_rejected(self):
        commands = FakeCommands()
        source = None

        def touch(argv, result):
            nonlocal source
            if "build" in argv:
                source = Path(self.builder.generated[1]["source"]["path"])
                source.write_text(source.read_text() + "\n")
        commands.on_command = touch
        with self.assertRaisesRegex(formal.common.PerfError, "generated source changed"):
            self.build_source(commands)
        row = self.sidecar()
        self.assertEqual(row["status"], "failed")
        self.assertIn("generated source changed", row["failure"]["message"])

    def test_tool_change_during_build_changes_saved_hashes_and_fails(self):
        commands = FakeCommands()
        opt = self.llvm / "opt"

        def change_tool(argv, result):
            if "--out-object" in argv:
                opt.write_bytes(b"changed opt")
        commands.on_command = change_tool
        with self.assertRaisesRegex(formal.common.PerfError, "tools or build metadata changed"):
            self.build_source(commands)
        row = self.sidecar()
        self.assertEqual(row["status"], "failed")
        self.assertFalse(row["hashes"]["unchanged"])
        self.assertNotEqual(row["hashes"]["before"]["opt"]["sha256"],
                            row["hashes"]["after"]["opt"]["sha256"])


if __name__ == "__main__":
    unittest.main()
