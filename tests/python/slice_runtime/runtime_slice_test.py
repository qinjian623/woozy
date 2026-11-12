"""Private Python borrowed-Slice runtime tests; standard library only."""
import gc
import importlib.util
from pathlib import Path
import sys
import unittest

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location(
    'langrt_slice_under_test', ROOT / 'runtime/python/langrt.py')
rt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rt)

# Language ARC/collection must not depend on CPython's own cycle collector.
gc.disable()
assert not gc.isenabled()


def forbid_host_collection(*args, **kwargs):
    raise AssertionError('Slice runtime tests must not call gc.collect()')


gc.collect = forbid_host_collection


def make_vec(element, values):
    return rt.Vec.create(element, list(values))


class RuntimeSlice(unittest.TestCase):
    def assert_live(self, value):
        self.assertFalse(value._dead)
        self.assertGreater(value.refs, 0)
        self.assertIn(value, rt._LIVE_VECS)

    def assert_dead(self, value):
        self.assertTrue(value._dead)
        self.assertEqual(value.refs, 0)
        self.assertEqual(value.iterations, 0)
        self.assertEqual(value.items, [])
        self.assertNotIn(value, rt._LIVE_VECS)

    def assert_panic(self, function, *args):
        with self.assertRaises(rt.Panic):
            function(*args)

    def test_create_copy_get_set_drop_and_bounds(self):
        scalar = rt.ValueType()
        owner = make_vec(scalar, [10, 11, 12, 13])
        view = rt.Slice.create(owner, 1, 3)
        self.assertIs(view.owner, owner)
        self.assertEqual((view.start, view.length), (1, 2))
        self.assertEqual((owner.refs, owner.iterations), (2, 1))
        self.assertEqual(len(view), 2)
        self.assertEqual(view.len(), 2)
        self.assertEqual(view.get(0), 11)
        self.assertEqual(view.at(1).value, 12)

        view.set(0, 20)
        self.assertEqual(owner.items, [10, 20, 12, 13])
        view.at(1).value = 21
        self.assertEqual(owner.items, [10, 20, 21, 13])

        slices = rt.ValueType('slice')
        copied = slices.copy(view)
        self.assertIsNot(copied, view)
        self.assertEqual((copied.owner, copied.start, copied.length),
                         (owner, 1, 2))
        self.assertEqual((owner.refs, owner.iterations), (3, 2))
        copied.set(1, 22)
        self.assertEqual(view.get(1), 22)
        self.assertEqual(owner.items, [10, 20, 22, 13])

        for index in (-1, 2, 1 << 64):
            self.assert_panic(view.get, index)
            self.assert_panic(view.set, index, 0)
            self.assert_panic(view.at, index)

        # Failed construction validates the half-open range before retaining
        # or pinning the owner.
        for start, end in ((-1, 0), (0, -1), (4, 3), (0, 5)):
            self.assert_panic(rt.Slice.create, owner, start, end)
            self.assertEqual((owner.refs, owner.iterations), (3, 2))

        slices.drop(copied)
        self.assertFalse(copied.owner)
        self.assertEqual((copied.start, copied.length), (0, 0))
        self.assertTrue(copied._dead)
        slices.drop(copied)
        self.assertEqual((owner.refs, owner.iterations), (2, 1))

        view.drop()
        self.assertFalse(view.owner)
        self.assertEqual((view.start, view.length), (0, 0))
        view.drop()
        self.assertEqual((owner.refs, owner.iterations), (1, 0))
        # The owner binding still owns its row and element storage.
        self.assert_live(owner)
        owner.release()
        self.assert_dead(owner)

    def test_move_like_transfer_does_not_add_edge_or_pin(self):
        owner = make_vec(rt.ValueType(), [4])
        view = rt.Slice.create(owner, 0, 1)
        moved = view
        view = None
        self.assertEqual((owner.refs, owner.iterations), (2, 1))
        self.assertEqual(moved.get(0), 4)
        moved.drop()
        self.assertEqual((owner.refs, owner.iterations), (1, 0))
        owner.release()

    def test_empty_views_pin_owner_and_blank_is_idempotent(self):
        scalar = rt.ValueType()
        owner = make_vec(scalar, [1])
        empty = rt.Slice.create(owner, 1, 1)
        self.assertEqual((empty.start, empty.length), (1, 0))
        self.assertEqual((owner.refs, owner.iterations), (2, 1))
        self.assertEqual(len(empty), 0)
        for index in (-1, 0, 1):
            self.assert_panic(empty.get, index)
            self.assert_panic(empty.set, index, 0)
        empty_copy = empty.sub(0, 0)
        self.assertEqual((empty_copy.start, empty_copy.length), (1, 0))
        self.assertEqual((owner.refs, owner.iterations), (3, 2))
        empty_copy.drop()
        empty.drop()
        self.assertEqual((owner.refs, owner.iterations), (1, 0))

        empty_owner = make_vec(scalar, [])
        owner_backed_empty = rt.Slice.create(empty_owner, 0, 0)
        self.assertEqual((empty_owner.refs, empty_owner.iterations), (2, 1))
        self.assertEqual(len(owner_backed_empty), 0)
        self.assert_panic(rt.Slice.create, empty_owner, 0, 1)
        owner_backed_empty.drop()
        self.assertEqual((empty_owner.refs, empty_owner.iterations), (1, 0))
        self.assertEqual(empty_owner.pop(), (False, None))
        empty_owner.release()
        owner.release()

        blank = rt.Slice.blank()
        self.assertEqual(len(blank), 0)
        blank_copy = blank.copy()
        self.assertFalse(blank_copy.owner)
        self.assertEqual(len(blank_copy), 0)
        blank_sub = blank.sub(0, 0)
        self.assertFalse(blank_sub.owner)
        blank.drop()
        blank.drop()
        blank_copy.drop()
        blank_sub.drop()
        self.assertTrue(blank._dead)
        self.assert_panic(blank.get, 0)

    def test_subviews_and_negative_bounds(self):
        owner = make_vec(rt.ValueType(), [0, 1, 2, 3, 4, 5])
        root = rt.Slice.create(owner, 1, 5)
        mid = root.sub(1, 3)
        terminal = root.sub(4, 4)
        omitted = root.sub(2, None)
        self.assertEqual((mid.start, mid.length), (2, 2))
        self.assertEqual((terminal.start, terminal.length), (5, 0))
        self.assertEqual((omitted.start, omitted.length), (3, 2))
        self.assertEqual((owner.refs, owner.iterations), (5, 4))
        self.assertEqual(mid.get(1), 3)
        self.assertEqual(omitted.get(1), 4)

        for start, end in ((-1, 0), (0, -1), (2, 1), (0, 5)):
            self.assert_panic(root.sub, start, end)
            self.assertEqual((owner.refs, owner.iterations), (5, 4))

        omitted.drop()
        terminal.drop()
        mid.drop()
        root.drop()
        self.assertEqual((owner.refs, owner.iterations), (1, 0))
        owner.release()

    def test_to_vec_copies_owned_nested_elements(self):
        scalar = rt.ValueType()
        vectors = rt.ValueType('vec')
        inner = make_vec(scalar, [1, 2])
        owner = make_vec(vectors, [inner])
        view = rt.Slice.create(owner, 0, 1)
        result = view.to_vec()
        self.assertIsNot(result, owner)
        self.assertIs(result.element, vectors)
        self.assertEqual(result.refs, 1)
        self.assertEqual(inner.refs, 2)
        self.assertIs(result.get(0), inner)

        # Vec-valued elements are logically copied, so the new container owns
        # another handle but still shares the inner container identity.
        result.get(0).set(0, 9)
        self.assertEqual(owner.get(0).get(0), 9)
        self.assertEqual(view.get(0).get(0), 9)

        replacement = make_vec(scalar, [3])
        result.set(0, replacement)
        self.assertEqual(inner.refs, 1)
        self.assertIs(owner.get(0), inner)
        self.assertIs(result.get(0), replacement)

        view.drop()
        owner.release()
        self.assert_dead(inner)
        result.release()
        self.assert_dead(replacement)

    def test_struct_enum_value_edges_and_trace(self):
        scalar = rt.ValueType()
        slice_ops = rt.ValueType('slice')
        owner = make_vec(scalar, [11, 12, 13])
        view = rt.Slice.create(owner, 1, 3)
        self.assertEqual((owner.refs, owner.iterations), (2, 1))

        struct_ops = rt.ValueType('struct', (slice_ops,))
        enum_ops = rt.ValueType('enum', variants={4: (slice_ops, slice_ops)})
        struct_copy = struct_ops.copy((view,))
        enum_copy = enum_ops.copy((4, (view, view)))
        self.assertEqual((owner.refs, owner.iterations), (5, 4))

        visited = []
        slice_ops.trace(view, visited.append)
        self.assertEqual(visited, [owner])
        visited.clear()
        struct_ops.trace(struct_copy, visited.append)
        self.assertEqual(visited, [owner])
        visited.clear()
        enum_ops.trace(enum_copy, visited.append)
        self.assertEqual(visited, [owner, owner])

        struct_ops.drop(struct_copy)
        self.assertEqual((owner.refs, owner.iterations), (4, 3))
        enum_ops.drop(enum_copy)
        self.assertEqual((owner.refs, owner.iterations), (2, 1))
        view.drop()
        self.assertEqual((owner.refs, owner.iterations), (1, 0))
        owner.release()

        # A slice trace visits only the owner Vec node, never the owner's own
        # element contents.
        vectors = rt.ValueType('vec')
        inner = make_vec(scalar, [7])
        outer = make_vec(vectors, [inner])
        outer_view = rt.Slice.create(outer, 0, 1)
        visited = []
        slice_ops.trace(outer_view, visited.append)
        self.assertEqual(visited, [outer])
        self.assertNotIn(inner, visited)
        outer_view.drop()
        outer.release()
        self.assert_dead(inner)

    def test_structural_mutation_blocked_through_every_alias(self):
        owner = make_vec(rt.ValueType(), [1, 2, 3])
        alias = owner
        view = rt.Slice.create(owner, 0, 3)
        original_items = list(owner.items)
        original_capacity = owner.capacity
        self.assertEqual((owner.refs, owner.iterations), (2, 1))

        # No-op reserve/resize are not structural changes and remain legal.
        owner.reserve(owner.capacity)
        alias.resize(len(alias.items), 0)
        self.assertEqual(owner.items, original_items)
        self.assertEqual(owner.capacity, original_capacity)

        mutations = (
            lambda: alias.reserve(alias.capacity + 1),
            lambda: alias.push(4),
            lambda: alias.pop(),
            lambda: alias.resize(4, 9),
            lambda: alias.resize(1, 9),
            lambda: alias.clear(),
            lambda: alias._replace_length([]),
            lambda: alias.reserve(32),
        )
        for mutation in mutations:
            self.assert_panic(mutation)
            self.assertEqual(owner.items, original_items)
            self.assertEqual(owner.capacity, original_capacity)
            self.assertEqual((owner.refs, owner.iterations), (2, 1))

        view.drop()
        self.assertEqual((owner.refs, owner.iterations), (1, 0))
        alias.push(4)
        self.assertEqual(owner.items, [1, 2, 3, 4])
        owner.resize(1, 0)
        self.assertEqual(owner.items, [1])
        owner.clear()
        self.assertEqual(owner.items, [])
        owner.reserve(8)
        self.assertGreaterEqual(owner.capacity, 8)
        owner.release()

    def test_normal_arc_view_outlives_vec_binding(self):
        scalar = rt.ValueType()
        owner = make_vec(scalar, [5, 6])
        view = rt.Slice.create(owner, 0, 2)
        owner.release()
        self.assertFalse(owner._dead)
        self.assertEqual(owner.refs, 1)
        self.assertEqual((owner.iterations, view.get(1)), (1, 6))
        view.set(1, 7)
        self.assertEqual(owner.items, [5, 7])
        copied = view.copy()
        self.assertEqual(owner.refs, 2)
        view.drop()
        self.assertEqual(owner.refs, 1)
        self.assertEqual(copied.get(1), 7)
        copied.drop()
        self.assert_dead(owner)

    def test_slice_cycles_in_struct_and_enum(self):
        slice_ops = rt.ValueType('slice')
        struct_ops = rt.ValueType('struct', (slice_ops,))
        struct_owner = rt.Vec.create(struct_ops, [])
        struct_owner.push((rt.Slice.blank(),))
        struct_slice = rt.Slice.create(struct_owner, 0, 1)
        struct_owner.set(0, (struct_slice,))
        self.assertEqual((struct_owner.refs, struct_owner.iterations), (2, 1))
        struct_owner.release()
        self.assertEqual((struct_owner.refs, struct_owner.iterations), (1, 1))
        struct_weak = rt.Weak(struct_owner)
        self.assertTrue(rt.collect_cycles())
        self.assert_dead(struct_owner)
        self.assertIsNone(struct_weak.upgrade())
        self.assertIsNone(struct_slice.owner)
        self.assertTrue(struct_slice._dead)
        self.assert_panic(struct_slice.get, 0)
        struct_slice.drop()

        enum_ops = rt.ValueType('enum', variants={7: (slice_ops,)})
        enum_owner = rt.Vec.create(enum_ops, [])
        enum_owner.push((7, (rt.Slice.blank(),)))
        enum_slice = rt.Slice.create(enum_owner, 0, 1)
        enum_owner.set(0, (7, (enum_slice,)))
        enum_owner.release()
        self.assertEqual((enum_owner.refs, enum_owner.iterations), (1, 1))
        self.assertTrue(rt.collect_cycles())
        self.assert_dead(enum_owner)
        self.assertIsNone(enum_slice.owner)
        self.assertTrue(enum_slice._dead)
        enum_slice.drop()

    def test_cross_node_slice_vec_cycle(self):
        scalar = rt.ValueType()
        slice_ops = rt.ValueType('slice')
        vectors = rt.ValueType('vec')
        a = rt.Vec.create(rt.ValueType('struct', (slice_ops,)), [])
        b = rt.Vec.create(vectors, [])
        a.push((rt.Slice.blank(),))
        # Install the Vec edge before pinning b: structural b changes are
        # rejected once the Slice owns its iteration pin.
        b.push(vectors.copy(a))
        edge = rt.Slice.create(b, 0, 1)
        a.set(0, (edge,))
        self.assertEqual((a.refs, b.refs, b.iterations), (2, 2, 1))
        a.release()
        b.release()
        self.assertEqual((a.refs, b.refs, b.iterations), (1, 1, 1))
        self.assertTrue(rt.collect_cycles())
        self.assert_dead(a)
        self.assert_dead(b)
        self.assertIsNone(edge.owner)
        edge.drop()
        # The scalar type is unused in this graph but keeps the fixture clear.
        self.assertIsInstance(scalar, rt.ValueType)

    def test_duplicate_slice_edges_and_live_owner(self):
        scalar = rt.ValueType()
        vectors = rt.ValueType('vec')
        slice_ops = rt.ValueType('slice')
        child = make_vec(scalar, [5])
        element = rt.ValueType('struct', (slice_ops, slice_ops, vectors))
        parent = rt.Vec.create(element, [])
        first = rt.Slice.create(child, 0, 1)
        second = rt.Slice.create(child, 0, 1)
        self.assertEqual((child.refs, child.iterations), (3, 2))
        parent.push((first, second, vectors.copy(parent)))
        parent.release()
        self.assertEqual((parent.refs, parent.iterations), (1, 0))
        self.assertTrue(rt.collect_cycles())

        self.assert_dead(parent)
        # Both duplicate edges were counted once, but the live child kept its
        # external owner.  Both pin counts were retired exactly once.
        self.assertFalse(child._dead)
        self.assertEqual(child.refs, 1)
        self.assertEqual(child.iterations, 0)
        self.assertEqual(child.get(0), 5)
        self.assertIsNone(first.owner)
        self.assertIsNone(second.owner)
        first.drop()
        second.drop()
        self.assertEqual(child.refs, 1)
        child.release()
        self.assert_dead(child)

    def test_foreign_finalizer_sees_stale_slice_and_reenters_gc(self):
        vectors = rt.ValueType('vec')
        slice_ops = rt.ValueType('slice')
        nested = rt.ValueType('struct', (slice_ops,))
        holder_element = rt.ValueType('enum', variants={3: (nested,)})

        first = rt.Vec.create(rt.ValueType(), [])
        second = rt.Vec.create(rt.ValueType(), [])
        ordered = [value for value in rt._LIVE_VECS
                   if value is first or value is second]
        self.assertEqual(len(ordered), 2)
        observer, holder = ordered

        holder.element = holder_element
        holder.push((3, ((rt.Slice.blank(),),)))
        stale = rt.Slice.create(holder, 0, 1)
        holder.set(0, (3, ((stale,),)))
        holder.release()
        self.assertEqual((holder.refs, holder.iterations), (1, 1))

        events = []

        class Foreign:
            def __del__(self):
                active = rt._GC.active
                try:
                    stale.get(0)
                    stale_rejected = False
                except rt.Panic:
                    stale_rejected = True
                blank = (stale.owner is None and stale.start == 0 and
                         stale.length == 0)
                stale.drop()
                stale.drop()
                nested_cycle = rt.Vec.create(vectors, [])
                nested_cycle.push(vectors.copy(nested_cycle))
                nested_cycle.release()
                events.append({
                    'active': active,
                    'stale_rejected': stale_rejected,
                    'stale_blank': blank,
                    'holder_iterations': holder.iterations,
                    'nested_collect': rt.collect_cycles(),
                    'nested_dead': nested_cycle._dead,
                })

        observer.element = rt.ValueType(
            'struct', (vectors, rt.ValueType('foreign')))
        observer.push((vectors.copy(observer), Foreign()))
        observer.release()
        self.assertEqual((observer.refs, observer.iterations), (1, 0))
        self.assertTrue(rt.collect_cycles())

        self.assertEqual(events, [{
            'active': False,
            'stale_rejected': True,
            'stale_blank': True,
            'holder_iterations': 0,
            'nested_collect': True,
            'nested_dead': True,
        }])
        self.assert_dead(observer)
        self.assert_dead(holder)
        self.assertIsNone(stale.owner)
        self.assertTrue(stale._dead)
        self.assertEqual(len(rt._GC.white_ids), 0)

    def test_pressure_keeps_live_root_and_discards_slice_cycles(self):
        scalar = rt.ValueType()
        slice_ops = rt.ValueType('slice')
        element = rt.ValueType('struct', (slice_ops,))
        root = make_vec(scalar, [99])
        baseline = set(rt._LIVE_VECS)
        self.assertEqual(baseline, {root})
        collections = rt._gc_stats()['collections']

        for iteration in range(8192):
            value = rt.Vec.create(element, [])
            value.push((rt.Slice.blank(),))
            value_edge = rt.Slice.create(value, 0, 1)
            value.set(0, (value_edge,))
            value.release()
            rt._gc_poll()
            self.assertFalse(root._dead)
            self.assertEqual(root.refs, 1)
            if iteration % 64 == 0:
                self.assertEqual(root.get(0), 99)

        self.assertTrue(rt.collect_cycles())
        self.assertEqual(root.get(0), 99)
        self.assert_live(root)
        self.assertEqual(set(rt._LIVE_VECS), baseline)
        self.assertGreater(rt._gc_stats()['collections'], collections)
        self.assertLess(rt._gc_stats()['peak_live'], len(baseline) + 1024)
        root.release()
        self.assert_dead(root)


if __name__ == '__main__':
    unittest.main()
