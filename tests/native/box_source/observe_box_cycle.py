#!/usr/bin/env python3
"""Observe a real generated Wz Box cycle with CPython collection disabled."""
import gc
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
gc.disable()


def forbid_host_collection(*args, **kwargs):
    raise AssertionError('Box cycle test must not use CPython gc.collect')


gc.collect = forbid_host_collection

directory = Path(sys.argv[1]).resolve()
sys.path.insert(0, str(directory))
import langrt as rt
import program

metadata = json.loads((directory / 'metadata.json').read_text())
functions = {row['source_name']: row['generated']
             for row in metadata['functions']}
objects = []
weaks = []
object_ids = {}
original_create = rt.Vec.create


def create(element, values):
    value = original_create(element, values)
    object_ids[id(value)] = len(objects)
    objects.append(value)
    weaks.append(rt.Weak(value))
    return value


rt.Vec.create = staticmethod(create)


def function(name):
    return getattr(program, functions[name])


def weak_for(value):
    return weaks[object_ids[id(value)]]


def live_objects():
    return [value for value in objects
            if value.refs > 0 and not value._dead]


def assert_all_dead():
    for value in objects:
        assert value.refs == 0 and value._dead, ('language object alive', value)
        assert not value.items, ('dead object retained rows', value)
        assert weak_for(value).upgrade() is None, ('weak upgraded dead value', value)


def stats():
    return rt._gc_stats()


def box_cycle():
    result = function('make_cycle')()
    assert isinstance(result, tuple) and len(result) == 1, result
    cell = result[0]
    assert isinstance(cell, rt.Vec), cell
    assert cell.refs == 2, cell.refs
    assert not gc.isenabled(), 'host GC must remain disabled'
    live_before = [
        {'refs': value.refs, 'kind': value.element.kind}
        for value in objects if value.refs > 0 and not value._dead]
    root_refs = cell.refs

    before = stats()
    assert rt.collect_cycles()
    rooted = stats()
    assert rooted['last_collected'] == 0, rooted
    assert cell.refs == 2 and not cell._dead, (cell.refs, cell._dead)

    # Drop the source-language root. The Box self-edge remains a live
    # language owner until the pure-Woozy collector runs.
    cell.release()
    after_release_refs = cell.refs
    assert after_release_refs == 1 and not cell._dead, (
        after_release_refs, cell._dead)
    assert rt.collect_cycles()
    after = stats()
    assert after['last_collected'] >= 2, after
    assert_all_dead()
    assert after['weak_dead'] > before['weak_dead'], (before, after)
    return {
        'case': 'box-cycle',
        'live_before_release': live_before,
        'root_refs_before_release': root_refs,
        'root_refs_after_release': after_release_refs,
        'collected': after['last_collected'],
        'weak_dead': after['weak_dead'],
        'host_gc_disabled': not gc.isenabled(),
        'allocations': len(objects),
    }


print(json.dumps(box_cycle(), sort_keys=True))
