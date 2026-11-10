"""C++ build callbacks for the five fixed non-sort algorithms.

The generated Source/C++/manifest files and the O2 object/ELF/IR come from the
existing ``large_prepare.py`` generation functions and frozen inputs.  This
module only chooses a fixed algorithm/size, preserves hashes and records the
four C++ build commands.  Source compilation, running the programs, calibration
samples and actual-work review are separate steps.
"""
from pathlib import Path
import importlib.util
import json
import sys

import formal_sampling
import formal_sort_programs

common = formal_sort_programs.common
HERE = Path(__file__).resolve().parent
ALGORITHMS = ("sieve", "fannkuch", "mandelbrot", "binarytrees", "matmul")
SIZES = {
    name: tuple(sorted(n for algorithm, n in formal_sampling.GRID
                       if algorithm == name))
    for name in ALGORITHMS
}


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load shared generator: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


GENERATORS = {
    "sieve": _load("_formal_algorithm_sieve_generator",
                   HERE / "sieve" / "large_prepare.py"),
    "fannkuch": _load("_formal_algorithm_fannkuch_generator",
                      HERE / "fannkuch" / "large_prepare.py"),
    "mandelbrot": _load("_formal_algorithm_mandelbrot_generator",
                        HERE / "mandelbrot" / "large_prepare.py"),
    "binarytrees": _load("_formal_algorithm_binarytrees_generator",
                         HERE / "binarytrees" / "large_prepare.py"),
    "matmul": _load("_formal_algorithm_matmul_generator",
                    HERE / "matmul" / "large_prepare.py"),
}
BINARYTREES_PREPARE = _load("_formal_algorithm_binarytrees_prepare",
                            HERE / "binarytrees" / "prepare.py")
GENERATORS["binarytrees"].BT = BINARYTREES_PREPARE

DEFAULT_ORACLE_DIRS = {
    "sieve": Path(GENERATORS["sieve"].DEFAULT_ORACLE_DIR),
    "fannkuch": Path(GENERATORS["fannkuch"].DEFAULT_ORACLE_DIR),
    "mandelbrot": Path(GENERATORS["mandelbrot"].DEFAULT_ORACLE_DIR),
    "binarytrees": Path(GENERATORS["binarytrees"].ORACLE).parent,
    "matmul": Path(GENERATORS["matmul"].DEFAULT_ORACLE_DIR),
}
TEMPLATE_DIRS = {name: HERE / name for name in ALGORITHMS}
TEMPLATE_NAMES = {
    "sieve": tuple(GENERATORS["sieve"].TEMPLATE_NAMES),
    "fannkuch": tuple(GENERATORS["fannkuch"].TEMPLATE_NAMES),
    "mandelbrot": tuple(GENERATORS["mandelbrot"].read_templates()),
    "binarytrees": tuple(BINARYTREES_PREPARE.TEMPLATE_NAMES),
    "matmul": tuple(GENERATORS["matmul"].TEMPLATE_NAMES),
}


