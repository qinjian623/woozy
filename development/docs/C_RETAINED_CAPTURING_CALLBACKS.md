# Capturing C callbacks: implementation contract

Status: implemented; main-directory full CTest passed 176/176 (191.60 seconds).
Only Linux x86_64 and explicitly selected hosted mode are covered. Existing
static callbacks, other C resources and schema-v2 declarations keep their ABI.

The user chose: userdata APIs only; retain captures for the registration;
unregister waits for in-flight calls; callbacks do not throw.

## Binding and Source

Callback declarations keep their complete C signature. An optional resource
`registration` and callable `registers` describe the lifetime and userdata:

```json
{"id":"events.Subscription","module":"events/lib","name":"Subscription",
 "kind":"resource","registration":{
   "callback":"events.Callback","unregister_symbol":"events_unsubscribe"}}
```

```json
{"id":"events.subscribe","module":"events/lib","name":"subscribe",
 "symbol":"events_subscribe","parameters":[
   {"name":"callback","type":"events.Callback"},
   {"name":"userdata","type":"PtrMut[Unit]"}],
 "result":{"type":"events.Subscription","ownership":"owned"},
 "registers":{"callback_parameter":"callback",
   "userdata_parameter":"userdata","callback_userdata_parameter":"context"}}
```

The callback type's `context` parameter must also be `PtrMut[Unit]`; its position
may be first, middle or last. Source omits only the callable's userdata argument
and replaces its callback argument with a normal nonthrowing function type
whose parameters omit the callback's userdata slot. Function variables,
capturing closures and bound method values work through the existing Fn ABI.
Source creates no userdata pointer and uses no new syntax or builtin type.

The parser freezes `CTypeDecl.registration` and `CCallableDecl.registers` using
the structs in project.hpp; numeric indexes refer to raw C parameters.
`modules::c_source_parameter_indices` and `hir::detail::c_callable_signature`
define the source order/signature once. HIR and IR keep their normal Call form.

A registration resource is move-only. It has no user-declared retain/release
symbol; its internal release_symbol is set to unregister_symbol for existing
ownership checks. A constructor must have registers metadata and return that
owned resource; no other callable may create a raw result of this type. A
callback type uses one consistent callback_userdata_parameter across its
registration constructors. Binding errors fail before any output.

A callable with the unregister symbol, one parameter of the matching resource
type and Unit result is the explicit close operation; its adapter calls runtime
close instead of passing a wrapper to C. Any conflicting declaration of that
symbol is invalid. Exporting a close callable is optional; lexical cleanup
uses the unregister symbol even when it is not exported. Other C calls may borrow the raw token through a call-scoped
lease; they must not retain it after returning. Closed resources cannot be
borrowed. Null resources pass a null token, matching existing nullable C results.
Managed registration resources may not appear in callback parameter/result
types; ownership across that callback boundary is not defined in this version.

Register returns an opaque pointer token; null means failure. Status/out-param
registration APIs, APIs without userdata, multiple managed callbacks per call,
and non-hosted use are rejected. On null failure the C API must already guarantee
no future or in-flight callback, including a dispatch holding userdata before
entering the trampoline. Unregister must offer the same complete quiescence.
These are library obligations, just like the declared C ABI, not facts the
compiler can prove from a JSON file. No executable-memory/libffi/per-instance
trampoline or global current-closure slot is used.
If a callback borrows its registration's token while unregister is waiting,
the declared C operation must remain memory-safe and reentrant for that token
until the callback returns. Quiescence alone does not establish this: a library
that invalidates the token earlier does not support that callback operation.

## Runtime and generation

The private ABI is runtime/hosted/c_callback.hpp. Context holds a copied LangFn
code/env pair, the foreign token, unregister function, short host synchronization,
in-flight/borrow counts and creating pid. The env retain is an external root;
context is never a GC graph node. Internal lifetime pins protect close/release
reentrancy; Source does not gain resource copying.

1. Emitter creates context ACTIVE before entering the existing C-call SAFE scope.
2. Adapter supplies one generic trampoline per callback type and context as C
   userdata. Context is fully initialized before C can synchronously call back.
3. Adapter returns the raw foreign token; after leaving SAFE the emitter calls
   finish. Failure consumes context and drops its env once, returning null.
4. Generic C trampoline packs all non-userdata arguments and calls a typed LLVM
   bridge with (context,out,args). Bridge enters the existing 32-byte GC callback
   scope, calls runtime enter with a separate 16-byte token, invokes Fn.code with
   env first, converts bool/storage as on the static path, leaves GC scope, then
   calls runtime leave. It never reads Fn after leave.
5. Each other C call with a managed resource uses borrow/end_borrow for the
   whole C call, not a bare pointer getter. Closing rejects unrelated new leases;
   an existing callback/lease may make nested calls before it finishes. TLS
   tokens identify self-waits, including a callback closing an outer lease.
6. Explicit close marks closing, drains prior leases, calls quiescent unregister
   SAFE without a context lock, waits for callbacks, and drops the env ACTIVE
   outside every host lock. Concurrent close is idempotent. Closing one's own
   callback/lease, or reentering close before the foreign stop finishes, Panics.
   After env ownership is detached, same-thread destructor reentry may no-op;
   other closers wait until cleanup completes. Last Source release closes once.

## GC and fork

Collector release must try defer before entering any callback/cleanup scope.
The queued object is the already-registered external root context, not a pure
Wz graph edge: it does not add a retain or remove env while stopped. Its retained
env and reachable graph were therefore live during the collector's scan. After
world resume and clearing the collector busy phase, unregister/drain and env
drop run normally. This is a narrow extension to gc_runtime's deferred external
payload contract; ordinary Fn/Vec/frame payloads must still never be deferred.
An external callback cycle remains externally rooted until explicit close.

PID checks precede host mutexes and foreign calls. In a fork child, inherited
implicit resource release is a no-op including ARC/env/foreign token: those
foreign roots remain until exec/exit. Explicit use/close/registration finish/new callback entry
Panics with an inherited-registration marker. A callback already on the fork
caller's stack only restores TLS on leave and pairs its existing GC leave;
it does not touch inherited counts or locks. New child-created contexts work.
This does not make arbitrary C-library business locks usable after fork.

## Required checks

Runtime: null/success/failure/synchronous registration, external thread and
concurrent callbacks, close waits for a real active callback and foreign-call
lease, idempotent close, self-wait rejection, exactly-once env drop, inherited
implicit release, new child context, and a deferred external-root teardown that
keeps its env alive until resume. Use ordinary controlled C fixtures and existing
GC hooks; no special TLS teardown or arbitrary exception tests are required.

Source O0/O2: local capture escapes, Vec contents survive, function variable,
bound method, multiple registrations with distinct userdata, callback userdata
positions, bool/scalar/Unit return, explicit/automatic close, failure cleanup,
ordinary token borrowing, normal and typed-error scope exits, GC-triggered
cleanup, and source callbacks from C threads. Reject throws/mode/signature/schema
errors with no generated output. Keep existing C static tests unchanged.
Python rejects this Native-only interface. Check default core ELF independence.
