"""Fixed-input generation checks; no benchmark timing."""
from pathlib import Path
import os
import struct
import tempfile
import unittest

import formal_sort_programs as formal


class SortProgramTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.fixture.cleanup)
        cls.root = Path(cls.fixture.name)
        state, raw = formal.SEED, []
        for _ in range(65536):
            state = (state * 1664525 + 1013904223) & 0xffffffff
            raw.append(state)
        for name, values in (("input", raw), ("expected", sorted(raw))):
            (cls.root/f"sort-n65536-{name}.u32le").write_bytes(struct.pack("<65536I", *values))
        cls.tools = cls.root / "tools"
        cls.tools.mkdir()
        for name in ("clang++", "opt", "ld.lld"):
            (cls.tools/name).write_bytes(b"test tool bytes; never executed")

    def make(self, algorithm="heap"):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        return formal.SortPrograms(algorithm, 65536, self.root,
                                   Path(temporary.name)/"new", self.tools, os.environ)

    def test_all_three_templates_render_fixed_arrays_and_rounds(self):
        for algorithm in ("heap", "quick", "merge"):
            builder = self.make(algorithm)
            case = builder.render(4)
            source = Path(case["source"]["path"]).read_text()
            cpp = Path(case["reference"]["path"]).read_text()
            self.assertIn("n=65536 seed=0x6d2b79f5 rounds=4", source)
            self.assertIn("n=65536 seed=0x6d2b79f5 rounds=4", cpp)
            self.assertNotIn("{{", source + cpp)
            self.assertEqual(builder.raw[0], 53002960)
            self.assertEqual(tuple(sorted(builder.raw)), builder.golden)
            self.assertEqual(builder.render(4), case)

    def test_shared_cpp_commands_disable_float_contraction(self):
        program, ir, obj, commands = formal.cpp_build_commands(
            4, self.root / "reference.cpp", self.root / "out", self.tools)
        self.assertEqual(len(commands), 4)
        for argv in commands[:3]:
            self.assertIn("-ffp-contract=off", argv)
            self.assertNotIn("-ffast-math", argv)
        self.assertIn(obj, commands[0])
        self.assertIn(obj, commands[1])
        self.assertIn(program, commands[1])
        self.assertIn("-emit-llvm", commands[2])
        self.assertIn("-passes=verify", commands[3])
        self.assertIn("-disable-output", commands[3])

    def test_new_output_and_fixed_scope_required(self):
        builder = self.make()
        with self.assertRaises(formal.common.PerfError):
            formal.SortPrograms("heap", 65536, self.root, builder.out, self.tools, os.environ)
        for rounds in (0, 3, True, (1 << 22)):
            with self.assertRaises(formal.common.PerfError):
                builder.render(rounds)
        with self.assertRaises(formal.common.PerfError):
            formal.SortPrograms("heap", 64, self.root, builder.out/"other", self.tools, os.environ)

    def test_changed_generated_file_and_tool_rejected(self):
        builder = self.make()
        case = builder.render(1)
        Path(case["reference"]["path"]).write_text("modified")
        with self.assertRaises(formal.common.PerfError):
            builder.render(1)
        tool = self.tools/"opt"
        before = tool.read_bytes()
        try:
            tool.write_bytes(b"modified tool")
            with self.assertRaises(formal.common.PerfError):
                builder.render(2)
        finally:
            tool.write_bytes(before)


if __name__ == "__main__":
    unittest.main()
