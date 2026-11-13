#!/usr/bin/env python3
"""Verify the pure text/identity half of the self-hosted Python emitter.

The repository tree is never modified.  stage0 first syntax-checks every
production file of selfhost/src/wzcc/pythonemit.  A copied project then
contains only api.wz/writer.wz (the helpers that do not depend on the shared
valueops classifier or the verifier gate) plus this directory's probe.wz and
main.wz; accepted packages are symlinked from the real source tree.  The probe
is compiled at O0 and O2 and its output is compared field by field against the
exact C++ emitter spelling rules.

The full pythonemit package imports app/wzcc/valueops and app/wzcc/hirverify.
While either is missing, complete package check, generated-code/runtime/
source_map/symbol comparison and Python 3.12 execution are reported as
not_run; no stub, fabricated classifier or private verified module is used.

When a real app/wzcc/valueops package and the current C++ build artifacts are
available, an additional differential probe copies the production files into a
disk-backed project (removing only the hirverify import and the public emit
function), builds independent.wz at O0 and O2, and compares 19 internal helper
cases against the current src/backend/python/emitter.cpp oracle.  Missing
dependencies are printed as not_run, never replaced by stubs.  The real
hirverify-backed public entry and full-corpus Python 3.12 execution remain
not_run until hirverify is accepted.
"""

from __future__ import annotations

import argparse
import difflib
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
DEFAULT_TIMEOUT = 300.0


def main_checkout() -> pathlib.Path:
    """Resolve the primary checkout when running from a linked worktree."""
    try:
        result = subprocess.run(
            ["git", "-C", str(ROOT), "rev-parse", "--path-format=absolute",
             "--git-common-dir"], text=True, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, check=False)
        if result.returncode == 0 and result.stdout.strip():
            return pathlib.Path(result.stdout.strip()).parent
    except OSError:
        pass
    return ROOT


def default_stage0() -> pathlib.Path:
    local = ROOT / "build" / "woozy-stdlib-native" / "woozyc"
    if local.is_file():
        return local
    primary = main_checkout()
    if primary != ROOT:
        return primary / "build" / "woozy-stdlib-native" / "woozyc"
    return local


def default_toolchain() -> pathlib.Path:
    local = ROOT.parent / "toolchains"
    if local.is_dir():
        return local
    primary = main_checkout()
    if primary != ROOT:
        return primary.parent / "toolchains"
    return local


DEFAULT_STAGE0 = default_stage0()
DEFAULT_TOOLCHAIN = default_toolchain()

PRODUCTION = ROOT / "selfhost" / "src" / "wzcc" / "pythonemit"
PURE_FILES = ["api.wz", "writer.wz"]
ACCEPTED_PACKAGES = ["model", "hir", "types", "system"]

# Internal-helper differential cases.  Each name appears once in the Wz fixture
# and once in the C++ oracle.  The first ten are the independent expression
# cases from the review, followed by nonzero-NodeId controls and the compound
# assignment controls.
DIFFERENTIAL_CASES = [
    "control_integer",
    "unary_node0",
    "unary_node1",
    "binary_node0",
    "binary_node12",
    "convert_node0",
    "convert_node1",
    "index_node0",
    "index_node1",
    "slice_node0",
    "slice_node1",
    "field_node0",
    "field_node1",
    "for_in_node0",
    "for_in_node1",
    "for_in_slice_node0",
    "for_in_slice_node1",
    "compound_assign_node0",
    "compound_assign_node1",
]

