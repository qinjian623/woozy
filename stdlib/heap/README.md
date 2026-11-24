# std/heap

`Heap[T]` is a binary priority heap over a shared Vec and an ordering callback
stored at construction.

```wz
import "std/heap" as heap;
import "std/option" as option;

func before(left i32, right i32) bool { return left > right; }

let queue = heap.new[i32](before);
heap.push(queue, 3);
heap.push(queue, 9);
// heap.peek(queue) == Some(9)
// heap.pop(queue) == Some(9)
```

`before(a, b)` returns true when `a` must be removed before `b`. Like
`std/sort`'s `less`, it must define a strict weak order: irreflexive,
transitive, and equivalent elements return false in both directions. Passing
`before` directly to `sort.sort_by` sorts in the heap's removal order; swapping
its arguments sorts in the opposite order. It is not the negation of `less`,
which would also accept equivalent elements. Use `left > right` for a max-heap
and `left < right` for a min-heap. Do not mutate the heap or the compared values
from the comparator; in particular, do not change keys through shared aliases
while they are queued, as that invalidates the stored order. Do not call heap
operations from the comparator: heap operations are not reentrant. A structural
re-entry during a sift Panics on the active iteration before it can invalidate
the sift, but such a comparator is still outside the contract. Equivalent
elements are all retained, but their relative order is not promised.

Heap copies share the backing Vec and use the same comparator. Each copy
retains the comparator's captured environment. Values are copied by the ordinary language rules on
push, peek, pop and `from_vec`; returned Option values own their copies, and the
heap releases its stored copy when an element is popped or cleared.

The public functions are:

```wz
pub func new[T](before func(T, T) bool) Heap[T]
pub func with_capacity[T](before func(T, T) bool, minimum usize) Heap[T]
pub func from_vec[T](source Vec[T], before func(T, T) bool) Heap[T]
pub func reserve[T](values Heap[T], minimum usize)
pub func len[T](values Heap[T]) usize
pub func capacity[T](values Heap[T]) usize
pub func is_empty[T](values Heap[T]) bool
pub func push[T](values Heap[T], value T)
pub func pop[T](values Heap[T]) option.Option[T]
pub func peek[T](values Heap[T]) option.Option[T]
pub func clear[T](values Heap[T])
```

`from_vec` copies `source` into a new backing Vec and builds the heap with
Floyd's bottom-up heapify. The source Vec is a distinct container: later
changes to its length or slots do not affect the heap, and heap operations do
not change the source Vec. Nested Vec or Box values are still copied by the
ordinary rules, so they can refer to the same inner object from both
containers.

`push` is amortized O(log n) and `pop` is O(log n), assuming constant-time
comparison, copying and dropping of elements. `from_vec` takes O(n) time for
`n` source elements when copying and comparison are constant-time. `peek`,
`len`, `is_empty` and `capacity` are O(1) under the same assumption. `clear` is
O(n) and retains the allocated Vec capacity. Popping or peeking an empty heap
returns `option.Option.None`; it does not throw or panic. Heap is not
synchronized; use a lock when aliases are accessed from multiple threads.
