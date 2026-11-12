#!/usr/bin/env python3
"""Run one generated Weak program with host GC disabled.

CPython's automatic collector is disabled before ``langrt`` is imported and
``gc.collect`` is replaced with an error.  Acyclic Source Weak programs must
therefore release every strong target through Woozy copy/drop alone; the final
``_LIVE_VECS`` set and ``_gc_stats`` prove that no logical Vec owner leaked.
"""
from __future__ import annotations

import contextlib
import gc
import io
import json
import runpy
import sys
from pathlib import Path

sys.dont_write_bytecode = True


def weak_runtime_regressions(rt):
    """Direct runtime checks that do not replace the Source acceptance runs."""
    ops = rt.ValueType()
    value = rt.Vec.create(ops, [1])
    handle = rt.Weak(value)
    upgraded = handle.upgrade()
    assert upgraded is value
    upgraded.release()
    value.release()
    # Weak is not a strong edge: after the last language owner is gone the
    # control block is closed even though the Python Weak object still exists.
    assert handle.upgrade() is None
    assert rt.Weak().upgrade() is None
    assert len(rt._LIVE_VECS) == 0


def main() -> int:
    program_dir = Path(sys.argv[1]).resolve()
    expected_return = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    gc.disable()
    if gc.isenabled():
        raise AssertionError("CPython automatic GC is still enabled")

    def forbid_host_collection(*args, **kwargs):
        raise AssertionError("generated Weak programs must not call gc.collect()")

    gc.collect = forbid_host_collection
    sys.path.insert(0, str(program_dir))
    import langrt as rt

    program_stdout, program_stderr = io.StringIO(), io.StringIO()
    exit_code = 0
    escaped = None
    with contextlib.redirect_stdout(program_stdout), \
            contextlib.redirect_stderr(program_stderr):
        try:
            runpy.run_path(str(program_dir / "program.py"), run_name="__main__")
        except SystemExit as stopped:
            exit_code = 0 if stopped.code is None else stopped.code
        except BaseException as error:
            escaped = f"{type(error).__name__}: {error}"
            exit_code = "uncaught"

    result = {
        "status": "passed",
        "exit": exit_code,
        "escaped": escaped,
        "program_stdout": program_stdout.getvalue(),
        "program_stderr": program_stderr.getvalue(),
        "stats": rt._gc_stats(),
        "live_vectors": len(rt._LIVE_VECS),
    }
    try:
        assert escaped is None, result
        assert exit_code == expected_return, result
        assert program_stdout.getvalue() == "", result
        assert program_stderr.getvalue() == "", result
        assert result["stats"]["active"] is False, result
        assert result["stats"]["busy"] == 0, result
        assert result["stats"]["live"] == 0, result
        assert result["live_vectors"] == 0, result
        weak_runtime_regressions(rt)
        result["stats_after_probe"] = rt._gc_stats()
        result["live_vectors_after_probe"] = len(rt._LIVE_VECS)
        assert result["stats_after_probe"]["live"] == 0, result
        assert result["live_vectors_after_probe"] == 0, result
    except AssertionError as error:
        result["status"] = "failed"
        result["error"] = str(error)
    print(json.dumps(result, sort_keys=True))
    return 0 if result["status"] == "passed" else 1


if __name__ == "__main__":
    sys.exit(main())
