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
