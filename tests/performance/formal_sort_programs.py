"""Generate preserved R/2R sort programs from the frozen full input files.

This is the build callback for formal_sampling.calibrate. It only builds C++;
Source compilation, actual-work review and timing are separate steps.
"""
from pathlib import Path
import struct

import formal_sampling
import run_sort

PINNED = {
    65536: ("a7e1143463245d0c84d4b1ffffeea8e5a00b215b00aef9e57794c3386deedd96",
            "feeb73ff75b1af7bd585b049ef8f3ae902634aa37402bf935acda53c4bf02256"),
    1048576: ("6ea2059cee402463d75d335dbed22021a275aa744848cacfe7437455e6c19240",
              "965f3aea3675c90c3bfc51f104e13bacf8cb65ba965c37d9f3c2e5b8d1e7e9ef"),
}
SEED = 0x6D2B79F5
common = run_sort.common


class SortPrograms:
    def __init__(self, algorithm, n, oracle_dir, out, llvm_bin, env):
        if algorithm not in run_sort.ALGORITHMS or type(n) is not int or n not in PINNED:
            raise common.PerfError("only the three fixed sorts and their two sizes are supported")
        self.algorithm, self.n = algorithm, n
        self.out, self.llvm_bin, self.env = Path(out), Path(llvm_bin), dict(env)
        if self.out.exists():
            raise common.PerfError(f"output already exists: {self.out}")
        self.inputs = {}
        arrays = []
        for name, expected in zip(("input", "expected"), PINNED[n]):
            path = Path(oracle_dir) / f"sort-n{n}-{name}.u32le"
            data = path.read_bytes()
            if len(data) != 4 * n or common.sha256_file(path) != expected:
                raise common.PerfError(f"frozen array differs: {path}")
            self.inputs[str(path)] = expected
            arrays.append(struct.unpack(f"<{n}I", data))
        self.raw, self.golden = arrays
        self.templates = run_sort.templates(algorithm)
        config = run_sort.algorithm_config(algorithm)
        for name in self.templates:
            path = config["directory"] / name
            self.inputs[str(path)] = common.sha256_file(path)
        self.tools = {}
        for name in ("clang++", "ld.lld", "opt"):
            path = self.llvm_bin / name
            self.tools[str(path)] = common.sha256_file(path)
        self.generated, self.builds = {}, []
        self.out.mkdir(parents=True, exist_ok=False)
        self.save()

    def save(self):
        common.write_json(self.out / "programs.json", {
            "algorithm": self.algorithm, "n": self.n, "seed": SEED,
            "inputs": self.inputs, "tools": self.tools,
            "generated": self.generated, "builds": self.builds,
            "actual_work_review": "pending", "formal_timing": False})

    def unchanged(self):
        for path, sha in {**self.inputs, **self.tools}.items():
            if common.sha256_file(path) != sha:
                raise common.PerfError(f"frozen input/tool changed: {path}")

    def render(self, rounds):
        if (type(rounds) is not int or rounds < 1 or rounds > 2 * formal_sampling.MAX_ROUNDS
                or rounds & (rounds - 1)):
            raise common.PerfError("R must be a supported power of two")
        self.unchanged()
        if rounds not in self.generated:
            case = run_sort.generate_sources(
                self.out / f"r{rounds}", self.templates, self.raw, self.golden,
                {"n": self.n, "seed": SEED, "rounds": rounds}, "source", self.algorithm)
            self.generated[rounds] = case
            self.save()
        case = self.generated[rounds]
        for key in ("manifest", "source", "reference"):
            info = case[key]
            if common.sha256_file(info["path"]) != info["sha256"]:
                raise common.PerfError(f"generated source changed: {info['path']}")
        return case

    def build_cpp(self, rounds):
        case = self.render(rounds)
        directory = self.out / f"r{rounds}" / "cpp-O2"
        directory.mkdir(exist_ok=False)
        program, ir, obj = directory / "reference", directory / "reference.ll", directory / "reference.o"
        row = {"rounds": rounds, "status": "running", "commands": []}
        self.builds.append(row)
        try:
            flags = [self.llvm_bin / "clang++", *run_sort.TARGET_FLAGS, "-std=c++20", "-O2"]
            commands = [
                flags + ["-c", case["reference"]["path"], "-o", obj],
                flags + [f"--ld-path={self.llvm_bin / 'ld.lld'}", obj, "-o", program],
                flags + ["-S", "-emit-llvm", case["reference"]["path"], "-o", ir],
                [self.llvm_bin / "opt", "-passes=verify", "-disable-output", ir],
            ]
            for argv in commands:
                result = common.command(argv, timeout=600, env=self.env)
                row["commands"].append(result)
                self.save()
                common.require_success(result, f"C++ R{rounds} build/export/verify")
            row["program"] = run_sort.file_rec(program)
            row["object"] = run_sort.file_rec(obj)
            row["llvm"] = run_sort.file_rec(ir)
            row["object_elf"] = common.checked_elf(obj, executable=False, label=f"C++ R{rounds} object")
            row["elf"] = common.checked_elf(program, executable=True, label=f"C++ R{rounds}")
            self.unchanged()
            row["status"] = "built"
            return program
        except Exception as error:
            row["status"] = "failed"
            row["failure"] = {"type": type(error).__name__, "message": str(error)}
            raise
        finally:
            self.save()
