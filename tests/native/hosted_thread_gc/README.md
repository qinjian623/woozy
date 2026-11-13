# Hosted Thread cleanup during cycle collection

A real Woozy closure/Vec cycle retains a completed Thread. The O0/O2 check
waits for the runner to drop its own reference, then requires pthread join and
condition/mutex destruction to happen outside the GC kernel. The existing
allocation observer also requires zero live runtime/Vec allocations.

The runner has already dropped its Woozy context when the final Thread
reference is released. Only its remaining host storage is deferred; no Woozy
strong edge is put in the external-release queue. The manifest explicitly
selects the hosted runtime. Run `ctest -R '^native.hosted_thread_gc$'`.