# These fragments prove that the NodeId operand was resolved to its real type
# instead of accidentally selecting a different TypeId.  They are asserted in
# addition to exact C++/Wz equality so two identical failures cannot pass.
DIFFERENTIAL_FRAGMENTS = {
    "control_integer": ["_t0_0 = 5"],
    "unary_node0": ["_rt.unary('neg', _t0_0, 8, True)"],
    "unary_node1": ["_rt.unary('neg', _t0_0, 8, True)"],
    "binary_node0": ["_rt.binary('add', _t0_0, _t0_1, 8, True)"],
    "binary_node12": ["_rt.binary('add', _t0_0, _t0_1, 8, True)"],
    "convert_node0": ["_rt.convert(_t0_0, 32, False)"],
    "convert_node1": ["_rt.float_to_int(_t0_0, 8, True)"],
    "index_node0": ["_rt.text_at(_t0_0, _t0_1)"],
    "index_node1": ["_rt.text_at(_t0_0, _t0_1)"],
    "slice_node0": [".sub(None, None)"],
    "slice_node1": [".sub(None, None)"],
    "field_node0": ["_t0_0[0]"],
    "field_node1": ["_t0_0[0]"],
    "for_in_node0": ["_frame.put((4, 2), _v3",
                     "_rt.text_at(_t0_0, _t0_2)"],
    "for_in_node1": ["_frame.put((4, 3), _v3",
                     "_rt.text_at(_t0_0, _t0_2)"],
    "for_in_slice_node0": ["_frame.put((4, 2), _v13"],
    "for_in_slice_node1": ["_frame.put((4, 3), _v13"],
    "compound_assign_node0": [
        "_rt.binary('add', _t0_1, _t0_2, 8, True)",
        "_t0_0.value = _t0_3",
    ],
    "compound_assign_node1": [
        "_rt.binary('add', _t0_1, _t0_2, 8, True)",
        "_t0_0.value = _t0_3",
    ],
}

FORBIDDEN_IMPORTS = [
    'import "std/fs"',
    'import "std/process"',
    'import "std/os"',
    'import "std/thread"',
    'import "std/async"',
    "subprocess",
    "__import__",
    "os.system",
    "os.exec",
]


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def environment(toolchain: pathlib.Path) -> dict[str, str]:
    env = dict(os.environ)
    parts = [
        str(toolchain / "llvm-22.1.8" / "lib"),
        str(toolchain / "libxml2-legacy-2.13.9" / "usr" / "lib"),
    ]
    previous = env.get("LD_LIBRARY_PATH")
    if previous:
        parts.append(previous)
    env["LD_LIBRARY_PATH"] = ":".join(parts)
    return env


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Syntax-check and probe the pure Python emitter helpers.")
    parser.add_argument("--woozyc", default=str(DEFAULT_STAGE0),
                        help=f"stage0 compiler (default: {DEFAULT_STAGE0})")
    parser.add_argument("--toolchain-root", default=str(DEFAULT_TOOLCHAIN),
                        help=f"LLVM/libxml2 toolchain root (default: {DEFAULT_TOOLCHAIN})")
    parser.add_argument("--work-dir", default=None,
                        help="output/copy directory (default: a fresh temp dir)")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT,
                        help=f"timeout in seconds for each command (default: {DEFAULT_TIMEOUT:g})")
    parser.add_argument("--valueops-dir",
                        default=os.environ.get("WOOZY_VALUEOPS_DIR", str(PRODUCTION.parent / "valueops")),
                        help="real app/wzcc/valueops package used by the "
                             "internal differential probe; without it that "
                             "probe is reported as not_run")
    parser.add_argument("--cpp-root", default=None,
                        help="C++ checkout with src/backend/python/emitter.cpp "
                             "(default: the main checkout)")
    parser.add_argument("--cpp-build", default=None,
                        help="build directory containing liblang_{hir,modules,"
                             "frontend,sema}.a (default: "
                             "<cpp-root>/build/woozy-stdlib-native)")
    return parser.parse_args(argv)


def run(command: list[str], label: str, work: pathlib.Path,
        env: dict[str, str], timeout: float) -> subprocess.CompletedProcess[str]:
    print("+", " ".join(command), flush=True)
    try:
        return subprocess.run(
            command, cwd=work, env=env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
            timeout=timeout)
    except subprocess.TimeoutExpired:
        fail(f"{label}: timed out after {timeout:g}s")
    except OSError as error:
        fail(f"{label}: {error}")


def expect_ok(result: subprocess.CompletedProcess[str], label: str) -> None:
    if result.returncode != 0:
        fail(f"{label}: exit {result.returncode}\nstdout:\n{result.stdout}"
             f"stderr:\n{result.stderr}")
    print(f"PASS: {label}")


