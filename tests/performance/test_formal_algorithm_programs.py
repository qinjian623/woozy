"""Fixed-input generation checks for the five non-sort algorithms."""
from pathlib import Path
import json
import os
import shutil
import tempfile
import unittest

import formal_algorithm_programs as formal


class AlgorithmProgramTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.fixture.cleanup)
        cls.root = Path(cls.fixture.name)
        cls.tools = cls.root / "tools"
        cls.tools.mkdir()
        for name in ("clang++", "opt", "ld.lld"):
            (cls.tools / name).write_bytes(b"test tool bytes; never executed")

    def make(self, algorithm, n, oracle_dir=None, name=None):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        return formal.AlgorithmPrograms(
            algorithm, n, oracle_dir or formal.DEFAULT_ORACLE_DIRS[algorithm],
            Path(temporary.name) / (name or "new"), self.tools, os.environ)

    def test_all_five_algorithms_two_sizes_render_r1_r2(self):
        self.assertEqual(formal.SIZES, {
            "sieve": (1000000, 10000000),
            "fannkuch": (9, 10),
            "mandelbrot": (512, 2048),
            "binarytrees": (14, 18),
            "matmul": (128, 512),
        })
        for algorithm, sizes in formal.SIZES.items():
            for n in sizes:
                with self.subTest(algorithm=algorithm, n=n):
                    builder = self.make(algorithm, n)
                    first = builder.render(1)
                    second = builder.render(2)
                    self.assertIs(builder.render(1), first)
                    for row in (first, second):
                        for key in ("source", "reference", "manifest"):
                            path = Path(row[key]["path"])
                            self.assertTrue(path.is_file())
                            self.assertEqual(formal.common.sha256_file(path),
                                             row[key]["sha256"])
                        self.assertEqual(row["inputs"], builder.inputs)
                        self.assertEqual(row["templates"], builder.templates)
                        self.assertEqual(row["tools"], builder.tools)
                        self.assertFalse(row["cache_reuse"])
                        self.assertNotIn(
                            "{{", Path(row["source"]["path"]).read_text()
                            + Path(row["reference"]["path"]).read_text())
                    self.assertNotEqual(first["source"]["path"],
                                        second["source"]["path"])
                    self.assertNotEqual(first["source"]["sha256"],
                                        second["source"]["sha256"])
                    self.assertNotEqual(first["reference"]["sha256"],
                                        second["reference"]["sha256"])
                    saved = json.loads(
                        (builder.out / "programs.json").read_text())
                    self.assertEqual(saved["actual_work_review"], "pending")
                    self.assertFalse(saved["formal_timing"])
                    self.assertEqual(sorted(int(key) for key in saved["generated"]),
                                     [1, 2])

    def test_changed_oracle_file_rejected(self):
        source = formal.DEFAULT_ORACLE_DIRS["binarytrees"]
        oracle = self.root / "binarytrees-oracle-copy"
        oracle.mkdir()
        shutil.copy2(source / "binarytrees.json", oracle / "binarytrees.json")
        builder = self.make("binarytrees", 14, oracle, "oracle-mutation")
        target = next(Path(path) for path in builder.inputs
                      if Path(path).parent == oracle)
        original = target.read_bytes()
        try:
            target.write_bytes(original + b"\n")
            with self.assertRaisesRegex(formal.common.PerfError, "changed"):
                builder.render(2)
        finally:
            target.write_bytes(original)
        builder.render(2)

    def test_changed_generated_file_and_tool_rejected(self):
        builder = self.make("binarytrees", 14, name="generated-mutation")
        case = builder.render(1)
        source = Path(case["source"]["path"])
        original = source.read_bytes()
        try:
            source.write_bytes(original + b"\n")
            with self.assertRaisesRegex(formal.common.PerfError, "changed"):
                builder.render(2)
        finally:
            source.write_bytes(original)
        tool = self.tools / "opt"
        before = tool.read_bytes()
        try:
            tool.write_bytes(b"modified tool")
            with self.assertRaisesRegex(formal.common.PerfError, "changed"):
                builder.render(2)
        finally:
            tool.write_bytes(before)

    def test_new_output_and_fixed_scope_required(self):
        builder = self.make("sieve", 1000000)
        with self.assertRaises(formal.common.PerfError):
            formal.AlgorithmPrograms(
                "sieve", 1000000, formal.DEFAULT_ORACLE_DIRS["sieve"],
                builder.out, self.tools, os.environ)
        for rounds in (0, 3, True, 1 << 22):
            with self.assertRaises(formal.common.PerfError):
                builder.render(rounds)
        for algorithm, n in (("sieve", 512), ("fannkuch", 8),
                             ("mandelbrot", 1024), ("binarytrees", 16),
                             ("matmul", 256), ("heap", 65536),
                             ("sieve", True)):
            with self.assertRaises(formal.common.PerfError):
                formal.AlgorithmPrograms(
                    algorithm, n, formal.DEFAULT_ORACLE_DIRS["sieve"],
                    self.root / f"invalid-{algorithm}-{n}", self.tools,
                    os.environ)


if __name__ == "__main__":
    unittest.main()