class AlgorithmPrograms:
    def __init__(self, algorithm, n, oracle_dir, out, llvm_bin, env):
        if (algorithm not in ALGORITHMS or type(n) is not int
                or n not in SIZES[algorithm]):
            raise common.PerfError(
                "only the five fixed large algorithms and their two sizes are supported")
        self.algorithm, self.n = algorithm, n
        self.generator = GENERATORS[algorithm]
        self.out, self.llvm_bin = Path(out).resolve(), Path(llvm_bin)
        self.env = dict(env)
        if self.out.exists():
            raise common.PerfError(f"output already exists: {self.out}")
        self.template_paths = [
            TEMPLATE_DIRS[algorithm] / name for name in TEMPLATE_NAMES[algorithm]
        ]
        self.templates = {
            str(path): common.sha256_file(path) for path in self.template_paths
        }
        self.template_text = self._read_templates()
        self.tools = {}
        for name in ("clang++", "ld.lld", "opt"):
            path = self.llvm_bin / name
            if not path.is_file():
                raise common.PerfError(f"missing LLVM tool: {path}")
            self.tools[str(path)] = common.sha256_file(path)
        self.inputs = {}
        self.out.mkdir(parents=True, exist_ok=False)
        self._load_oracle(Path(oracle_dir).resolve())
        for path in self._script_paths():
            self.inputs[str(path)] = common.sha256_file(path)
        self.generated, self.builds = {}, []
        self.save()

    def _read_templates(self):
        if self.algorithm == "binarytrees":
            return BINARYTREES_PREPARE.read_templates()
        return self.generator.read_templates()

    def _script_paths(self):
        paths = [
            Path(__file__), Path(formal_sort_programs.__file__),
            Path(formal_sampling.__file__), Path(common.__file__),
            Path(formal_sort_programs.run_sort.__file__),
            Path(self.generator.__file__),
        ]
        if self.algorithm == "binarytrees":
            paths.append(Path(BINARYTREES_PREPARE.__file__))
        else:
            paths.append(Path(self.generator.legacy.__file__))
        return paths

    def _load_oracle(self, oracle_dir):
        if not oracle_dir.is_dir():
            raise common.PerfError(f"frozen input directory is missing: {oracle_dir}")
        self.oracle_dir = oracle_dir
        if self.algorithm == "sieve":
            _flags, _composite, record, files = self.generator.load_oracle(oracle_dir)
            row = record["cases"][str(self.n)]
            self.expected_count, self.expected_sum = row["count"], row["sum"]
        elif self.algorithm == "fannkuch":
            expected, record, files = self.generator.load_oracle(oracle_dir)
            self.expected = expected[self.n]
        elif self.algorithm == "mandelbrot":
            payloads, record, files = self.generator.load_oracle(oracle_dir)
            self.payload = payloads[self.n]
        elif self.algorithm == "matmul":
            record, files = self.generator.load_oracle(oracle_dir, self.out)
            selected = {(self.n, role):
                        record["records"][f"n{self.n}-{role}"]
                        for role in ("a", "b", "expected")}
            tables, verification = self.generator.verify_oracle_matrices(
                self.n, selected)
            self.literals = self.generator.build_literals(self.n, tables)
            self.oracle_verification = verification
        else:
            oracle = oracle_dir / "binarytrees.json"
            expected_sha = self.generator.ORACLE_SHA
            if common.sha256_file(oracle) != expected_sha:
                raise common.PerfError(f"frozen binarytrees oracle differs: {oracle}")
            document = json.loads(oracle.read_text(encoding="utf-8"))
            frozen = {int(row["max_depth"]): row for row in document["cases"]}
            if self.n not in frozen:
                raise common.PerfError(f"frozen binarytrees oracle lacks n={self.n}")
            self.oracle_case = BINARYTREES_PREPARE.oracle_case(self.n)
            self.oracle_verification = self.generator.compare_oracle(
                self.n, frozen[self.n], self.oracle_case)
            record = {"path": str(oracle), "sha256": expected_sha,
                      "method": document.get("method")}
            files = [oracle]
        self.oracle_record = record
        for path in files:
            path = Path(path).resolve()
            self.inputs[str(path)] = common.sha256_file(path)

    def save(self):
        common.write_json(self.out / "programs.json", {
            "algorithm": self.algorithm, "n": self.n,
            "inputs": self.inputs, "templates": self.templates,
            "tools": self.tools, "oracle": self.oracle_record,
            "generated": self.generated, "builds": self.builds,
            "actual_work_review": "pending", "formal_timing": False})

    def unchanged(self):
        for path, sha in {**self.inputs, **self.templates, **self.tools}.items():
            try:
                actual = common.sha256_file(path)
            except OSError as error:
                raise common.PerfError(
                    f"frozen input/template/tool is missing: {path}: {error}") from error
            if actual != sha:
                raise common.PerfError(f"frozen input/template/tool changed: {path}")

    def _generated_unchanged(self):
        for row in self.generated.values():
            for key in ("source", "reference", "manifest"):
                info = row[key]
                if common.sha256_file(info["path"]) != info["sha256"]:
                    raise common.PerfError(
                        f"generated source changed: {info['path']}")

    def _generate(self, root, rounds):
        if root.exists():
            raise common.PerfError(f"round output already exists: {root}")
        if self.algorithm == "sieve":
            return self.generator.generate_case(
                root, self.template_text, self.n, rounds,
                self.expected_count, self.expected_sum, "correct")
        if self.algorithm == "fannkuch":
            return self.generator.generate_case(
                root, self.template_text, self.n, rounds, self.expected, "correct")
        if self.algorithm == "mandelbrot":
            return self.generator.generate_case(
                root, self.template_text, self.n, rounds, self.payload, "correct")
        if self.algorithm == "binarytrees":
            return self.generator.make_case(
                root, self.template_text, self.oracle_case, "correct", rounds)
        return self.generator.generate_case(
            root, self.template_text, self.n, rounds, self.literals, "correct")

    def _case_record(self, case, rounds):
        row = {
            "algorithm": self.algorithm, "n": self.n, "rounds": rounds,
            "variant": "correct",
            "source": case["source"], "reference": case["reference"],
            "manifest": case["manifest"],
            "inputs": dict(self.inputs), "templates": dict(self.templates),
            "tools": dict(self.tools), "cache_reuse": False,
            "generator": case,
        }
        for key in ("expected", "expected_file", "expected_bits"):
            if key in case:
                row[key] = case[key]
        return row

    def render(self, rounds):
        if (type(rounds) is not int or rounds < 1
                or rounds > 2 * formal_sampling.MAX_ROUNDS
                or rounds & (rounds - 1)):
            raise common.PerfError("R must be a supported power of two")
        self.unchanged()
        self._generated_unchanged()
        if rounds not in self.generated:
            self.generated[rounds] = self._case_record(
                self._generate(self.out / f"r{rounds}", rounds), rounds)
            self.save()
        self._generated_unchanged()
        self.unchanged()
        return self.generated[rounds]

    def build_cpp(self, rounds):
        row = {"rounds": rounds, "status": "running", "commands": [],
               "debug": False, "tools": dict(self.tools)}
        self.builds.append(row)
        try:
            case = self.render(rounds)
            row.update({key: case[key] for key in
                        ("source", "reference", "manifest")})
            self.save()
            program = formal_sort_programs.build_cpp_artifacts(
                rounds, case["reference"]["path"],
                self.out / f"r{rounds}" / "cpp-O2",
                self.llvm_bin, self.env, row, self.save)
            if self.algorithm == "mandelbrot":
                row["float_policy"] = self.generator.float_policy(
                    row["llvm"]["path"],
                    f"mandelbrot n={self.n} R{rounds} C++ O2")
            elif self.algorithm == "matmul":
                row["float_policy"] = self.generator.float_flags(
                    row["llvm"]["path"],
                    f"matmul n={self.n} R{rounds} C++ O2")
            self.unchanged()
            for key in ("source", "reference", "manifest"):
                info = case[key]
                if common.sha256_file(info["path"]) != info["sha256"]:
                    raise common.PerfError(
                        f"generated source changed during C++ build: {info['path']}")
            row["status"] = "built"
            return program
        except Exception as error:
            row["status"] = "failed"
            row["failure"] = {"type": type(error).__name__, "message": str(error)}
            raise
        finally:
            self.save()