def py_lit(value: str) -> str:
    """The C++ emitter's string_literal() spelling."""
    return "b'" + "".join(f"\\x{byte:02x}" for byte in value.encode()) + \
        "'.decode('utf-8')"


def py_code(text: str) -> str:
    return text.replace("\n", "\\n")


def span(snapshot: int, begin: int, end: int) -> str:
    return f"{snapshot}:{begin}-{end}"


def expected_lines() -> list[str]:
    binding_info = (
        "{'runtime_version': " + py_lit("compiler-dev/1-python-values/10") +
        ", 'bundles': [{'path': " + py_lit("bindings/x.json") +
        ", 'sha256': " + py_lit("abc") +
        ", 'contract': " + py_lit("python-binding/1") +
        ", 'review_status': " + py_lit("reviewed") +
        ", 'allowed_backends': [" + py_lit("python") + ", " +
        py_lit("native") + "]" +
        ", 'package_requirements': [{'package': " + py_lit("numpy") +
        ", 'version_policy': " + py_lit("^2") + "}]" +
        ", 'modules': [{'logical_path': " + py_lit("std/np") +
        ", 'host_module': " + py_lit("numpy") +
        ", 'provider': " + py_lit("pypi") + "}]}]}")

    foreign_helpers = """\
def _lang_host_error(exc, file, begin_byte, end_byte, operation):
    return _rt.TypedError(_rt.ERROR_TYPE_KEY, _rt.Error(exc, file, begin_byte, end_byte, operation), _rt.ERROR_OPS)
def _lang_host_get(base, path, file, begin_byte, end_byte, operation, returns_text):
    try:
        value = base
        for attribute in path:
            value = getattr(value, attribute)
        return _rt._host_text(value) if returns_text else value
    except _rt.TypedError:
        raise
    except Exception as exc:
        raise _lang_host_error(exc, file, begin_byte, end_byte, operation) from exc
def _lang_host_set(base, path, value, file, begin_byte, end_byte, operation):
    try:
        target = base
        for attribute in path[:-1]:
            target = getattr(target, attribute)
        setattr(target, path[-1], value)
    except _rt.TypedError:
        raise
    except Exception as exc:
        raise _lang_host_error(exc, file, begin_byte, end_byte, operation) from exc
def _lang_host_getitem(base, key, file, begin_byte, end_byte, operation, returns_text):
    try:
        value = base[key]
        return _rt._host_text(value) if returns_text else value
    except _rt.TypedError:
        raise
    except Exception as exc:
        raise _lang_host_error(exc, file, begin_byte, end_byte, operation) from exc
def _lang_host_setitem(base, key, value, file, begin_byte, end_byte, operation):
    try:
        base[key] = value
    except _rt.TypedError:
        raise
    except Exception as exc:
        raise _lang_host_error(exc, file, begin_byte, end_byte, operation) from exc
"""

    return [
        "escape_empty=" + py_lit(""),
        "escape_ascii=" + py_lit("Ab"),
        "escape_unicode=" + py_lit("\u00e9\U0001d11e"),
        "escape_controls=" + py_lit("a\n\t\\\"'\x00"),
        "join_empty=",
        "join_single=a",
        "join_pair=a, b",
        "join_leading_empty=a",
        "join_all_empty=",
        "join_middle_empty=a, , c",
        "tuple_empty=()",
        "tuple_single=(x,)",
        "tuple_pair=(a, b,)",
        "operations=add,sub,mul,div,rem,shl,shr,and,or,xor,eq,ne,lt,le,gt,ge",
        "names=_s0,_f7,_v3,(0, 5),(1, 5)",
        "ordered=1,3,5,9|dup|yes|no",
        "foreign_key=@10:host.Thing|@2:\u00e9|@0:",
        "substring=weak_upgrade|",
        "specs=__woozy_weak_upgrade,__woozy_async_run,__woozy_vec_empty",
        "specs_missing=internal|unknown system operation",
        "printer_basic=" + py_code(
            "x = 1\n"
            "if x:\n"
            "    y = 2\n"
            "while True:\n"
            "    break\n"),
        "printer_basic_map=" + ";".join([
            "1:" + span(1, 1, 2),
            "2:" + span(1, 5, 6),
            "3:" + span(1, 3, 4),
            "4:" + span(1, 9, 10),
            "5:" + span(1, 7, 8),
        ]),
        "printer_basic_lines=5",
        "printer_chain=" + py_code(
            "if a:\n"
            "    pass1\n"
            "elif b:\n"
            "    pass2\n"
            "else:\n"
            "    pass3\n"),
        "printer_try=" + py_code(
            "try:\n"
            "    body\n"
            "except E:\n"
            "    handle\n"
            "finally:\n"
            "    clean\n"),
        "printer_empty=" + py_code("pass\n") + "|lines=1",
        "printer_checking_only=|||0",
        "printer_indent=rejected|2|generated Python indentation exceeds the "
        "supported profile (80)|-",
        "printer_depth=rejected|1|generated Python control blocks exceed the "
        "CPython 3.12 static limit (21)|" + span(1, 7, 8),
        "foreign_helpers=" + py_code(foreign_helpers),
        "type_keys=" + "|".join([
            "Unit", "bool", "string", "isize", "usize", "i8", "u32", "f32",
            "f64", "app/pkg.Thing", "app/pkg.Thing[i8]", "Vec[i8]",
            "Slice[Vec[i8]]",
            "func(inout i8,move u32)->(f32) throws(string)",
            "dyn std/traits.Show[bool]", "@10:host.Thing", "py.Error",
            "internal", "non-concrete Python error type identity",
            "app/pkg.Option[i8]",
        ]),
        "type_key_error=py.Error|@10:host.Thing",
        "type_key_bad_error=rejected|0|Python foreign error payload requires "
        "a runtime_error declaration|" + span(1, 5, 9),
        "type_key_missing=internal|missing canonical type 999",
        "numeric=0, False|8, True|32, False|32, False|64, False|64, True|"
        "64, False",
        "weak_targets=box|box|vec|no|no|thing",
        "weak_ops=i8|i8",
        "weak_bad=internal|verified weak.upgrade lost its Option target",
        "binding_info=" + binding_info,
        "source_path=src/main.wz",
        "foreign_location=" + py_lit("src/main.wz") + ", 10, 20, " +
        py_lit("host.Thing"),
    ]


