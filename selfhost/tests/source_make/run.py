#!/usr/bin/env python3
"""Cold/warm/private/public/corrupt checks for the source-level Make path.

Run with ``python3 -B selfhost/tests/source_make/run.py --woozyc BIN``.
The compiler must have its normal toolchain/runtime/stdlib resources next to
it, including LLVM 22.1.8; inherited LD_LIBRARY_PATH is preserved.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
EXPECTED = 19
PRIVATE_EXPECTED = 24


def run(command, cwd, env, timeout=600):
    print("$", " ".join(command), flush=True)
    result = subprocess.run(
        command, cwd=cwd, env=env, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True, timeout=timeout,
    )
    if result.returncode != 0:
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
    return result


def build(woozyc, project, out_dir, cache_dir, env, cwd, opt="2", backend=None):
    command = [
        str(woozyc), "build", "--project", str(project),
        "--out-dir", str(out_dir), "--source-make", "--opt", opt,
        "--format", "json",
    ]
    if cache_dir is not None:
        command[6:6] = ["--cache-dir", str(cache_dir)]
    if backend is not None:
        command.extend(["--backend", backend])
    return run(command, cwd, env)


def stats(out_dir):
    return json.loads((out_dir / "cache-statistics.json").read_text())


def check(condition, message, failures):
    if not condition:
        failures.append(message)


def check_nominal(woozyc, root, cache_dir, env, failures):
    project = root / "nominal"
    shutil.copytree(HERE / "nominal", project)
    manifest = project / "lang.toml"
    out_dir = root / "nominal-out"
    layout = project / "src/model/types.wz"
    cases = (("O0", "0", None), ("O2", "2", None),
             ("public layout edit", "2", ("pub right i64", "pub right i32")),
             ("private layout edit", "2", ("key i32", "key i64")))
    for label, opt, change in cases:
        if change is not None:
            layout.write_text(layout.read_text().replace(*change))
        result = build(woozyc, manifest, out_dir, cache_dir, env, root, opt=opt)
        check(result.returncode == 0, f"nominal {label} build failed", failures)
        if result.returncode != 0:
            continue
        executed = run([str(out_dir / "program")], root, env)
        check(executed.returncode == 37, f"nominal {label} program value", failures)
        if change is not None:
            check(stats(out_dir)["opt_jobs"] == 2,
                  f"{label} did not recompile both packages", failures)

def check_partitioned(woozyc, root, env, failures):
    tools = woozyc.parent / "toolchain"
    if not all((tools / name).is_file() for name in ("llvm-split", "llvm-objcopy")):
        return
    project = root / "partitioned"
    for module in ("left", "right"):
        source = project / "src" / module
        source.mkdir(parents=True)
        fillers = "\n".join(f"func item_{i}() i32 {{ return {i}; }}" for i in range(511))
        (source / "module.wz").write_text(
            f'package {module};\npub func value() string {{ return "{module}"; }}\n{fillers}\n',
        )
    manifest = project / "lang.toml"
    shutil.copy2(HERE / "lang.toml", manifest)
    (project / "src/main.wz").write_text(
        'package app;\nimport "app/left" as left;\nimport "app/right" as right;\n'
        'func main() i32 { if left.value() == "left" && right.value() == "right" '
        '{ return 61; } return 1; }\n',
    )
    output = root / "partitioned-out"
    result = build(woozyc, manifest, output, None, env, root)
    check(result.returncode == 0, "partitioned packages failed to link", failures)
    if result.returncode != 0:
        return
    packages = json.loads((output / "make-report.json").read_text())["packages"]
    for package in packages:
        if package["logical_path"] in ("app/left", "app/right"):
            check((package["opt_jobs"], package["llc_jobs"]) == (17, 16),
                  f'{package["logical_path"]} did not use verified partitioning', failures)
    executed = run([str(output / "program")], root, env)
    check(executed.returncode == 61, "partitioned packages changed program value", failures)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--woozyc", type=Path,
        default=Path(os.environ.get("WOOZYC", ROOT / "build/woozy-stdlib-native/woozyc")),
    )
    args = parser.parse_args()
    woozyc = args.woozyc.resolve()
    env = os.environ.copy()
    env["MAKEFLAGS"] = "-j3"
    failures = []

    with tempfile.TemporaryDirectory(prefix="woozy-source-make-") as temporary:
        root = Path(temporary)
        project = root / "project"
        shutil.copytree(HERE / "src", project / "src")
        shutil.copy2(HERE / "lang.toml", project / "lang.toml")
        manifest = project / "lang.toml"
        out_dir = root / "out"
        cache_dir = root / "cache"
        math_file = project / "src/libmath/math.wz"
        original_math = math_file.read_text()

        capture = root / "preflight-capture"
        capture.write_bytes(b"caller-owned file")
        cold = build(woozyc, manifest, out_dir, cache_dir, env, root)
        check(cold.returncode == 0, "cold build failed", failures)
        check(capture.read_bytes() == b"caller-owned file",
              "preflight touched the caller's directory", failures)
        if cold.returncode == 0:
            cold_stats = stats(out_dir)
            check(cold_stats["checked_bodies"] == 4, "cold body count", failures)
            check(cold_stats["lowered_functions"] == 4, "cold lowering count", failures)
            check(cold_stats["opt_jobs"] == 3, "cold opt count", failures)
            check(cold_stats["llc_jobs"] == 3, "cold llc count", failures)
            check(cold_stats["object_hits"] == 0, "cold object hits", failures)
            executed = run([str(out_dir / "program")], root, env)
            check(executed.returncode == EXPECTED, "cold program value", failures)
            makefile = (out_dir / "Makefile").read_text()
            check(str(root) not in makefile, "Makefile contains a real path", failures)
            for name in ("make.log", "combine.log", "link.log"):
                check((out_dir / name).is_file(), f"missing {name}", failures)

        warm = build(woozyc, manifest, out_dir, cache_dir, env, root)
        check(warm.returncode == 0, "warm build failed", failures)
        if warm.returncode == 0:
            warm_stats = stats(out_dir)
            check(warm_stats["checked_bodies"] == 0, "warm body checks", failures)
            check(warm_stats["lowered_functions"] == 0, "warm lowering", failures)
            check(warm_stats["opt_jobs"] == 0, "warm opt", failures)
            check(warm_stats["llc_jobs"] == 0, "warm llc", failures)
            check(warm_stats["object_hits"] == 3, "warm object hits", failures)
            packages = json.loads((out_dir / "make-report.json").read_text())["packages"]
            check(len(packages) == 3 and all(p["declaration_scope"] == "none" for p in packages),
                  "warm workers ran name resolution", failures)
            executed = run([str(out_dir / "program")], root, env)
            check(executed.returncode == EXPECTED, "warm program value", failures)

        math_file.write_text(original_math.replace("value * 2", "value * 3"))
        private = build(woozyc, manifest, out_dir, cache_dir, env, root)
        check(private.returncode == 0, "private-body build failed", failures)
        if private.returncode == 0:
            private_stats = stats(out_dir)
            check(private_stats["checked_bodies"] == 1, "private body count", failures)
            check(private_stats["opt_jobs"] == 1, "private opt count", failures)
            check(private_stats["llc_jobs"] == 1, "private llc count", failures)
            check(private_stats["object_hits"] == 2, "private object hits", failures)
            executed = run([str(out_dir / "program")], root, env)
            check(executed.returncode == PRIVATE_EXPECTED, "private program value", failures)

        old_make_log = (out_dir / "make.log").read_text()
        math_file.write_text(original_math.replace("value * 2", "unknown_in_math"))
        unknown = build(woozyc, manifest, out_dir, cache_dir, env, root)
        check(unknown.returncode != 0 and "E_NAME_NOT_FOUND" in unknown.stdout,
              "worker missed the unresolved name in a module body", failures)
        failed_make_log = (out_dir / "make.log").read_text()
        check(failed_make_log != old_make_log and "source-make module start:" in failed_make_log,
              "body resolution error was diagnosed before Make ran", failures)

        math_file.write_text(original_math.replace(
            "pub func twice(value i32) i32 {",
            "pub func twice(value i32, extra i32) i32 {",
        ))
        public = build(woozyc, manifest, out_dir, cache_dir, env, root)
        check(public.returncode != 0, "public signature change unexpectedly built", failures)
        check("main.wz" in public.stdout + public.stderr,
              "public signature diagnostic is not in importer", failures)
        preserved = run([str(out_dir / "program")], root, env)
        check(preserved.returncode == PRIVATE_EXPECTED,
              "failed build replaced the previous program", failures)
        math_file.write_text(original_math)
        restored_warm = build(woozyc, manifest, out_dir, cache_dir, env, root)
        check(restored_warm.returncode == 0, "restored warm build failed", failures)

        status = json.loads((out_dir / "module-0000000000000000.json").read_text())
        used_object = cache_dir / "frontend" / status["object_key"]
        if used_object.is_file():
            used_object.write_bytes(b"broken")
            recovered = build(woozyc, manifest, out_dir, cache_dir, env, root)
            check(recovered.returncode == 0, "corrupt-object build failed", failures)
            if recovered.returncode == 0:
                recovered_stats = stats(out_dir)
                check(recovered_stats["object_hits"] == 2, "corrupt object hits", failures)
                check(recovered_stats["checked_bodies"] == 1, "corrupt recheck", failures)
                executed = run([str(out_dir / "program")], root, env)
                check(executed.returncode == EXPECTED, "corrupt recovery value", failures)

        entry = project / "src/main.wz"
        entry.write_text(entry.read_text().replace(
            "math.twice(5)", "math.twice(value: 5) + math.answer",
        ))
        constants = "\nconst hidden i32 = 9;\npub const answer i32 = hidden;\n"
        math_file.write_text(original_math + constants)
        named = build(woozyc, manifest, out_dir, cache_dir, env, root)
        check(named.returncode == 0, "cross-package named argument failed", failures)
        if named.returncode == 0:
            executed = run([str(out_dir / "program")], root, env)
            check(executed.returncode == EXPECTED + 9, "initial exported constant value", failures)
        math_file.write_text(original_math + constants.replace("= 9;", "= 10;"))
        changed_constant = build(woozyc, manifest, out_dir, cache_dir, env, root)
        check(changed_constant.returncode == 0, "private constant edit failed", failures)
        if changed_constant.returncode == 0:
            packages = json.loads((out_dir / "make-report.json").read_text())["packages"]
            misses = {p["logical_path"] for p in packages if not p["object_hit"]}
            check(misses == {"app", "app/libmath"}, "private constant did not invalidate importer", failures)
            executed = run([str(out_dir / "program")], root, env)
            check(executed.returncode == EXPECTED + 10, "importer used a stale constant", failures)

        manifest.write_text(manifest.read_text().replace(
            'backend = "native"', 'backend = "python"',
        ))
        override = build(woozyc, manifest, out_dir, None, env, root, backend="native")
        check(override.returncode == 0, "worker lost the Native backend override", failures)
        if override.returncode == 0:
            executed = run([str(out_dir / "program")], root, env)
            check(executed.returncode == EXPECTED + 10, "backend override program value", failures)
            report = json.loads((out_dir / "make-report.json").read_text())
            check(report.get("prepared_projects") == 1, "project was not prepared once", failures)
            check(len(report["packages"]) == 3 and all(
                package["declaration_scope"] == "shared-semantic-declarations"
                for package in report["packages"]
            ), "cold workers lost shared declaration preparation", failures)

        saved_make_log = (out_dir / "make.log").read_text()
        math_file.write_text(original_math.replace("value * 2", "unknown_in_math") + constants)
        uncached_unknown = build(woozyc, manifest, out_dir, None, env, root, backend="native")
        check(uncached_unknown.returncode != 0 and "E_NAME_NOT_FOUND" in uncached_unknown.stdout,
              "shared preparation lost the original unresolved-name diagnostic", failures)
        failed_make_log = (out_dir / "make.log").read_text()
        check(failed_make_log != saved_make_log and "source-make module start:" in failed_make_log,
              "uncached body resolution error bypassed Make workers", failures)
        preserved = run([str(out_dir / "program")], root, env)
        check(preserved.returncode == EXPECTED + 10, "failed uncached build replaced program", failures)

        check_nominal(woozyc, root, cache_dir, env, failures)
        check_partitioned(woozyc, root, env, failures)

    if failures:
        for failure in failures:
            print("FAIL:", failure, file=sys.stderr)
        return 1
    print("source_make: passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
