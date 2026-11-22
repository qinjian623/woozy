# Capturing C callbacks: implementation contract

Status: implemented; main-directory full CTest passed 176/176 (191.60 seconds).
Only Linux x86_64 and explicitly selected hosted mode are covered. Existing
static callbacks, other C resources and schema-v2 declarations keep their ABI.

Design constraints: userdata-based APIs only, captures stay retained for the
lifetime of the registration, unregister waits for in-flight calls, and
callbacks cannot throw.

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

The callback type's `context` parameter must also be `PtrMut[Unit]`; it may
appear first, in the middle, or last. Woozy source omits only the callable's
userdata argument and replaces its callback argument with a normal nonthrowing
function type whose parameters omit the callback's userdata slot. Function
variables, capturing closures, and bound method values work through the existing
Fn ABI. Source code creates no userdata pointer and requires no new syntax or
builtin type.

The parser freezes `CTypeDecl.registration` and `CCallableDecl.registers` using
the structs in project.hpp; numeric indexes refer to raw C parameters.
`modules::c_source_parameter_indices` and `hir::detail::c_callable_signature`
define the source parameter order and signature once. HIR and IR keep their
normal Call form.

A registration resource is move-only. It has no user-declared retain or release
symbol; its internal release_symbol is set to unregister_symbol so existing
ownership checks apply. A constructor must carry registers metadata and return
that owned resource; no other callable may create a raw result of this type. A
callback type must use one consistent callback_userdata_parameter across all of
its registration constructors. Binding errors fail before any output is written.

A callable with the unregister symbol, one parameter of the matching resource
type, and a Unit result serves as the explicit close operation; its adapter
calls runtime close instead of passing a wrapper directly to C. Any conflicting
declaration of that symbol is invalid. Exporting a close callable is optional;
lexical cleanup still uses the unregister symbol when it is not exported. Other
C calls may borrow the raw token through a call-scoped lease and must not retain
it after returning. Closed resources cannot be borrowed. Null resources pass a
null token, matching existing nullable C results. Managed registration resources
may not appear in callback parameter or result types; ownership across that
callback boundary is not defined in this version.

Register returns an opaque pointer token where null means failure. Status or
out-param registration APIs, APIs without userdata, multiple managed callbacks
per call, and non-hosted use are rejected. When registration fails by returning
null, the C API must guarantee that no callback is in flight or will run later,
including a dispatch holding userdata before entering the trampoline. Unregister
must provide the same complete quiescence. These are library obligations, just
like the declared C ABI, not properties the compiler can prove from a JSON file.
No executable-memory, libffi, per-instance trampoline, or global current-closure
slot is used. If a callback borrows its registration's token while unregister is
waiting, the declared C operation must remain memory-safe and reentrant for that
token until the callback returns. Quiescence alone does not guarantee this: a
library that invalidates the token earlier does not support that callback
operation.

## Runtime and generation

The private ABI lives in runtime/hosted/c_callback.hpp. Each context holds a
copied LangFn code/env pair, the foreign token, the unregister function, short
host synchronization state, in-flight and borrow counts, and the creating PID.
The env retain acts as an external root; the context itself is never a GC graph
node. Internal lifetime pins guard close and release reentrancy; Woozy source
does not gain resource copying.

1. The emitter creates the context while ACTIVE before entering the existing
   C-call SAFE scope.
2. The adapter passes one generic trampoline per callback type and uses the
   context as the C userdata. The context is fully initialized before C can
   invoke the callback synchronously.
3. The adapter returns the raw foreign token; after leaving SAFE, the emitter
   calls finish. On failure, finish consumes the context, drops its env once,
   and returns null.
4. The generic C trampoline packs all non-userdata arguments and calls a typed
   LLVM bridge with (context,out,args). The bridge enters the existing 32-byte
   GC callback scope, calls runtime enter with a separate 16-byte token, invokes
   Fn.code with env first, converts bool/storage as on the static path, leaves
   the GC scope, and then calls runtime leave. It never reads Fn after leave.
5. Every other C call taking a managed resource uses borrow/end_borrow for the
   duration of the C call rather than a bare pointer getter. Closing rejects
   unrelated new leases, while an existing callback or lease may make nested
   calls before finishing. TLS tokens detect self-waits, including a callback
   attempting to close an outer lease.
6. Explicit close marks the context as closing, drains prior leases, calls the
   quiescent unregister function while SAFE without holding a context lock,
   waits for active callbacks, and drops env while ACTIVE outside every host
   lock. Concurrent close is idempotent. Closing one's own callback or lease, or
   reentering close before the foreign stop finishes, Panics. Once env ownership
   is detached, same-thread destructor reentry may no-op, while other closers
   wait until cleanup completes. The final Source release closes once.

## GC and fork

Collector release must try to defer teardown before entering any callback or
cleanup scope. The queued object is the already-registered external root
context rather than a pure Wz graph edge: it neither adds a retain nor removes
env while stopped, so its retained env and reachable graph remain live during
the collector's scan. After the world resumes and the collector busy phase
clears, unregister, draining, and env drop run normally. This is a narrow
extension to gc_runtime's deferred external payload contract; ordinary
Fn/Vec/frame payloads must still never be deferred. An external callback cycle
stays externally rooted until explicit close.

PID checks run before acquiring host mutexes or making foreign calls. In a fork
child, implicit release of an inherited resource is a no-op for ARC, env, and
the foreign token; those foreign roots remain until exec/exit. Explicit use,
close, registration finish, or new callback entry Panics with an
inherited-registration marker. A callback already on the fork caller's stack
only restores TLS on leave and pairs its existing GC leave without touching
inherited counts or locks. New contexts created in the child work normally. This
does not make arbitrary C-library business locks usable after fork.

## Required checks

Runtime checks cover null, success, failure, and synchronous registration;
external-thread and concurrent callbacks; close waiting on an active callback
and a foreign-call lease; idempotent close; self-wait rejection; exactly-once
env drop; inherited implicit release; new child contexts; and deferred
external-root teardown that keeps its env alive until world resume. Use ordinary
controlled C fixtures and existing GC hooks; no special TLS teardown or
arbitrary exception tests are required.

Source O0/O2 checks cover escaping local captures, surviving Vec contents,
function variables, bound methods, multiple registrations with distinct
userdata, all callback userdata positions, bool/scalar/Unit returns, explicit
and automatic close, failure cleanup, ordinary token borrowing, normal and
typed-error scope exits, GC-triggered cleanup, and source callbacks invoked from
C threads. Reject throws, mode, signature, and schema errors with no generated
output. Keep existing C static tests unchanged. Python rejects this Native-only
interface. Check default core ELF independence.
