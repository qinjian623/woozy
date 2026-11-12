#!/usr/bin/env python3
"""Run a generated dyn-cycle program with CPython GC disabled.

The source program creates a pure-Woozy Vec[dyn Trait] cycle on every loop
iteration.  No host collection is allowed: the language collector must retire
DynObject wrappers and release every counted edge exactly once.
"""
import gc
import json
from pathlib import Path
import runpy
import sys

sys.dont_write_bytecode = True
gc.disable()


def forbid_host_collection(*args, **kwargs):
    raise AssertionError('dyn cycle test must not use CPython gc.collect')


gc.collect = forbid_host_collection
directory = Path(sys.argv[1]).resolve()
sys.path.insert(0, str(directory))
import langrt as rt

vecs, dyns = [], []
original_create = rt.Vec.create
original_init = rt.DynObject.__init__


def create(element, values):
    value = original_create(element, values)
    vecs.append(value)
    return value


def init(self, *args, **kwargs):
    original_init(self, *args, **kwargs)
    dyns.append(self)


rt.Vec.create = staticmethod(create)
rt.DynObject.__init__ = init
try:
    runpy.run_path(str(directory / 'program.py'), run_name='__main__')
except SystemExit as error:
    assert error.code == 0, error.code

stats = rt._gc_stats()
assert not rt._LIVE_VECS, [value for value in rt._LIVE_VECS if not value._dead]
assert stats['collections'] > 0, stats
assert stats['total_collected'] >= 384, stats
for value in vecs:
    assert value.refs == 0 and value._dead and not value.items, value
for value in dyns:
    assert value._dead, value
print(json.dumps({
    'status': 'passed',
    'host_gc_disabled': not gc.isenabled(),
    'collections': stats['collections'],
    'total_collected': stats['total_collected'],
    'live_vecs': len(rt._LIVE_VECS),
    'dyn_wrappers': len(dyns),
}, sort_keys=True))
