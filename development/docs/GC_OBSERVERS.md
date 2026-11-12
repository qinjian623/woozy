# GC observer migration

Native programs now allocate SourceGC world nodes, registry entries and closure
environments through the same `lang_alloc_v1`/`lang_free_v1` hooks as Vec
descriptors and inline payloads. The old rule `len(allocations) == vec_count` is
therefore invalid.

`run_unbound_methods.py` and `run_receiver_modes.py` keep the exact expected
number and order of Vec events. They also require every heap allocation to be
freed exactly once. `heap_allocation_roles` separates Vec-backed allocations
(an `ALLOC` immediately followed by `VEC:1` for the same pointer) from runtime
metadata without a `VEC:1` event. Error-cleanup lifetime checks apply only to
Vec-backed payloads; GC metadata is not element payload.

Python output is unchanged and still uses the exact allocation count.

# SourceGC cycle migration

`recursive-cycle-retained` used to assert a deliberate one-allocation leak.
SourceGC now clears and collects the self-cycle, so every Vec identity and heap
allocation must be released.

Ordinary final release still requires the complete reverse `event 4` drop order.
A collected cycle is cleared first (`clear` drops elements directly), and its
final `event 5` therefore has no original `event 4` prefix. Only that single
non-empty cycle accepts an empty drop list. Duplicate frees, unknown identities,
frees during iteration, missing frees and any remaining live object still fail.
