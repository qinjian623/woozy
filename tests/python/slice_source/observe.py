#!/usr/bin/env python3
"""Run one generated Python Slice program with independent runtime accounting.

CPython's automatic cycle collector is disabled before langrt is imported and
``gc.collect`` is replaced with an error.  The generated program must therefore
rely only on Woozy logical copy/drop plus the private Slice-aware collector.

The observer wraps only public/private helper methods that the generated code
already calls.  It does not change a Slice/Vec operation or its ordering.  The
reported counts prove logical retain/release and begin/end balance, and the
final ``_gc_stats``/``_LIVE_VECS`` prove no Vec or pin leaked.
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


def main() -> int:
    program_dir = Path(sys.argv[1]).resolve()
    panic_mode = len(sys.argv) > 2 and sys.argv[2] == "panic"
    gc.disable()
    if gc.isenabled():
        raise AssertionError("CPython automatic GC is still enabled")

    def forbid_host_collection(*args, **kwargs):
        raise AssertionError("generated Slice programs must not call gc.collect()")

    gc.collect = forbid_host_collection
    sys.path.insert(0, str(program_dir))
    import langrt as rt

    counts = {name: 0 for name in (
        "vec_create", "vec_retain", "vec_release", "vec_begin", "vec_end",
        "slice_retain", "slice_release", "slice_begin", "slice_end",
        "slice_create", "slice_copy", "slice_drop", "slice_retire",
        "slice_to_vec", "collector_release",
    )}
    state = {"in_slice": 0}

    def around_slice(original, key):
        def wrapper(*args, **kwargs):
            counts[key] += 1
            state["in_slice"] += 1
            try:
                return original(*args, **kwargs)
            finally:
                state["in_slice"] -= 1
        return wrapper

    def patch(cls, name, key, slice_operation=False):
        original = getattr(cls, name)
        if slice_operation:
            wrapped = around_slice(original, key)
        else:
            def wrapped(*args, **kwargs):
                counts[key] += 1
                return original(*args, **kwargs)
        setattr(cls, name, wrapped)
        return original

    vec_retain = getattr(rt.Vec, "retain")
    vec_release = getattr(rt.Vec, "release")
    vec_begin = getattr(rt.Vec, "begin")
    vec_end = getattr(rt.Vec, "end")

    def retain(self):
        counts["vec_retain"] += 1
        if state["in_slice"]:
            counts["slice_retain"] += 1
        return vec_retain(self)

    def release(self):
        counts["vec_release"] += 1
        if state["in_slice"]:
            counts["slice_release"] += 1
        elif rt._GC.active:
            counts["collector_release"] += 1
        return vec_release(self)

    def begin(self):
        counts["vec_begin"] += 1
        if state["in_slice"]:
            counts["slice_begin"] += 1
        return vec_begin(self)

    def end(self):
        counts["vec_end"] += 1
        if state["in_slice"]:
            counts["slice_end"] += 1
        return vec_end(self)

    vec_create = getattr(rt.Vec, "create")

    def create_vector(element, values):
        counts["vec_create"] += 1
        return vec_create(element, values)

    rt.Vec.create = staticmethod(create_vector)
    rt.Vec.retain, rt.Vec.release = retain, release
    rt.Vec.begin, rt.Vec.end = begin, end
    patch(rt.Slice, "copy", "slice_copy", slice_operation=True)
    patch(rt.Slice, "drop", "slice_drop", slice_operation=True)
    patch(rt.Slice, "_retire_counted_edge", "slice_retire", slice_operation=True)
    patch(rt.Slice, "to_vec", "slice_to_vec")
    create = getattr(rt.Slice, "create")

    def create_slice(owner, start, end):
        counts["slice_create"] += 1
        state["in_slice"] += 1
        try:
            return create(owner, start, end)
        finally:
            state["in_slice"] -= 1

    rt.Slice.create = staticmethod(create_slice)

    def identity_regressions():
        """Live views must keep owner.element; the argument is blank-only."""
        live_before = len(rt._LIVE_VECS)
        scalar = rt.ValueType()

        # Blank has no owner, so the supplied descriptor is the only metadata.
        blank_ops = rt.ValueType("vec")
        blank = rt.Slice.blank().to_vec(blank_ops)
        assert blank.element is blank_ops and blank.items == []
        blank.release()

        # A distinct but equivalent trivial descriptor must not be rejected or
        # substituted for the owner's authoritative descriptor.
        trivial = rt.ValueType()
        owner = rt.Vec.create(trivial, [17, 18])
        view = rt.Slice.create(owner, 0, 2)
        copied = view.to_vec(rt.ValueType())
        assert copied.element is trivial and copied.items == [17, 18]
        copied.release()
        view.drop()
        owner.release()

        # Same for Vec-owned elements: the result must use owner.element and
        # perform the real Vec copy (one retained inner edge), not erase it.
        owned = rt.ValueType("vec")
        inner = rt.Vec.create(scalar, [7])
        owner = rt.Vec.create(owned, [inner])
        view = rt.Slice.create(owner, 0, 1)
        before = inner.refs
        copied = view.to_vec(rt.ValueType("vec"))
        assert copied.element is owned and copied.items[0] is inner
        assert inner.refs == before + 1
        copied.release()
        assert inner.refs == before
        view.drop()
        owner.release()

        # An unrelated fallback for a live Vec-owned view must neither replace
        # owner.element nor lose the copied/retained inner container.
        owned2 = rt.ValueType("vec")
        inner2 = rt.Vec.create(scalar, [9])
        owner2 = rt.Vec.create(owned2, [inner2])
        view2 = rt.Slice.create(owner2, 0, 1)
        before2 = inner2.refs
        copied2 = view2.to_vec(rt.ValueType())
        assert copied2.element is owned2 and copied2.items[0] is inner2
        assert inner2.refs == before2 + 1
        copied2.release()
        assert inner2.refs == before2
        view2.drop()
        owner2.release()

        # The same fallback rule for a trivial live view.
        trivial2 = rt.ValueType()
        owner3 = rt.Vec.create(trivial2, [5])
        view3 = rt.Slice.create(owner3, 0, 1)
        copied3 = view3.to_vec(rt.ValueType("slice"))
        assert copied3.element is trivial2 and copied3.items == [5]
        copied3.release()
        view3.drop()
        owner3.release()

        assert len(rt._LIVE_VECS) == live_before
        return True

    program_stdout, program_stderr = io.StringIO(), io.StringIO()
    exit_code = 0
    escaped = None
    with contextlib.redirect_stdout(program_stdout), \
            contextlib.redirect_stderr(program_stderr):
        try:
            runpy.run_path(str(program_dir / "program.py"), run_name="__main__")
        except SystemExit as stopped:
            exit_code = 0 if stopped.code is None else stopped.code
        except BaseException as error:  # A panic must be translated by _rt.run.
            escaped = f"{type(error).__name__}: {error}"
            exit_code = "uncaught"

    stats = rt._gc_stats()
    live_vectors = len(rt._LIVE_VECS)

    # Direct runtime regressions: the blank fallback keeps the supplied
    # ValueType, while live views use owner.element even when the argument is a
    # distinct equivalent or unrelated descriptor. Snapshot counters first;
    # these probes are not part of the generated program's accounting.
    saved_counts = dict(counts)
    identity_ok = False
    identity_error = None
    try:
        identity_ok = identity_regressions()
    except Exception as error:
        identity_error = f"{type(error).__name__}: {error}"
    finally:
        counts.clear()
        counts.update(saved_counts)

    result = {
        "status": "passed",
        "exit": exit_code,
        "escaped": escaped,
        "program_stdout": program_stdout.getvalue(),
        "program_stderr": program_stderr.getvalue(),
        "counts": counts,
        "stats": stats,
        "live_vectors": live_vectors,
        "blank_to_vec_typed": identity_ok,
        "identity_regressions": identity_ok,
        "identity_error": identity_error,
    }
    try:
        assert escaped is None, result
        assert stats["active"] is False and stats["busy"] == 0, result
        assert identity_ok, result
        if panic_mode:
            # A source Panic is terminal. It deliberately skips language
            # cleanup, so the remaining live Vec/pins are not a leak claim.
            assert exit_code == 101, result
            assert counts["slice_drop"] == 0, result
            assert counts["slice_retire"] == 0, result
        else:
            # A collected Slice owner edge is decremented directly by the
            # collector; its retirement is the corresponding counted release.
            assert (counts["vec_retain"] + counts["vec_create"] ==
                    counts["vec_release"] + counts["slice_retire"]), result
            assert counts["slice_begin"] == counts["slice_end"], result
            assert (counts["slice_create"] + counts["slice_copy"] ==
                    counts["slice_begin"]), result
            assert (counts["slice_create"] + counts["slice_copy"] ==
                    counts["slice_retain"]), result
            assert counts["slice_drop"] == counts["slice_release"], result
            assert (counts["slice_create"] + counts["slice_copy"] ==
                    counts["slice_drop"] + counts["slice_retire"]), result
            # The cycle collector decrements a counted Slice owner edge
            # directly (not through Vec.release) and then retires exactly that
            # descriptor, ending its pin without releasing the owner again.
            assert (counts["slice_retain"] - counts["slice_release"] ==
                    counts["slice_retire"]), result
            assert stats["live"] == 0 and live_vectors == 0, result
    except AssertionError as error:
        result["status"] = "failed"
        result["error"] = str(error)
    print(json.dumps(result, sort_keys=True))
    return 0 if result["status"] == "passed" else 1


if __name__ == "__main__":
    sys.exit(main())