def compare(label: str, expected: list[str], actual: str) -> None:
    expected_text = "\n".join(expected) + "\n"
    if expected_text == actual:
        print(f"PASS: {label}: {len(expected)} probe lines identical")
        return
    diff = "".join(difflib.unified_diff(
        expected_text.splitlines(keepends=True),
        actual.splitlines(keepends=True), fromfile="expected", tofile="wz",
        n=3))
    fail(f"{label}: probe output mismatch\n{diff[:12000]}")


def public_entry_static_check() -> None:
    """Guard the non-throwing public result shape while hirverify is absent.

    This is only a source-structure check; it is not a type check and does not
    replace the real hirverify-backed public-entry test reported as not_run.
    """
    path = PRODUCTION / "emit.wz"
    source = path.read_text(encoding="utf-8")
    signature = ("pub func emit(input hirverify.VerifiedHirModule, entry usize, "
                 "options Options,\n              runtime string) EmitResult {")
    if signature not in source:
        fail("public emit must keep its non-throwing EmitResult signature")
    snapshot = source.find("hirverify.snapshot(input)")
    if snapshot < 0:
        fail("public emit lost its verifier snapshot call")
    try_at = source.rfind("try {", 0, snapshot)
    if try_at < 0:
        fail("verifier snapshot is outside the public emit try block")
    catch_at = source.find("catch error model.SupportError", snapshot)
    if catch_at < 0:
        fail("public emit does not catch SupportError from snapshot")
    finish_at = source.find(
        "finish_failure(inout result, support_failure(error));", catch_at)
    if finish_at < 0:
        fail("SupportError catch does not publish a failed EmitResult")
    if "Output" in source[catch_at:finish_at]:
        fail("SupportError catch can publish Output")
    print("PASS: public emit source contract (static only; real hirverify "
          "check and runtime are not_run)")


