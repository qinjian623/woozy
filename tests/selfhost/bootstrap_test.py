"""Small checks for bootstrap subprocess auditing and source freezing."""

import importlib.util
from pathlib import Path
import tempfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("bootstrap", ROOT / "selfhost/bootstrap.py")
bootstrap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bootstrap)

with tempfile.TemporaryDirectory(prefix="woozy-bootstrap-check-") as directory:
    work = Path(directory)
    trace = work / "exec.log"
    compiler = work / "stage1/woozyc"
    llvm = work / "toolchain/clang"
    allowed = {str(compiler), str(llvm)}
    trace.write_text(f'123 execve("{compiler}", ["woozyc"], 0x0) = 0\n'
                     f'124 execve("{llvm}", ["clang"], 0x0) = 0\n')
    assert bootstrap.trace_executables(trace, allowed) == sorted(allowed)
    for text in ('execve("/old/lang_native_helper", [], 0x0) = 0\n',
                 'execveat(3, "", [], 0x0, AT_EMPTY_PATH) = 0\n',
                 '+++ exited with 0 +++\n'):
        trace.write_text(text)
        try:
            bootstrap.trace_executables(trace, allowed)
        except RuntimeError:
            pass
        else:
            raise AssertionError(f"accepted unverified execution: {text}")
    source = work / "source"
    source.mkdir()
    (source / "main.wz").write_text("package main; func main() {}\n")
    (source / "unrelated.txt").write_text("excluded\n")
    copied = work / "copy"
    hashes = bootstrap.copy_sources(source, copied)
    assert set(hashes) == {"main.wz"}
    assert (source / "main.wz").read_bytes() == (copied / "main.wz").read_bytes()
    assert not (copied / "unrelated.txt").exists()

print("PASS: bootstrap execution audit and frozen source copy")
