# std/deque

`Deque[T]` is a double-ended queue with a ring buffer.

```wz
import "std/deque" as deque;

let queue = deque.new[i32]();
deque.push_back(queue, 2);
deque.push_front(queue, 1);
// deque.pop_front(queue) == Some(1)
```

Copying a Deque copies its shared handle. The original and every alias observe
the same logical deque, including pushes, pops and clear. Values are copied by
the ordinary language rules; a value returned by `pop_*` or `peek_*` is an
owned copy and can outlive the deque. Stored managed values (Vec, Box, function
values, and so on) are retained by their slot and released when the slot is
removed or cleared.

The public functions are:

```wz
pub func new[T]() Deque[T]
pub func with_capacity[T](minimum usize) Deque[T]
pub func reserve[T](values Deque[T], minimum usize)
pub func len[T](values Deque[T]) usize
pub func capacity[T](values Deque[T]) usize
pub func is_empty[T](values Deque[T]) bool
pub func push_front[T](values Deque[T], value T)
pub func push_back[T](values Deque[T], value T)
pub func pop_front[T](values Deque[T]) option.Option[T]
pub func pop_back[T](values Deque[T]) option.Option[T]
pub func peek_front[T](values Deque[T]) option.Option[T]
pub func peek_back[T](values Deque[T]) option.Option[T]
pub func get[T](values Deque[T], index usize) option.Option[T]
pub func to_vec[T](values Deque[T]) Vec[T]
pub func clear[T](values Deque[T])
```

`to_vec` returns a snapshot in front-to-back order. The returned Vec is a
distinct container: later deque changes do not modify it. Its elements are
copied by the ordinary language rules, so nested Vec, Box or other reference
values still refer to the same inner objects as the deque slots.

`push_front` and `push_back` are amortized O(1). A push or `reserve` that
grows the ring takes time proportional to its new capacity. `reserve`
without growth, `pop_front`, `pop_back`, `peek_front`, `peek_back`, `get` and
`is_empty` are O(1). `to_vec` and `clear` are O(len). These bounds assume
constant-time element copies and drops. `clear` retains the ring. An empty
`pop_*`, `peek_*` or out-of-range `get` returns `option.Option.None`; it does
not throw or panic.

`capacity` counts ring slots, not live elements. There is no automatic
shrink. A reserve request too large for Vec storage Panics through the same
checked allocation path as `std/vec`; capacity doubling is checked before it
could wrap. Deque is not synchronized; use a lock when aliases are accessed
from multiple threads.