def syntax_check(stage0: pathlib.Path, work: pathlib.Path,
                 env: dict[str, str], timeout: float) -> None:
    files = sorted(PRODUCTION.glob("*.wz"))
    if not files:
        fail(f"no production files under {PRODUCTION}")
    for path in files:
        result = run([str(stage0), "check", "--syntax-only", str(path)],
                     f"syntax {path.name}", work, env, timeout)
        expect_ok(result, f"syntax {path.name}")
        if "syntax ok" not in result.stdout:
            fail(f"syntax {path.name}: unexpected output {result.stdout!r}")


def static_scan() -> None:
    offenders: list[str] = []
    for path in sorted(PRODUCTION.glob("*.wz")):
        content = path.read_text(encoding="utf-8")
        for token in FORBIDDEN_IMPORTS:
            if token in content:
                offenders.append(f"{path.name}: {token}")
    if offenders:
        fail("production pythonemit uses host I/O or subprocesses: " +
             ", ".join(offenders))
    print("PASS: static scan: no filesystem/process imports in production "
          "pythonemit sources")


def copy_pure_project(work: pathlib.Path) -> pathlib.Path:
    project = work / "project"
    if project.exists():
        shutil.rmtree(project)
    package = project / "src" / "wzcc" / "pythonemit"
    package.mkdir(parents=True)
    for name in PURE_FILES:
        shutil.copy2(PRODUCTION / name, package / name)
    shutil.copy2(HERE / "probe.wz", package / "probe.wz")
    for name in ACCEPTED_PACKAGES:
        source = ROOT / "selfhost" / "src" / "wzcc" / name
        if not source.is_dir():
            fail(f"accepted dependency package missing: {source}")
        os.symlink(source, project / "src" / "wzcc" / name)
    shutil.copy2(HERE / "main.wz", project / "src" / "main.wz")
    shutil.copy2(HERE / "lang.toml", project / "lang.toml")
    return project


def pure_probe(stage0: pathlib.Path, work: pathlib.Path,
               env: dict[str, str], timeout: float) -> None:
    project = copy_pure_project(work)
    manifest = str(project / "lang.toml")
    checked = run([str(stage0), "check", "--project", manifest,
                   "--backend", "native"], "pure check", work, env, timeout)
    expect_ok(checked, "pure helper package check")
    outputs: dict[str, str] = {}
    expected = expected_lines()
    for opt in ("0", "2"):
        out = work / f"opt{opt}"
        if out.exists():
            shutil.rmtree(out)
        built = run([str(stage0), "build", "--project", manifest,
                     "--backend", "native", "--opt", opt, "--out-dir",
                     str(out)], f"pure build O{opt}", work, env, timeout)
        expect_ok(built, f"pure build O{opt}")
        program = out / "program"
        if not program.is_file():
            fail(f"O{opt}: missing {program}")
        values = run([str(program)], f"pure probe O{opt}", work, env, timeout)
        expect_ok(values, f"pure probe O{opt}")
        outputs[opt] = values.stdout
        compare(f"O{opt} probe output", expected, values.stdout)
    if outputs["0"] != outputs["2"]:
        fail("O0 and O2 probe outputs differ")


def dependency_probe(stage0: pathlib.Path, work: pathlib.Path,
                     env: dict[str, str], timeout: float) -> bool:
    """Check the whole package once its shared dependencies exist."""
    missing = [name for name in ("valueops", "hirverify")
               if not (ROOT / "selfhost" / "src" / "wzcc" / name).is_dir()]
    if missing:
        print("NOT_RUN: complete pythonemit package check, emit parity and "
              "Python 3.12 execution: missing app/wzcc/" + ", app/wzcc/".join(
                  missing))
        return False
    project = work / "integration"
    if project.exists():
        shutil.rmtree(project)
    (project / "src" / "wzcc").mkdir(parents=True)
    for source in sorted((ROOT / "selfhost" / "src" / "wzcc").iterdir()):
        if source.is_dir():
            os.symlink(source, project / "src" / "wzcc" / source.name)
    (project / "src" / "main.wz").write_text(
        'package app;\nimport "app/wzcc/pythonemit" as pythonemit;\n'
        "func main() i32 { return 0; }\n", encoding="utf-8")
    shutil.copy2(HERE / "lang.toml", project / "lang.toml")
    manifest = str(project / "lang.toml")
    result = run([str(stage0), "check", "--project", manifest,
                  "--backend", "native"], "integration check", work, env,
                 timeout)
    if result.returncode != 0:
        fail("complete pythonemit package check failed: exit "
             f"{result.returncode}\nstdout:\n{result.stdout}"
             f"stderr:\n{result.stderr}")
    print("PASS: complete pythonemit package check against real valueops/"
          "hirverify")
    print("NOT_RUN: emit code/runtime/source_map/symbol differential parity "
          "and Python 3.12 execution: needs the integrated driver that "
          "produces a VerifiedHirModule")
    return True


