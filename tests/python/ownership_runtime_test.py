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
        self.assertEqual(rt.RUNTIME_VERSION, 'compiler-dev/1-python-values/4')
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
