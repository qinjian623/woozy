#!/usr/bin/env python3
"""Minimal direct checks for _rt.replace_place over all six place shapes.

Each shape must publish the complete new value before the replaced value is
released, release that value exactly once, and leave the new owner at refs=1:
Owner (owning local slot), Cell (writable local), Field over Owner, Field over
Cell, Index (Vec/Slice element) and Field over Index. Index shapes must not drop
twice because Vec.set already drops the replaced element, and an inactive Owner
slot (taken value) must install without dropping anything.

This runs against the real embedded runtime source, and the owning-inout CTest
runner executes it so the checks are part of the normal test suite.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True


def load_runtime(path: Path):
    spec = importlib.util.spec_from_file_location("owning_inout_langrt", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def make_place(rt, kind, ops, old):
    """Build one real place object of the requested shape."""
    if kind == "Owner":
        return rt.Owner(ops, old)
    if kind == "Cell":
        return rt.Cell(old)
    if kind == "FieldOwner":
        aggregate = rt.ValueType("struct", fields=(ops, rt.ValueType()))
        return rt.Field(rt.Owner(aggregate, (old, 7)), 0)
    if kind == "FieldCell":
        return rt.Field(rt.Cell((old, 7)), 0)
    if kind == "Index":
        return rt.Index(rt.Vec.create(ops, [old]), 0)
    if kind == "FieldIndex":
        struct = rt.ValueType("struct", fields=(ops, rt.ValueType()))
        return rt.Field(rt.Index(rt.Vec.create(struct, [(old, 7)]), 0), 0)
    raise AssertionError("unknown place kind " + kind)


def release_place(rt, kind, ops, place):
    """Release the installed new owner exactly once for this shape."""
    if kind == "Owner":
        place.drop()
    elif kind == "Cell":
        ops.drop(place.value)
    elif kind == "FieldOwner":
        place.root.drop()
    elif kind == "FieldCell":
        aggregate = rt.ValueType("struct", fields=(ops, rt.ValueType()))
        aggregate.drop(place.root.value)
    elif kind == "Index":
        place.receiver.release()
    else:
        place.root.receiver.release()


def check_shape(rt, kind):
    ops = rt.ValueType("vec")
    old = rt.Vec.create(rt.ValueType(), [1])
    new = rt.Vec.create(rt.ValueType(), [9])
    drops = []
    holder = []

    class Observer(rt.ValueType):
        def drop(self, value):
            if value is old:
                # Publication must already be complete when the old owner runs
                # its destructor, so a reentrant cleanup never reads the old
                # member.
                drops.append({"published": holder[0].value is new,
                              "refs": value.refs})
            super().drop(value)

    observer = Observer("vec")
    place = make_place(rt, kind, observer, old)
    holder.append(place)
    try:
        rt.replace_place(place, observer, new)
        # Exactly one release of the replaced owner, after publication.
        assert drops == [{"published": True, "refs": 1}], (kind, drops)
        assert old._dead, (kind, "old value was not released")
        assert new.refs == 1, (kind, new.refs)
        # Reading through the place reaches the new value after the drop.
        assert place.value is new, (kind, "place lost the new value")
        row = {"kind": kind, "drops": list(drops), "old_dead": old._dead,
               "new_refs": new.refs, "status": "passed"}
    finally:
        release_place(rt, kind, observer, place)
    return row


def check_inactive_owner(rt):
    """An Owner whose value was taken installs without dropping anything."""
    ops = rt.ValueType("vec")
    taken = rt.Vec.create(rt.ValueType(), [3])
    fresh = rt.Vec.create(rt.ValueType(), [4])
    drops = []

    class Observer(rt.ValueType):
        def drop(self, value):
            drops.append(value is taken)
            super().drop(value)

    observer = Observer("vec")
    place = rt.Owner(observer, taken)
    assert place.take() is taken
    rt.replace_place(place, observer, fresh)
    assert place.value is fresh, "inactive Owner did not receive the value"
    assert drops == [], ("inactive Owner dropped a value", drops)
    assert taken._dead is False, "inactive Owner released the taken value"
    row = {"kind": "OwnerInactive", "drops": list(drops),
           "old_dead": taken._dead, "new_refs": fresh.refs,
           "status": "passed"}
    observer.drop(place.value)
    taken.release()
    return row


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--format", choices=("json",), default="json")
    args = parser.parse_args()
    rt = load_runtime(args.runtime)
    kinds = ("Owner", "Cell", "FieldOwner", "FieldCell", "Index", "FieldIndex")
    rows = [check_shape(rt, kind) for kind in kinds]
    rows.append(check_inactive_owner(rt))
    print(json.dumps({"runtime": str(args.runtime), "status": "passed",
                      "places": rows}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
