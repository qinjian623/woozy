# reference_frontend

`reference_frontend_test.cpp` is the first source `Weak` frontend stage. It
drives the real lexer/parser/resolver/checker over `.wz` fixtures and checks:

- canonical `Weak[Vec[T]]`, function values, `std/box.Box`,
  `std/thread.Thread`, `std/sync.Mutex/Condvar/AtomicI64` and
  `std/ipc.Memory` are accepted, including a dependent generic target that is
  re-validated at instantiation;
- `upgrade` returns the canonical `std/option.Option[T]` declaration;
- `Weak[string]`, ordinary and same-shape user structs, `os.File`, `Slice`,
  scalar targets, wrong arity and a bad dependent instance are rejected with
  `E_TYPE_MISMATCH` and the explicit Weak-target reason;
- `Weak` stays owning and non-POD-inout; a whole-binding `inout` and a direct
  private `__woozy_weak_*` call outside `std/weak` are capability errors;
- concrete invalid `Weak` buried in substituted nominal fields/payloads is a
  Sema source `E_TYPE_MISMATCH` with the field declaration as its primary span,
  even in an unexecuted function signature or return type. Covered patterns:
  struct field, enum payload, function signature/return, function-typed field,
  throwing function-typed field, nested generic struct/enum and a nominal
  reached through a function signature. Their `Vec` counterparts, dependent
  generic templates and the existing recursive function-type family stay
  accepted;
- forged HIR (non-canonical interned `Weak` targets, including scalar,
  user/same-shape struct, Slice and a foreign callable type) and forged
  NativeIR system-call metadata (operation swap, origin replacement/drop) are
  `E_IR_INVALID`;
- NativeIR lowers the verified Weak program without a new opcode;
  Python/64 accepts the backend-neutral source rules, and a 32-bit profile is
  rejected with `E_CAPABILITY_UNAVAILABLE` instead of defining another ABI.

An external Python/C object as a Weak target is covered by the policy helper
and by the shared `Weak` target checks, but a real foreign-object fixture is
still pending the binding owner (the Root case package marks it
`pending_binding_fixture`). `std/os.File` and `Slice` cover the descriptor and
borrowed-view rejections.

Runtime emission and execution are **not** part of this test. The Native and
Python emitters plus the runtime Weak value operations are owned separately;
this stage makes no MEM01 execution/completion claim.

The shared CMake file registers `reference_frontend.weak`. The target follows the existing `sema.concurrency` pattern by compiling
`src/ir/lower.cpp` and `src/ir/verify.cpp` into the test executable.

Root also checks nominal Weak fields behind five and seven function-parameter layers; no signature-depth cutoff may silently accept invalid targets. Canonical Box/Thread Weak fields are checked after their declaration fields are populated.
