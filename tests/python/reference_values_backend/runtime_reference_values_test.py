"""Private runtime contract probes for the Python reference-value backend.

These tests exercise the *generated-code shapes* published in
development/plans/REFERENCE_VALUES.md against runtime/python/langrt.py
directly. They are runtime-shape tests: they are NOT source-level closure,
bound-method or Weak acceptance, because the shared frontend/HIR stage still
rejects nondefault FunctionValue captures/bound and every Weak type. Nothing
here may be reported as a source capture pass.
"""
import importlib.util
from pathlib import Path
import sys
import unittest

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location(
    'langrt_reference_values_under_test', ROOT / 'runtime/python/langrt.py')
rt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rt)

I32 = rt.ValueType(stride=4)
VEC = rt.ValueType('vec')
FUNCTION = rt.ValueType('function')
WEAK = rt.ValueType('weak')

# The emitter installs exactly one record row; each capture is one field.
ROW_OF_ONE = rt.ValueType('struct', fields=(VEC,))


def one_row(value):
    """Create the environment exactly as emitted code does (single row)."""
    return rt.Vec.create(ROW_OF_ONE, ((value,),))


def cell(value):
    """A shared mutable Box-like cell used by the bound-method cases."""
    return rt.Vec.create(I32, [value])


def tearDownModule():
    if rt._LIVE_VECS:
        raise AssertionError(f'{len(rt._LIVE_VECS)} logical Vec owners leaked')


class EnvironmentRow(unittest.TestCase):
    def test_environment_is_one_borrowed_record_row(self):
        captured = cell(7)
        env = one_row(captured)
        # Vec.create takes the row value; it does not retain a second handle.
        self.assertEqual(env.refs, 1)
        self.assertEqual(captured.refs, 1)
        self.assertEqual(len(env.items), 1)
        row = env.get(0)
        self.assertIs(row, env.items[0])
        self.assertEqual(row[0], captured)
        # Loading a capture field is a borrow: no refcount change at all.
        local = env.get(0)[0]
        self.assertIs(local, captured)
        self.assertEqual(captured.refs, 1)
        self.assertEqual(env.refs, 1)
        env.release()
        self.assertTrue(env._dead)
        self.assertTrue(captured._dead)

    def test_lambda_entry_borrows_and_does_not_drop_capture_fields(self):
        captured = cell(11)
        env = one_row(captured)

        def code(environment):
            # Emitted prologue: one row, one borrowed field.
            _frame = rt.Frame()
            _row = environment.get(0)
            _s0 = _row[0]
            # A borrowed read of the capture (emitted owned reads copy first).
            value = _s0.get(0)
            self.assertEqual(_s0.refs, 1)
            self.assertEqual(environment.refs, 1)
            return value

        function = rt.FunctionValue(code, env)
        self.assertEqual(function(), 11)
        # Function exit does not drop the environment or its fields.
        self.assertEqual(env.refs, 1)
        self.assertEqual(captured.refs, 1)
        self.assertFalse(env._dead)
        # The wrapper owns the environment; dropping it releases the row once.
        function.drop()
        self.assertEqual(env.refs, 0)
        self.assertTrue(env._dead)
        self.assertEqual(captured.refs, 0)
        self.assertTrue(captured._dead)

    def test_owned_read_copies_once_and_child_takes_exactly_once(self):
        captured = cell(3)
        env = one_row(captured)
        frame = rt.Frame()
        key = ('temporary', 1)
        # Creator's ordinary Read produces one owning copy as a temporary.
        frame.put(key, VEC, VEC.copy(env.get(0)[0]))
        self.assertEqual(captured.refs, 2)
        # The FunctionValue capture child takes that exact value; no copy.
        taken = frame.take(key)
        self.assertEqual(captured.refs, 2)
        child_env = rt.Vec.create(ROW_OF_ONE, ((taken,),))
        # Moving the row into the child environment transfers the same edge.
        self.assertEqual(captured.refs, 2)
        child_env.release()
        self.assertEqual(captured.refs, 1)
        env.release()
        self.assertEqual(captured.refs, 0)

    def test_function_value_copies_share_environment(self):
        captured = cell(5)
        env = one_row(captured)

        def code(environment):
            return environment.get(0)[0].get(0)

        function = rt.FunctionValue(code, env)
        copy = FUNCTION.copy(function)
        self.assertEqual(env.refs, 2)
        self.assertEqual(function(), 5)
        self.assertEqual(copy(), 5)
        self.assertIs(function.environment, copy.environment)
        function.drop()
        self.assertEqual(env.refs, 1)
        self.assertEqual(copy(), 5)
        copy.drop()
        self.assertEqual(env.refs, 0)
        self.assertEqual(captured.refs, 0)