def decode_probe_literal(literal: str, label: str) -> str:
    """Decode the C++ emitter's string_literal() spelling used by both sides."""
    match = re.fullmatch(r"b'((?:\\x[0-9a-f]{2})*)'\.decode\('utf-8'\)",
                         literal)
    if not match:
        fail(f"{label}: unexpected generated literal {literal[:160]!r}")
    raw = bytes(int(value, 16) for value in
                re.findall(r"\\x([0-9a-f]{2})", match.group(1)))
    return raw.decode("utf-8")


def parse_differential(text: str, label: str) -> dict[str, str]:
    """Parse NAME=OK|<literal> lines and reject any FAIL/unknown spelling."""
    parsed: dict[str, str] = {}
    for line in text.splitlines():
        if "=" not in line:
            fail(f"{label}: malformed probe line {line[:160]!r}")
        name, rest = line.split("=", 1)
        if name in parsed:
            fail(f"{label}: duplicate case {name}")
        if rest.startswith("FAIL|"):
            fail(f"{label}: {name} did not generate Python: {rest}")
        if not rest.startswith("OK|"):
            fail(f"{label}: {name} has no explicit success marker: {rest}")
        code = decode_probe_literal(rest[3:], f"{label}/{name}")
        if not code:
            fail(f"{label}: {name} generated empty Python")
        if "missing canonical type" in code or "type has no numeric width" in code:
            fail(f"{label}: {name} leaked a type-resolution failure into output")
        parsed[name] = code
    expected = set(DIFFERENTIAL_CASES)
    actual = set(parsed)
    if actual != expected:
        fail(f"{label}: case set mismatch; missing={sorted(expected - actual)} "
             f"extra={sorted(actual - expected)}")
    for name, fragments in DIFFERENTIAL_FRAGMENTS.items():
        for fragment in fragments:
            if fragment not in parsed[name]:
                fail(f"{label}: {name} is missing {fragment!r}")
    return parsed


def copy_differential_project(work: pathlib.Path, valueops: pathlib.Path) -> pathlib.Path:
    project = work / "project"
    if project.exists():
        shutil.rmtree(project)
    package = project / "src" / "wzcc" / "pythonemit"
    package.mkdir(parents=True)
    packages = main_checkout() / "selfhost" / "src" / "wzcc"
    if not packages.is_dir():
        fail(f"accepted package tree missing: {packages}")
    for source in sorted(packages.iterdir()):
        if not source.is_dir() or source.name in ("pythonemit", "valueops"):
            continue
        os.symlink(source, project / "src" / "wzcc" / source.name)
    os.symlink(valueops, project / "src" / "wzcc" / "valueops")
    for name in ("api.wz", "writer.wz", "state.wz", "expr.wz", "stmt.wz",
                 "emit.wz"):
        shutil.copy2(PRODUCTION / name, package / name)
    # The verified-module gate is owned by hirverify, which is not present in
    # this checkout.  Remove only that import and the public emit function so
    # the real internal expression/statement implementation can be checked
    # against the current C++ emitter.  Nothing else is rewritten.
    emit = package / "emit.wz"
    text = emit.read_text(encoding="utf-8")
    import_line = 'import "app/wzcc/hirverify" as hirverify;\n'
    if import_line not in text:
        fail("internal projection lost the hirverify import")
    text = text.replace(import_line, "", 1)
    marker = "\npub func emit("
    index = text.find(marker)
    if index < 0:
        fail("internal projection could not find the public emit function")
    emit.write_text(text[:index] + "\n", encoding="utf-8")
    shutil.copy2(HERE / "probe.wz", package / "probe.wz")
    shutil.copy2(HERE / "independent.wz", package / "independent.wz")
    shutil.copy2(HERE / "differential_main.wz", project / "src" / "main.wz")
    shutil.copy2(HERE / "lang.toml", project / "lang.toml")
    return project


