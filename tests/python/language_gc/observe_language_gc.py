#!/usr/bin/env python3
"""Import one generated Wz program and verify real language ownership/GC."""
import gc
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
gc.disable()


def forbid_host_collection(*args, **kwargs):
    raise AssertionError('language GC test must not use CPython gc.collect')


gc.collect = forbid_host_collection
directory = Path(sys.argv[1]).resolve()
case = sys.argv[2]
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


def upgrade_is(value):
    upgraded = weak_for(value).upgrade()
    assert upgraded is value, ('upgrade failed for live object', value)
    upgraded.release()


def assert_all_dead():
    for value in objects:
        assert value.refs == 0 and value._dead, ('language object alive', value)
        assert not value.items, ('dead object retained rows', value)
        assert weak_for(value).upgrade() is None, ('weak upgraded dead value', value)


def stats():
    return rt._gc_stats()


def self_cycle():
    value = function('make_self')()
    assert value.refs == 2, value.refs
    weak = weak_for(value)
    before = stats()
    assert rt.collect_cycles()
    after = stats()
    assert after['last_collected'] == 0, after
    assert value.refs == 2 and not value._dead, (value.refs, value._dead)
    upgrade_is(value)
    assert value.refs == 2

    # Drop the source-language root; the self edge remains a live language
    # owner until the collector runs.
    value.release()
    assert value.refs == 1 and not value._dead, (value.refs, value._dead)
    upgrade_is(value)
    assert rt.collect_cycles()
    after = stats()
    assert after['last_collected'] >= 1, after
    assert value.refs == 0 and value._dead and not value.items
    assert weak.upgrade() is None
    assert after['weak_dead'] > before['weak_dead']


def two_cycles():
    root = function('make_pair')()
    live = live_objects()
    assert len(live) == 2, [(v.refs, v.element.kind) for v in live]
    for value in live:
        upgrade_is(value)
    root.release()
    assert not root._dead
    assert all(value.refs > 0 and not value._dead for value in live)
    for value in live:
        upgrade_is(value)
    assert rt.collect_cycles()
    after = stats()
    assert after['last_collected'] >= 2, after
    assert_all_dead()


def duplicate_edges():
    root = function('make_duplicate')()
    live = live_objects()
    assert len(live) == 2, [(v.refs, v.element.kind) for v in live]
    child = next(value for value in live if value is not root)
    # Two parent rows independently own the child; one child edge returns to
    # the parent.  Counting duplicate edges once would leak the child.
    assert child.refs == 2, child.refs
    root.release()
    assert root.refs == 1 and child.refs == 2, (root.refs, child.refs)
    assert rt.collect_cycles()
    after = stats()
    assert after['last_collected'] >= 2, after
    assert_all_dead()


def dead_to_live():
    child = function('make_dead_to_live')()
    live = live_objects()
    assert len(live) == 2, [(v.refs, v.element.kind) for v in live]
    parent = next(value for value in live if value is not child)
    assert child.refs == 2, child.refs
    parent_weak = weak_for(parent)
    assert rt.collect_cycles()
    after = stats()
    assert after['last_collected'] >= 1, after
    assert parent._dead and parent.refs == 0 and not parent.items
    assert parent_weak.upgrade() is None
    assert live_objects() == [child]
    assert child.refs == 1 and not child._dead, child.refs
    upgrade_is(child)
    child.release()
    assert child.refs == 0 and child._dead
    assert weak_for(child).upgrade() is None


def nested_owned():
    result = function('make_nested')()
    nodes, blocks = result
    assert nodes.refs == 2 and blocks.refs == 1, (nodes.refs, blocks.refs)
    nodes_weak = weak_for(nodes)
    upgrade_is(blocks)
    nodes.release()
    assert nodes.refs == 1 and not nodes._dead
    assert rt.collect_cycles()
    after = stats()
    assert after['last_collected'] >= 1, after
    assert nodes._dead and nodes.refs == 0 and not nodes.items
    assert nodes_weak.upgrade() is None
    assert not blocks._dead and blocks.refs == 1
    upgrade_is(blocks)
    blocks.release()
    assert blocks.refs == 0 and blocks._dead
    assert live_objects() == [], live_objects()
    assert weak_for(blocks).upgrade() is None


def automatic_churn(name, loops):
    before = stats()
    assert function(name)(loops) == 0
    after = stats()
    assert after['polls'] > before['polls'], after
    assert after['collections'] > before['collections'], after
    assert after['total_collected'] - before['total_collected'] > 50, after
    assert after['live'] < 100, after
    assert any(weak._invalid for weak in weaks), 'collector did not retire Weak handles'
    # The generated loop header poll, not a manual call in this observer, did
    # the collection above.  Drain the final threshold remainder explicitly.
    assert rt.collect_cycles()
    final = stats()
    assert final['last_collected'] >= 1, final
    assert final['live'] == 0, final
    assert_all_dead()


def exception_defer():
    assert function('exception_path')(False) == 10
    assert function('exception_path')(True) == 100
    assert rt.collect_cycles()
    after = stats()
    assert after['last_collected'] >= 2, after
    assert_all_dead()


CASES = {
    'self-cycle': self_cycle,
    'two-cycles': two_cycles,
    'duplicate-edges': duplicate_edges,
    'dead-to-live': dead_to_live,
    'nested-owned': nested_owned,
    'auto-churn': lambda: automatic_churn('churn', 300),
    'always-continue': lambda: automatic_churn('churn_continue', 300),
    'exception-defer': exception_defer,
}

CASES[case]()
print(json.dumps({'case': case, 'stats': stats(),
                  'live': len(live_objects())}, ensure_ascii=False))
