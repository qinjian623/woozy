"""Private runtime failure injection and ownership tests; stdlib only."""
import importlib.util
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('rt', Path(__file__).resolve().parents[2] / 'runtime/python/langrt.py')
rt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rt)


class Runtime(unittest.TestCase):
    def test_version_and_panic(self):
        self.assertEqual(rt.RUNTIME_VERSION, 'compiler-dev/1-python-values/7')
        self.assertTrue(issubclass(rt.Panic, BaseException))
        self.assertFalse(issubclass(rt.Panic, Exception))

    def test_reference_count_overflow(self):
        v = rt.Vec.create(rt.ValueType(), [1])
        v.refs = rt._MAX_COUNT
        with self.assertRaises(rt.Panic):
            v.retain()
        self.assertEqual(v.refs, rt._MAX_COUNT)

    def test_object_allocation_failure(self):
        with patch.object(rt, '_allocate_vec', side_effect=MemoryError):
            with self.assertRaises(rt.Panic):
                rt.Vec.create(rt.ValueType(), [1])

    def test_elements_allocation_failure(self):
        with patch.object(rt, '_allocate_elements', side_effect=MemoryError):
            with self.assertRaises(rt.Panic):
                rt.Vec.create(rt.ValueType(), [1])

    def test_size_overflow(self):
        with self.assertRaises(rt.Panic):
            rt._allocation_size(1 << 63, 8)
        with self.assertRaises(rt.Panic):
            rt._allocation_size(1 << 63, 1)

    def test_bounds_preserve_math_value(self):
        v = rt.Vec.create(rt.ValueType(), [1])
        for index in (-1, 1, 1 << 64, (1 << 64) - 1):
            with self.assertRaises(rt.Panic):
                v.get(index)
            with self.assertRaises(rt.Panic):
                v.set(index, 2)
        self.assertEqual(v.get(0), 1)
        for start, end in ((-1, 1), (0, 2), (1, 0), (0, 1 << 64)):
            with self.assertRaises(rt.Panic):
                v.slice(start, end)
        v.release()

    def test_iteration_prevents_change_before_write(self):
        v = rt.Vec.create(rt.ValueType(), [1, 2])
        v.retain()
        self.assertEqual(v.begin(), 2)
        self.assertEqual(v.begin(), 2)
        for replacement in ([1], [1, 2, 3]):
            with self.assertRaises(rt.Panic):
                v._replace_length(replacement)
            self.assertEqual(v.items, [1, 2])
        v.set(1, 7)
        self.assertEqual(v.get(1), 7)
        v.end()
        with self.assertRaises(rt.Panic):
            v._replace_length([])
        v.end()
        v._replace_length([9])
        self.assertEqual(v.items, [9])
        v.release()
        v.release()

    def test_slice_and_shared_nested_elements(self):
        scalar = rt.ValueType()
        reference = rt.ValueType('vec')
        inner = rt.Vec.create(scalar, [1])
        outer = rt.Vec.create(reference, [inner])
        part = outer.slice(0, 1)
        self.assertIsNot(part, outer)
        self.assertIs(part.get(0), inner)
        self.assertEqual(inner.refs, 2)
        part.get(0).set(0, 7)
        self.assertEqual(outer.get(0).get(0), 7)
        part.release()
        self.assertEqual(inner.refs, 1)
        outer.release()
        self.assertEqual(inner.refs, 0)

    def test_drop_order(self):
        events = []
        class Observe:
            stride = 8
            def drop(self, value):
                events.append(value)
        t = Observe()
        v = rt.Vec.create(t, [1, 2, 3])
        v.release()
        self.assertEqual(events, [3, 2, 1])
        events.clear()
        rt.ValueType('struct', (t, t)).drop((1, 2))
        self.assertEqual(events, [2, 1])
        events.clear()
        rt.ValueType('enum', variants={4: (t, t), 5: (t,)}).drop((4, (1, 2)))
        self.assertEqual(events, [2, 1])

    def test_typed_payload_transfer_once(self):
        v = rt.Vec.create(rt.ValueType(), [1])
        error = rt.TypedError('app.E', v, rt.ValueType('vec'))
        self.assertIs(error.take(), v)
        with self.assertRaises(RuntimeError):
            error.take()
        error.drop()
        self.assertEqual(v.refs, 1)
        v.release()
        self.assertEqual(v.refs, 0)

    def test_uncaught_typed_payload_drop(self):
        v = rt.Vec.create(rt.ValueType(), [1])
        error = rt.TypedError('app.E', v, rt.ValueType('vec'))
        error.drop()
        error.drop()
        self.assertEqual(v.refs, 0)


    def test_error_payload_copy_and_source(self):
        class Inner(Exception):
            pass
        class Outer(Exception):
            pass
        try:
            try:
                raise Inner('inner')
            except Inner as inner:
                raise Outer('outer') from inner
        except Outer as outer:
            payload = rt.Error(outer, '/tmp/source.lang', 10, 20, 'host.read')
        self.assertIs(payload.copy(), payload)
        self.assertTrue(payload.type_name.endswith('Outer'))
        self.assertEqual(payload.message, 'outer')
        self.assertIs(payload.cause.__class__, Inner)
        self.assertIn('Outer', payload.traceback_text)
        self.assertEqual((payload.file, payload.begin_byte, payload.end_byte,
                          payload.operation),
                         ('/tmp/source.lang', 10, 20, 'host.read'))
        payload.drop()
        self.assertTrue(payload.describe().endswith(
            "Outer: outer at /tmp/source.lang:10:20 (host.read)"))

    def test_foreign_value_copy_and_drop(self):
        foreign = rt.ValueType('foreign')
        error = rt.ValueType('error')
        payload = object()
        self.assertIs(foreign.copy(payload), payload)
        self.assertIs(error.copy(payload), payload)
        foreign.drop(payload)
        dropped = []
        class Drop:
            def drop(self):
                dropped.append(True)
        error.drop(Drop())
        self.assertEqual(dropped, [True])

    def test_error_payload_copy_in_vec_and_aggregates(self):
        class Inner(Exception):
            pass
        class Outer(Exception):
            pass
        try:
            try:
                raise Inner('inner')
            except Inner as inner:
                raise Outer('outer') from inner
        except Outer as outer:
            payload = rt.Error(outer, '/tmp/source.lang', 10, 20, 'host.read')
        exception = payload.exception
        cause = payload.cause
        traceback_object = exception.__traceback__
        cause_traceback = cause.__traceback__
        ids = (id(exception), id(cause), id(traceback_object),
               id(cause_traceback))
        error = rt.ValueType('error')
        self.assertIs(error.copy(payload), payload)

        values = rt.Vec.create(error, [payload])
        part = values.slice(0, 1)
        self.assertIs(values.get(0), payload)
        self.assertIs(part.get(0), payload)

        struct = rt.ValueType('struct', (error, error))
        copied = struct.copy((payload, payload))
        self.assertIs(copied[0], payload)
        self.assertIs(copied[1], payload)
        struct.drop(copied)

        enum = rt.ValueType('enum', variants={3: (error,)})
        copied_enum = enum.copy((3, (payload,)))
        self.assertIs(copied_enum[1][0], payload)
        enum.drop(copied_enum)

        values.release()
        self.assertIs(part.get(0), payload)
        part.release()
        # Language cleanup only releases owner slots. The host exception,
        # cause and traceback remain the same live Python objects.
        error.drop(payload)
        error.drop(payload)
        self.assertIs(payload.exception, exception)
        self.assertIs(payload.cause, cause)
        self.assertIs(payload.exception.__traceback__, traceback_object)
        self.assertIs(payload.cause.__traceback__, cause_traceback)
        self.assertEqual((id(payload.exception), id(payload.cause),
                          id(payload.exception.__traceback__),
                          id(payload.cause.__traceback__)), ids)
        self.assertIn('Outer', payload.traceback_text)
        self.assertTrue(payload.describe().endswith(
            'Outer: outer at /tmp/source.lang:10:20 (host.read)'))

    def test_foreign_reference_copy_in_vec_and_aggregates(self):
        foreign = rt.ValueType('foreign')
        host = object()
        outer = rt.Vec.create(foreign, [host])
        part = outer.slice(0, 1)
        self.assertIs(outer.get(0), host)
        self.assertIs(part.get(0), host)
        outer.release()
        self.assertIs(part.get(0), host)
        part.release()
        struct = rt.ValueType('struct', (foreign, foreign))
        copied = struct.copy((host, host))
        self.assertIs(copied[0], host)
        self.assertIs(copied[1], host)
        struct.drop(copied)
        enum = rt.ValueType('enum', variants={3: (foreign,)})
        copied_enum = enum.copy((3, (host,)))
        self.assertIs(copied_enum[1][0], host)
        enum.drop(copied_enum)

    def test_foreign_read_wraps_host_error_once(self):
        class Outer(Exception):
            pass

        def fail():
            try:
                raise ValueError('cause')
            except ValueError as cause:
                raise Outer('outer') from cause

        with self.assertRaises(rt.TypedError) as caught:
            rt.foreign_call(fail, (), {}, '/tmp/f.lang', 3, 4, 'f.call', False)
        error = caught.exception
        self.assertEqual(error.type_key, 'lang.py.Error')
        self.assertIsInstance(error.value, rt.Error)
        self.assertTrue(error.value.type_name.endswith('Outer'))
        self.assertEqual(error.value.message, 'outer')
        self.assertIs(error.value.cause.__class__, ValueError)
        self.assertEqual((error.value.file, error.value.begin_byte,
                          error.value.end_byte, error.value.operation),
                         ('/tmp/f.lang', 3, 4, 'f.call'))
        self.assertIn('Outer', error.value.traceback_text)
        error.drop()

    def test_foreign_call_does_not_rewrap_internal_errors(self):
        typed = rt.TypedError('app.E', 1, rt.ValueType())
        with self.assertRaises(rt.TypedError) as caught:
            rt.foreign_call(lambda: (_ for _ in ()).throw(typed), (), {},
                            '/tmp/f.lang', 1, 1, 'f', False)
        self.assertIs(caught.exception, typed)

    def test_foreign_call_preserves_base_exceptions(self):
        for kind in (rt.Panic, KeyboardInterrupt, SystemExit):
            def fail(kind=kind):
                raise kind('base')
            with self.assertRaises(kind):
                rt.foreign_call(fail, (), {}, '/tmp/f.lang', 1, 1, 'f', False)

    def test_foreign_call_unit_contract(self):
        self.assertIsNone(
            rt.foreign_call(lambda: None, (), {}, '/tmp/f.lang', 1, 1, 'f', True))
        with self.assertRaises(rt.Panic):
            rt.foreign_call(lambda: 1, (), {}, '/tmp/f.lang', 1, 1, 'f', True)

    def test_missing_host_module_is_environment_error(self):
        with self.assertRaises(rt.HostEnvironmentError):
            rt.host_module('not/a/module', 't10_missing_host_module_for_test')
        with self.assertRaises(rt.HostEnvironmentError):
            rt.host_module('not/a/module', 't10_missing_host_module_for_test')

    def test_host_module_cache_includes_declared_host_identity(self):
        import cmath
        import math
        first = rt.host_module('shared-test', 'math')
        second = rt.host_module('shared-test', 'cmath')
        other = rt.host_module('other-test', 'cmath')
        self.assertIs(first, math)
        self.assertIs(second, cmath)
        self.assertIsNot(second, math)
        self.assertIs(other, cmath)
        self.assertIs(rt.host_module('shared-test', 'cmath'), second)
        value = rt.foreign_read(second, ('sqrt',), 'cache.lang', 3, 9,
                                'shared-test.sqrt')
        result = rt.foreign_call(value, (-1.0,), {}, 'cache.lang', 3, 10,
                                 'shared-test.sqrt', False)
        self.assertEqual(result, 1j)
        failed = False
        try:
            rt.host_module('missing-cache', 't10_missing_host_module_for_test')
        except rt.HostEnvironmentError:
            failed = True
        self.assertTrue(failed)
        with self.assertRaises(rt.HostEnvironmentError):
            rt.host_module('missing-cache', 't10_missing_host_module_for_test')

    def test_no_gc_language_drop(self):
        import gc
        v = rt.Vec.create(rt.ValueType(), [1])
        owner = rt.Owner(rt.ValueType('vec'), v)
        del owner
        gc.collect()
        self.assertEqual(v.refs, 1)
        v.release()


if __name__ == '__main__':
    unittest.main()