def build_cpp_oracle(cpp_root: pathlib.Path, cpp_build: pathlib.Path,
                     toolchain: pathlib.Path, base: pathlib.Path,
                     env: dict[str, str], timeout: float) -> pathlib.Path:
    source = cpp_root / "src" / "backend" / "python" / "emitter.cpp"
    if not source.is_file():
        fail(f"current C++ emitter missing: {source}")
    directory = base / "cpp"
    directory.mkdir(parents=True)
    original = source.read_text(encoding="utf-8")
    needle = "class Emitter {"
    if original.count(needle) != 1:
        fail(f"current C++ emitter shape changed around {needle!r}")
    (directory / "cpp_emitter_public.cpp").write_text(
        original.replace(needle, needle + "\npublic:", 1), encoding="utf-8")
    shutil.copy2(HERE / "oracle.cpp", directory / "oracle.cpp")
    libraries = [cpp_build / f"liblang_{name}.a"
                 for name in ("hir", "modules", "frontend", "sema")]
    missing = [str(path) for path in libraries if not path.is_file()]
    if missing:
        fail("C++ link libraries missing: " + ", ".join(missing))
    cxx = shutil.which(os.environ.get("CXX", "g++"))
    if not cxx:
        fail("no C++ compiler found")
    command = [cxx, "-std=c++20", "-O0",
               "-I" + str(cpp_root / "include"),
               "-I" + str(cpp_build / "src" / "backend" / "python"),
               "oracle.cpp", "-o", "oracle"]
    command += [str(path) for path in libraries]
    xml_lib = toolchain / "libxml2-legacy-2.13.9" / "usr" / "lib"
    if xml_lib.is_dir():
        command += ["-L", str(xml_lib)]
    command += ["-lxml2", "-lgmpxx", "-lgmp"]
    built = run(command, "C++ oracle build", directory, env, timeout)
    expect_ok(built, "C++ oracle build")
    return directory / "oracle"