class BoundAdapters(unittest.TestCase):
    def test_move_self_adapter_copies_the_receiver_each_call(self):
        state = cell(0)
        env = one_row(state)

        def method(receiver):
            # Declared `move Self`: the callee owns and drops its receiver.
            value = receiver.get(0)
            receiver.set(0, value + 1)
            VEC.drop(receiver)
            return value

        def adapter(environment):
            # Emitted move-Self adapter: fresh ordinary copy per invocation.
            _receiver = VEC.copy(environment.get(0)[0])
            return method(_receiver)

        bound = rt.FunctionValue(adapter, env)
        alias = FUNCTION.copy(bound)
        self.assertEqual([bound(), alias(), bound(), alias()], [0, 1, 2, 3])
        # The environment field was never taken, emptied or cached.
        self.assertIs(env.get(0)[0], state)
        self.assertEqual(state.refs, 1)
        self.assertEqual(state.get(0), 4)
        self.assertEqual(env.refs, 2)
        alias.drop()
        self.assertEqual([bound()], [4])
        self.assertEqual(state.refs, 1)
        bound.drop()
        self.assertEqual(env.refs, 0)
        self.assertEqual(state.refs, 0)

    def test_ordinary_self_adapter_borrows_the_environment_field(self):
        state = cell(9)
        env = one_row(state)

        def method(receiver):
            # Declared ordinary Self: borrowed for the call, no ownership.
            self.assertIs(receiver, state)
            value = receiver.get(0)
            receiver.set(0, value + 2)
            return value

        def adapter(environment):
            # Emitted ordinary-Self adapter passes the borrowed row field.
            return method(environment.get(0)[0])

        bound = rt.FunctionValue(adapter, env)
        self.assertEqual([bound(), bound()], [9, 11])
        self.assertEqual(state.refs, 1)
        self.assertEqual(env.refs, 1)
        # The wrapper owns the environment; releasing it drops the row and the
        # borrowed field exactly once.
        bound.drop()
        self.assertEqual(env.refs, 0)
        self.assertTrue(env._dead)
        self.assertEqual(state.refs, 0)
        self.assertTrue(state._dead)


class IndirectCalleeBorrow(unittest.TestCase):
    def _invoke(self, throwing, events):
        captured = cell(41)
        env = one_row(captured)
        frame = rt.Frame()
        key = ('temporary', 77)

        def code(environment):
            # Must never run when an argument throws before the call.
            events.append('body')
            return environment.get(0)[0].get(0)

        callee = frame.put(key, FUNCTION, rt.FunctionValue(code, env))
        events.append('callee')
        # Emitted order: callee evaluation, borrow, then arguments.
        frame.borrow(key)
        events.append('borrow')
        try:
            events.append('argument')
            if throwing:
                raise rt.TypedError('app.Failure', 1, I32)
            events.append('call')
            result = callee()
        except rt.TypedError:
            # Typed-error exit ends the callee borrow from the verified plan.
            frame.end_borrow(key)
            events.append('error')
        else:
            # Success exit ends the same borrow before the full expression.
            frame.end_borrow(key)
            events.append('success')
            self.assertEqual(result, 41)
        self.assertEqual(frame.borrows, [])
        # The owning callee survives the failing expression; release belongs
        # to the end of the full expression.
        self.assertIs(frame.owners[key].value, callee)
        frame.drop(key)
        self.assertEqual(env.refs, 0)
        self.assertEqual(captured.refs, 0)
        return events

    def test_success_path(self):
        events = self._invoke(False, [])
        self.assertEqual(events, ['callee', 'borrow', 'argument', 'call',
                                  'body', 'success'])

    def test_typed_error_path(self):
        events = self._invoke(True, [])
        self.assertEqual(events, ['callee', 'borrow', 'argument', 'error'])

    def test_borrow_ends_before_arguments_are_evaluated(self):
        # The begin_borrow key is active for the whole argument evaluation and
        # no duplicate borrow remains after either exit.
        frame = rt.Frame()
        frame.borrow(('temporary', 1))
        self.assertEqual(len(frame.borrows), 1)
        frame.end_borrow(('temporary', 1))
        self.assertEqual(frame.borrows, [])


class WeakEncoding(unittest.TestCase):
    def test_weak_descriptor_is_plain_value_operation(self):
        handle = rt.Weak()
        self.assertIsInstance(WEAK.copy(handle), rt.Weak)
        WEAK.drop(handle)  # drop of a weak owner is a no-op

    def test_empty_weak_upgrades_to_none(self):
        self.assertIsNone(rt.Weak().upgrade())

    def test_immortal_noncapturing_function(self):
        def code(value):
            return value + 1

        function = rt.FunctionValue(code, None)
        handle = rt.Weak(function)
        self.assertIs(handle.upgrade(), code)
        self.assertIs(handle.upgrade(), code)
        function.drop()
        # A wrapper with no environment keeps its original callable.
        self.assertIs(handle.upgrade(), code)

    def test_dead_captured_function_never_calls_code(self):
        called = []

        def code(environment):
            called.append(True)
            return environment.get(0)[0].get(0)

        env = one_row(cell(42))
        function = rt.FunctionValue(code, env)
        self.assertEqual(function(), 42)
        handle = rt.Weak(function)
        function.drop()
        self.assertIsNone(handle.upgrade())
        self.assertEqual(called, [True])  # only the live call above
        self.assertTrue(handle.is_empty())
        self.assertEqual(env.refs, 0)


if __name__ == '__main__':
    unittest.main()
