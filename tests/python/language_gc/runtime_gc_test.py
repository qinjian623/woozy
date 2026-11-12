"""Private language-ARC/cycle/Weak runtime tests; standard library only."""
import builtins
import importlib.util
from pathlib import Path
import sys
import unittest

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location(
    'langrt_gc_under_test', ROOT / 'runtime/python/langrt.py')
rt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rt)


def capture_of(value=0):
    return rt.Vec.create(rt.ValueType(), [value])


class RuntimeLanguageGC(unittest.TestCase):
    def test_dyn_alias_is_retired_before_any_white_row_callback(self):
        observed = []
        test = self

        class Payload:
            def drop(self):
                observed.append((boxed._dead, value.refs))
                test.assertTrue(boxed._dead)
                boxed.drop()
                with test.assertRaises(rt.Panic):
                    boxed.copy()
                with test.assertRaises(rt.Panic):
                    boxed.call(0, [])

        row = rt.ValueType('struct', fields=(rt.ValueType('dyn'),
                                            rt.ValueType('error')))
        value = rt.Vec.create(row, [])
        boxed = rt.DynObject.create(rt.ValueType('vec'), value, ())
        value.items.append((boxed, Payload()))
        value.release()
        self.assertTrue(rt.collect_cycles())
        self.assertEqual(observed, [(True, 0)])
        self.assertTrue(value._dead)

    def assert_live(self, value):
        self.assertFalse(value._dead)
        self.assertGreater(value.refs, 0)
        self.assertIn(value, rt._LIVE_VECS)

    def test_function_environment_copy_drop_and_cycle(self):
        captures = capture_of()
        fv = rt.FunctionValue(lambda env, arg: env.get(0) + arg, captures)
        self.assertEqual(captures.refs, 1)
        copied = rt.ValueType('function').copy(fv)
        self.assertEqual(captures.refs, 2)
        moved = fv
        fv = None
        copied.drop()
        self.assertEqual(captures.refs, 1)
        moved.drop()
        self.assertEqual(captures.refs, 0)
        self.assertTrue(captures._dead)

        # env -> FunctionValue -> env is a pure Wz closure cycle.
        env = rt.Vec.create(rt.ValueType('function'), [])
        fv = rt.FunctionValue(lambda environment: len(environment.items), env)
        env.push(fv.copy())
        self.assertEqual(env.refs, 2)
        fv.drop()
        self.assertEqual(env.refs, 1)
        weak = rt.Weak(env)
        before = rt._gc_stats()['total_collected']
        self.assertTrue(rt.collect_cycles())
        self.assertEqual(env.refs, 0)
        self.assertTrue(env._dead)
        self.assertEqual(env.items, [])
        self.assertIsNone(weak.upgrade())
        self.assertGreaterEqual(
            rt._gc_stats()['total_collected'] - before, 1)

    def test_function_trace_visits_only_environment(self):
        captures = capture_of(7)
        fv = rt.FunctionValue(lambda env: env.get(0), captures)
        visited = []
        rt.ValueType('function').trace(fv, visited.append)
        self.assertEqual(visited, [captures])
        visited.clear()
        rt.ValueType('weak').trace(rt.Weak(captures), visited.append)
        self.assertEqual(visited, [])
        fv.drop()
        self.assertTrue(captures._dead)

    def test_empty_weak_is_safe(self):
        weak = rt.Weak()
        self.assertTrue(weak.is_empty())
        self.assertIsNone(weak.upgrade())
        self.assertIsNone(weak.upgrade())

    def test_weak_no_capture_function_is_immortal(self):
        def global_code(value):
            return value + 1
        weak = rt.Weak(global_code)
        self.assertIs(weak.upgrade(), global_code)
        fv = rt.FunctionValue(global_code, None)
        self.assertIs(rt.Weak(fv).upgrade(), global_code)
        self.assertEqual(fv(4), 5)

    def test_weak_captured_function_upgrades_with_environment(self):
        environment = capture_of(7)
        function = rt.FunctionValue(lambda env, value: env.get(0) + value,
                                    environment)
        weak = rt.Weak(function)
        upgraded = weak.upgrade()
        self.assertIsInstance(upgraded, rt.FunctionValue)
        self.assertIs(upgraded.environment, environment)
        self.assertEqual(environment.refs, 2)
        self.assertEqual(upgraded(3), 10)
        upgraded.drop()
        self.assertEqual(environment.refs, 1)
        function.drop()
        self.assertTrue(environment._dead)
        self.assertIsNone(weak.upgrade())
        self.assertTrue(weak.is_empty())

    def test_dead_function_weak_never_becomes_immortal(self):
        environment = capture_of()
        function = rt.FunctionValue(lambda env: env.get(0), environment)
        function.drop()
        self.assertTrue(function._dead)
        weak = rt.Weak(function)
        self.assertTrue(weak.is_empty())
        self.assertIsNone(weak.upgrade())

        no_capture = rt.FunctionValue(lambda value: value, None)
        no_capture.drop()
        weak = rt.Weak(no_capture)
        self.assertTrue(weak.is_empty())
        self.assertIsNone(weak.upgrade())

    def test_collected_function_rows_retire_only_owned_wrappers(self):
        vectors = rt.ValueType('vec')
        functions = rt.ValueType('function')
        structs = rt.ValueType('struct', (vectors, functions))
        enums = rt.ValueType('enum', variants={1: (vectors, functions)})

        environment = capture_of(9)
        live = rt.FunctionValue(lambda env: env.get(0), environment)
        live_copy = live.copy()
        owned = live.copy()
        outer = rt.Vec.create(structs, [])
        outer.push((vectors.copy(outer), owned))
        outer.release()
        self.assertEqual(outer.refs, 1)
        self.assertEqual(environment.refs, 3)
        self.assertTrue(rt.collect_cycles())
        self.assertTrue(outer._dead)
        self.assertTrue(owned._dead)
        self.assertIsNone(owned.environment)
        with self.assertRaises(rt.Panic):
            owned()
        owned.drop()
        owned.drop()
        self.assertFalse(live._dead)
        self.assertFalse(live_copy._dead)
        self.assertFalse(environment._dead)
        self.assertEqual(environment.refs, 2)
        self.assertEqual(live(), 9)
        self.assertEqual(live_copy(), 9)
        self.assertTrue(rt.Weak(owned).is_empty())
        live.drop()
        live_copy.drop()
        self.assertTrue(environment._dead)

        enum_environment = capture_of(4)
        enum_owned = rt.FunctionValue(lambda env: env.get(0), enum_environment)
        enum_outer = rt.Vec.create(enums, [])
        enum_outer.push((1, (vectors.copy(enum_outer), enum_owned)))
        enum_outer.release()
        self.assertTrue(rt.collect_cycles())
        self.assertTrue(enum_outer._dead)
        self.assertTrue(enum_owned._dead)
        self.assertIsNone(enum_owned.environment)
        self.assertTrue(enum_environment._dead)

    def test_collector_snapshot_allocation_failure_restores_state(self):
        before = rt._gc_stats()
        original = vars(rt).get('list', builtins.list)
        rt.list = lambda *args, **kwargs: (_ for _ in ()).throw(
            MemoryError('injected collector snapshot allocation failure'))
        try:
            with self.assertRaises(rt.Panic):
                rt.collect_cycles()
        finally:
            if 'list' in vars(rt):
                del rt.list
            else:
                rt.list = original
        after = rt._gc_stats()
        self.assertFalse(after['active'])
        self.assertEqual(after['collections'], before['collections'])
        self.assertEqual(after['total_collected'], before['total_collected'])
        self.assertTrue(rt.collect_cycles())
        self.assertFalse(rt._gc_stats()['active'])

    def test_collector_white_allocation_failure_restores_state(self):
        vectors = rt.ValueType('vec')
        value = rt.Vec.create(vectors, [])
        value.push(vectors.copy(value))
        value.release()
        self.assertEqual(value.refs, 1)
        before = rt._gc_stats()
        original = vars(rt).get('set', builtins.set)
        def fail_white_allocation(*args):
            if args:
                raise MemoryError('injected white set allocation failure')
            return original(*args)
        rt.set = fail_white_allocation
        try:
            with self.assertRaises(rt.Panic):
                rt.collect_cycles()
        finally:
            del rt.set
        after = rt._gc_stats()
        self.assertFalse(after['active'])
        self.assertEqual(after['collections'], before['collections'])
        self.assertEqual(after['total_collected'], before['total_collected'])
        self.assertFalse(value._dead)
        self.assertEqual(value.refs, 1)
        self.assertTrue(rt.collect_cycles())
        self.assertTrue(value._dead)
        self.assertFalse(rt._gc_stats()['active'])

    def test_foreign_finalizer_reenters_after_active_is_off(self):
        events = []
        vectors = rt.ValueType('vec')
        structs = rt.ValueType('struct', (vectors, rt.ValueType('foreign')))

        class Foreign:
            def __init__(self, weak):
                self.weak = weak
            def __del__(self):
                event = {
                    'active': rt._GC.active,
                    'outer_weak_empty': self.weak.upgrade() is None,
                }
                nested = rt.Vec.create(vectors, [])
                nested.retain()
                nested.push(nested)
                nested.release()
                event['nested_collect'] = rt.collect_cycles()
                event['nested_dead'] = nested._dead
                events.append(event)

        outer = rt.Vec.create(structs, [])
        weak = rt.Weak(outer)
        outer.retain()
        outer.push((outer, Foreign(weak)))
        outer.release()
        self.assertTrue(rt.collect_cycles())
        self.assertEqual(events, [{
            'active': False,
            'outer_weak_empty': True,
            'nested_collect': True,
            'nested_dead': True,
        }])
        self.assertEqual(len(rt._LIVE_VECS), 0)
        self.assertFalse(rt._GC.white_ids)

    def test_all_functions_retire_before_any_old_row_callback(self):
        vectors = rt.ValueType('vec')
        functions = rt.ValueType('function')
        for callback in ('foreign', 'error', 'private'):
            for callback_first in (True, False):
                for layout in ('function', 'struct', 'enum', 'nested'):
                    with self.subTest(callback=callback,
                                      callback_first=callback_first,
                                      layout=layout):
                        events = []
                        first = rt.Vec.create(rt.ValueType(), [])
                        second = rt.Vec.create(rt.ValueType(), [])
                        # Exercise both actual registry traversal orders,
                        # independently of object addresses/set hash order.
                        ordered = [value for value in rt._LIVE_VECS
                                   if value is first or value is second]
                        if callback_first:
                            observer, environment = ordered
                        else:
                            environment, observer = ordered
                        function = rt.FunctionValue(
                            lambda env: len(env.items), environment)
                        row = function.copy()
                        rows = [row]
                        if layout == 'function':
                            element, value = functions, row
                        elif layout == 'struct':
                            element = rt.ValueType('struct', (functions,))
                            value = (row,)
                        elif layout == 'enum':
                            element = rt.ValueType('enum',
                                                   variants={1: (functions,)})
                            value = (1, (row,))
                        else:
                            extra = function.copy()
                            no_capture = rt.FunctionValue(lambda: 99)
                            rows.extend((extra, no_capture))
                            inner = rt.ValueType('struct',
                                                 (functions, functions))
                            variant = rt.ValueType('enum', variants={3: (inner,)})
                            element = rt.ValueType('struct',
                                                   (functions, variant))
                            value = (row, (3, ((extra, no_capture),)))
                        environment.element = element
                        environment.push(value)
                        function.drop()

                        def observe():
                            rejected = 0
                            for owned in rows:
                                try:
                                    owned()
                                except rt.Panic:
                                    rejected += 1
                            events.append({
                                'active': rt._GC.active,
                                'retired': all(owned._dead and
                                               owned.environment is None
                                               for owned in rows),
                                'rejected': rejected,
                                'environment_refs': environment.refs,
                            })

                        class Foreign:
                            def __del__(self):
                                observe()

                        class HostError(Exception):
                            def __del__(self):
                                observe()

                        class PrivateOps:
                            stride = 8

                            def trace(self, value, visit):
                                visit(value)
                            def drop(self, value):
                                observe()

                        if callback == 'private':
                            observer.element = PrivateOps()
                            observer.push(vectors.copy(observer))
                        else:
                            observer.element = rt.ValueType(
                                'struct', (vectors, rt.ValueType(callback)))
                            payload = (Foreign() if callback == 'foreign' else
                                       rt.Error(HostError(), '<test>', 0, 0,
                                                'collector callback'))
                            observer.push((vectors.copy(observer), payload))
                            del payload
                        observer.release()
                        self.assertTrue(rt.collect_cycles())
                        self.assertEqual(events, [{
                            'active': False, 'retired': True,
                            'rejected': len(rows), 'environment_refs': 0,
                        }])
                        for owned in rows:
                            owned.drop()
                            owned.drop()
                        self.assertEqual(environment.refs, 0)
                        self.assertTrue(environment._dead)
                        self.assertTrue(observer._dead)
                        self.assertFalse(rt._GC.white_ids)
                        self.assertEqual(len(rt._LIVE_VECS), 0)

    def test_weak_does_not_upgrade_language_dead_vec(self):
        value = capture_of()
        weak = rt.Weak(value)
        upgraded = weak.upgrade()
        self.assertIs(upgraded, value)
        self.assertEqual(value.refs, 2)
        upgraded.release()
        value.release()
        self.assertEqual(value.refs, 0)
        self.assertTrue(value._dead)
        # The Python variable still physically holds the object.
        self.assertIsNotNone(value)
        self.assertNotIn(value, rt._LIVE_VECS)
        self.assertIsNone(weak.upgrade())
        self.assertTrue(weak.is_empty())

    def test_white_weak_is_dead_before_storage_clear(self):
        value = capture_of()
        weak = rt.Weak(value)
        rt._GC.white_ids.add(id(value))
        try:
            self.assertIsNone(weak.upgrade())
            self.assertTrue(weak.is_empty())
            new_weak = rt.Weak(value)
            self.assertIsNone(new_weak.upgrade())
        finally:
            rt._GC.white_ids.discard(id(value))
        self.assertIsNone(weak.upgrade())
        fresh = rt.Weak(value)
        upgraded = fresh.upgrade()
        self.assertIs(upgraded, value)
        upgraded.release()
        value.release()

    def test_duplicate_edges_are_counted_independently(self):
        scalar = rt.ValueType()
        vectors = rt.ValueType('vec')
        child = rt.Vec.create(scalar, [1])
        parent = rt.Vec.create(vectors, [vectors.copy(child), vectors.copy(child)])
        child.release()
        parent.push(vectors.copy(parent))
        parent.release()
        self.assertEqual(parent.refs, 1)
        self.assertEqual(child.refs, 2)
        before = rt._gc_stats()['total_collected']
        self.assertTrue(rt.collect_cycles())
        self.assertTrue(parent._dead)
        self.assertEqual(parent.refs, 0)
        self.assertTrue(child._dead)
        self.assertEqual(child.refs, 0)
        self.assertGreaterEqual(
            rt._gc_stats()['total_collected'] - before, 2)

    def test_dead_cycle_releases_live_child_without_phantom(self):
        scalar = rt.ValueType()
        vectors = rt.ValueType('vec')
        child = rt.Vec.create(scalar, [7])
        child.retain()
        child.retain()
        parent = rt.Vec.create(vectors, [child])
        parent.push(vectors.copy(parent))
        parent.release()
        self.assertEqual(child.refs, 3)
        self.assertTrue(rt.collect_cycles())
        self.assertTrue(parent._dead)
        self.assertEqual(parent.refs, 0)
        self.assertFalse(child._dead)
        # Two external owners remain; the dead parent edge was released.
        self.assertEqual(child.refs, 2)
        weak = rt.Weak(child)
        upgraded = weak.upgrade()
        self.assertIs(upgraded, child)
        upgraded.release()
        child.release()
        child.release()
        self.assertTrue(child._dead)
        self.assertEqual(child.refs, 0)

    def test_python_owned_cycle_is_left_to_the_host(self):
        value = capture_of()
        class Host:
            pass
        first, second = Host(), Host()
        first.other, second.other = second, first
        first.value = second.value = value
        del first, second
        # CPython may or may not reclaim the host cycle later; the language
        # collector must not use host references as roots either way.
        self.assertEqual(value.refs, 1)
        self.assert_live(value)
        self.assertTrue(rt.collect_cycles())
        self.assert_live(value)
        value.release()
        self.assertTrue(value._dead)

    def test_reentrant_collection_reports_busy(self):
        rt._GC.active = True
        busy = rt._GC.busy
        try:
            self.assertFalse(rt.collect_cycles())
            self.assertEqual(rt._GC.busy, busy + 1)
        finally:
            rt._GC.active = False


if __name__ == '__main__':
    unittest.main()