def differential_probe(stage0: pathlib.Path, work: pathlib.Path,
                       env: dict[str, str], timeout: float,
                       valueops: str | None, cpp_root: pathlib.Path,
                       cpp_build: pathlib.Path,
                       toolchain: pathlib.Path) -> None:
    """Compare the real internal Wz emitter with the current C++ emitter."""
    if not valueops:
        print("NOT_RUN: internal Python-emitter differential: no real "
              "app/wzcc/valueops package supplied (--valueops-dir or "
              "WOOZY_VALUEOPS_DIR); no classifier stub was used")
        return
    valueops_path = pathlib.Path(valueops).expanduser().resolve()
    if not (valueops_path / "valueops.wz").is_file():
        print(f"NOT_RUN: internal Python-emitter differential: "
              f"{valueops_path}/valueops.wz is missing")
        return
    if not (cpp_root / "src" / "backend" / "python" / "emitter.cpp").is_file():
        print(f"NOT_RUN: internal Python-emitter differential: current C++ "
              f"emitter missing under {cpp_root}")
        return
    libraries = [cpp_build / f"liblang_{name}.a"
                 for name in ("hir", "modules", "frontend", "sema")]
    if any(not path.is_file() for path in libraries) or \
       not (cpp_build / "src" / "backend" / "python" / "runtime.hpp").is_file():
        print(f"NOT_RUN: internal Python-emitter differential: build "
              f"artifacts missing under {cpp_build}")
        return

    base = work / "differential"
    project = copy_differential_project(base, valueops_path)
    manifest = str(project / "lang.toml")
    checked = run([str(stage0), "check", "--project", manifest,
                   "--backend", "native"], "internal package check", work,
                  env, timeout)
    expect_ok(checked, "internal package check with real valueops")
    raw: dict[str, str] = {}
    for opt in ("0", "2"):
        out = base / f"wz-O{opt}"
        if out.exists():
            shutil.rmtree(out)
        built = run([str(stage0), "build", "--project", manifest,
                     "--backend", "native", "--opt", opt, "--out-dir",
                     str(out)], f"internal build O{opt}", work, env, timeout)
        expect_ok(built, f"internal build O{opt}")
        program = out / "program"
        if not program.is_file():
            fail(f"O{opt}: missing {program}")
        values = run([str(program)], f"internal probe O{opt}", work, env,
                     timeout)
        expect_ok(values, f"internal probe O{opt}")
        raw[opt] = values.stdout

    cpp_oracle = build_cpp_oracle(cpp_root, cpp_build, toolchain, base, env,
                                  timeout)
    cpp_values = run([str(cpp_oracle)], "C++ oracle run", cpp_oracle.parent,
                     env, timeout)
    expect_ok(cpp_values, "C++ oracle run")

    parsed = {
        "0": parse_differential(raw["0"], "Wz O0"),
        "2": parse_differential(raw["2"], "Wz O2"),
        "cpp": parse_differential(cpp_values.stdout, "C++ O0"),
    }
    if raw["0"] != raw["2"]:
        diff = "".join(difflib.unified_diff(
            raw["0"].splitlines(keepends=True),
            raw["2"].splitlines(keepends=True), fromfile="wz-O0",
            tofile="wz-O2", n=1))
        fail("Wz O0 and O2 differential output differ\n" + diff[:12000])
    if raw["0"] != cpp_values.stdout:
        diff = "".join(difflib.unified_diff(
            cpp_values.stdout.splitlines(keepends=True),
            raw["0"].splitlines(keepends=True), fromfile="current-C++",
            tofile="wz-O0", n=1))
        fail("Wz and current C++ differential output differ\n" + diff[:12000])
    if parsed["0"] != parsed["2"] or parsed["0"] != parsed["cpp"]:
        fail("decoded differential programs differ despite identical text")
    print(f"PASS: internal Python-emitter differential: "
          f"{len(DIFFERENTIAL_CASES)} cases, Wz O0 == Wz O2 == current C++ "
          f"emitter; NodeId 0 and non-zero operands verified")


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    stage0 = pathlib.Path(args.woozyc).expanduser().resolve()
    toolchain = pathlib.Path(args.toolchain_root).expanduser().resolve()
    if not stage0.is_file():
        fail(f"stage0 compiler missing: {stage0}")
    if args.timeout <= 0:
        fail(f"--timeout must be positive, got {args.timeout:g}")
    if args.work_dir:
        work = pathlib.Path(args.work_dir).expanduser().resolve()
        work.mkdir(parents=True, exist_ok=True)
    else:
        work = pathlib.Path(tempfile.mkdtemp(prefix="pythonemit-tests-"))
    env = environment(toolchain)
    # Disk-backed workdir for every compiler temporary file.
    env["TMPDIR"] = str(work)
    cpp_root = pathlib.Path(args.cpp_root).expanduser().resolve() \
        if args.cpp_root else main_checkout()
    cpp_build = pathlib.Path(args.cpp_build).expanduser().resolve() \
        if args.cpp_build else cpp_root / "build" / "woozy-stdlib-native"
    print(f"stage0: {stage0}")
    print(f"work-dir: {work}")
    print(f"cpp-root: {cpp_root}")

    syntax_check(stage0, work, env, args.timeout)
    static_scan()
    public_entry_static_check()
    pure_probe(stage0, work, env, args.timeout)
    differential_probe(stage0, work, env, args.timeout, args.valueops_dir,
                       cpp_root, cpp_build, toolchain)
    dependency_probe(stage0, work, env, args.timeout)
    if not (ROOT / "selfhost" / "src" / "wzcc" / "hirverify").is_dir():
        print("NOT_RUN: public pythonemit.emit(...) type check and all-corpus "
              "Python 3.12 execution: app/wzcc/hirverify is absent; no stub "
              "verifier or fabricated VerifiedHirModule was used")
    print("DONE: pure Python-emitter helpers verified at O0 and O2; "
          "internal differential and any not_run items are reported above")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
