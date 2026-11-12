# SourceGC cycle migration

`recursive-cycle-retained` used to assert a deliberate one-allocation leak.
SourceGC now clears and collects the self-cycle, so every Vec identity and heap
allocation must be released.

Ordinary final release still requires the complete reverse `event 4` drop order.
A collected cycle is cleared first (`clear` drops elements directly), and its
final `event 5` therefore has no original `event 4` prefix. Only that single
non-empty cycle accepts an empty drop list. Duplicate frees, unknown identities,
frees during iteration, missing frees and any remaining live object still fail.
