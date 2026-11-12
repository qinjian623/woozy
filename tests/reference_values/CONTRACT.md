# Reference values source cases

Task: `woozy-reference-source-cases-20261112`  
Correction operation: `woozy-reference-source-cases-correction-20261112`  
Contract: `compiler-dev/1`  
Candidate worktree base: `9f1ab40`  
Read-only Root CLI/source revision: `68b3cde4a6bb8bd3015a19a81ea635b15fd72e92`  
Status: **planned source cases; the target feature is not implemented at this Root revision**

This directory is an independent source/expected-value package for capturing
closures, bound method values, function values, indirect callee cleanup, thread
capture, and source `Weak`. It does not modify the compiler, runtime, shared
CMake, stdlib, or any RED-test registration. Nothing here claims that the
feature passes.

## 1 Authority and scope

The source cases use the public rules in:

- `docs/SEMANTICS.md` §7.1 (copy/object identity), §7.2 (parameter modes),
  §8 (typed throws/Panic), §9 (defer/destruction), §7.3 (runtime boundary),
  and the section “已确定、尚待实现的闭包与 Weak 规则”.
- `development/plans/REFERENCE_VALUES.md`, including the Root supplement
  “字段交接与未具体化类型”. Only its reviewed interface/behavior rules are
  used; the field schema is not redefined here.

Where the plan and `docs/SEMANTICS.md` describe user-visible behavior, the
user-selected semantics in `docs/SEMANTICS.md` take priority. The source cases
are written against the planned public behavior, not against any private HIR
or runtime spelling.

Out of scope for this package:

- No new compiler/runtime/stdlib API or configuration.
- No temporary mock of `Weak`, closure capture, bound methods, or HIR fields.
- No source registration in an existing RED suite or shared CMake.
- No build/run claim while the feature is absent.

## 2 User-visible behavior covered

### Captures

- A lambda captures the logical value visible at its creation point. Later
  assignment to the creator's scalar binding does not rewrite an existing
  capture. Sibling lambdas are independent snapshots.
- Capture names are read-only inside the lambda. Rebinding or moving a captured
  name is rejected. Mutating shared contents through a captured `Vec`/`Box`
  handle is ordinary aliasing and remains legal.
- A `move func` consumes its owning captures (`Vec`, `Box`, function values,
  etc.) but only copies scalar captures. Reading an owning binding after the
  move is rejected; a scalar binding remains readable/assignable.
- Nested and transitive captures work through an intermediate lambda. The
  original declaration remains the capture origin, while every lambda
  instance has its own local capture binding.
- Generic creator instances (`i32` and `Vec[i32]`) keep the same capture
  origin but get distinct concrete capture types.
- A returned closure may outlive the creator's lexical scope; its captured
  owning values remain valid through the normal copy/retain rules.

### Function values

- A returned capturing function value can be stored in a struct field or a
  `Vec` element, copied, and called through each copy. Copies share the
  environment; they do not take a new snapshot.
- A capturing function that declares `throws` runs its registered `defer` on
  the typed-error path and remains callable after the error. Defer runs once
  per invocation, not once per Fn copy. The post-catch call of the throwing Fn
  in `fn-throws-defer-capture` is legal because `main` declares the same
  `throws Failure` effect; the selected value does not throw at runtime.
- When an indirectly called Fn is an owned temporary, its borrow starts after
  callee evaluation and before argument evaluation. A throwing argument must
  not enter the Fn body; the callee's owning release belongs to the end of the
  full expression, on both success and typed-error exits. The indirect
  fixture carries its event log in a shared `Box[i32]`; no `inout` parameter
  is captured and no capture name is passed as a whole-binding `inout`.
- A thread entry may be a capturing Fn and may still take a scalar `A`.
  `thread.spawn[A]` must copy/drop the Fn environment as well as `A`; the
  source case uses a finite `thread.wait_for` timeout.

  Note: `docs/SEMANTICS.md` §7.3 still records the earlier implementation
  limit “先支持无捕获的 func(A) i64”. The capturing-entry case follows the
  later Root-reviewed handoff in `REFERENCE_VALUES.md` §5 (thread context
  holds `LangFn` then `A`) and is planned with the rest of LANG01. It is not a
  claim that the current base or the old §7.3 wording already permits it. If
  the integrator decides the §7.3 restriction remains authoritative for this
  round, this case must be marked `unsupported` until that decision changes.

### Bound methods

- An ordinary-Self bound method copies the receiver when the method value is
  created. Repeated calls and Fn copies observe that snapshot. A newly bound
  method observes the current receiver. Shared `Vec`/`Box` handles inside the
  receiver still refer to the same object.
- A move-Self bound method retains the receiver in the Fn environment. Every
  call, including calls through a copied Fn, first makes a fresh ordinary copy
  of the receiver and then invokes the move-Self method. The adapter must not
  take/empty the environment field.
- Binding an inout-Self method is rejected because the exclusive borrow would
  escape into a repeatable function value.
- A receiver/capture that is not `ordinary_copyable` must be rejected. A real
  C-resource fixture is deferred to the CABI owner (see §5).

### Weak

- Public API: builtin `Weak[T]` plus `std/weak.empty[T]()`,
  `std/weak.downgrade[T](T)`, and `std/weak.upgrade[T](Weak[T]) Option[T]`.
  There is no `is_alive`/`is_empty`; liveness is observed only through
  `upgrade` returning `Some`/`None`.
- `Vec`, `Box`, and function values are eligible targets. Ordinary by-value
  structs/enums, `string`, descriptor wrappers such as `std/os.File`, and
  foreign Python/C objects are rejected explicitly, not accepted by layout.
- While a strong target handle exists, `upgrade` returns `Some` and observes
  shared mutation. After the last strong handle for an acyclic target leaves
  its lexical block, `upgrade` returns `None`.
- A non-capturing Fn (`env == null`) is an immortal Weak target: repeated
  upgrades return a callable `Some`. A capturing Fn with no strong environment
  left is dead: `upgrade` returns `None` and must never call the captured code.

## 3 Fixtures and expected values

All manifests use `backend = "native"` and `word_bits = 64`; the source is
backend-neutral, and Python/64 is intended to use the same project through the
existing CLI. Positive cases assert `main` return `0` and internal scalar or
container checks; they do not parse stdout.

| # | Fixture | Kind | Independent expectation |
|---|---|---|---|
| 1 | `closure-copy-at-creation-sibling` | runtime | `first()==1`, `second()==2`, `first()==1`, creator `x==3` |
| 2 | `closure-move-owning-and-scalar` | runtime | `scalar==1` before reassignment; `read()==42`; `scalar==2` after reassignment |
| 3 | `closure-move-owning-read-after` | reject | reading the moved owning capture is rejected |
| 4 | `closure-readonly-shared-mutation` | runtime | two calls give `19`, `25`; shared `Vec`/`Box` state is `5`/`20` |
| 5 | `closure-readonly-assign` | reject | assignment to a captured name is rejected |
| 6 | `closure-two-capture-types` | runtime | `read(0)==43`, `read(7)==50` (i32 slot + Vec slot) |
| 7 | `closure-nested-transitive` | runtime | `first(1)==42`, `second(2)==13`, `first(0)==41` |
| 8 | `closure-creator-scope-escape` | runtime | returned Fn after creator scope returns `81` |
| 9 | `closure-generic-two-instances` | runtime | i32 instance `42`; Vec instance element `7`, then `8` after mutation |
| 10 | `fn-return-field-vec` | runtime | field/Vec/copy calls return `15`, `17`, `18` |
| 11 | `fn-throws-defer-capture` | runtime | `main` declares `throws Failure`; calls return `14`, throw, then `16`; log sequence `19192` then `1919219` |
| 12 | `bound-ordinary-repeat-copy` | runtime | snapshot calls `7`, `7`; fresh `9`; total `14` |
| 13 | `bound-move-repeat-copy` | runtime | repeated/copied calls return `0,1,2,3`; cell ends at `4` |
| 14 | `bound-inout-reject` | reject | binding an inout-Self method is rejected |
| 15 | `indirect-callee-arg-throw` | runtime | shared `Box` log `1234` on error path, success `ok==15`, shared log `123415` |
| 16 | `thread-capture-scalar-arg` | runtime | finite wait succeeds; `join==42` |
| 17 | `weak-empty-upgrade-none` | runtime | `upgrade(empty)` is `None` |
| 18 | `weak-vec-lifecycle` | runtime | alive `Some` sees `7`; after drop `None` |
| 19 | `weak-box-lifecycle` | runtime | alive `Some` sees `8`; after both Box aliases drop `None` |
| 20 | `weak-fn-captured-dead` | runtime | alive call `41`; after last strong Fn drops `None` |
| 21 | `weak-fn-immortal` | runtime | two upgrades are `Some` and both calls return `42` |
| 22 | `weak-reject-user-struct` | reject | `Weak[Payload]` for an ordinary struct is rejected |
| 23 | `weak-reject-string` | reject | `Weak[string]` is rejected |
| 24 | `weak-reject-file` | reject | `Weak[os.File]` is rejected |

The machine-readable form is `tests/reference_values/cases.json`. Each entry
contains the manifest path, covered rule keys, expected `main` return, named
checks, and—where relevant—the drop/cleanup condition. Reject entries state
the required rejection behavior and explicitly forbid treating an unrelated
compile failure as a passing negative.

## 4 Planned versus executed

### Executed for the correction with read-only Root CLI `68b3cde`

1. **Syntax only.** Every one of the 24 fixtures was checked with:

   ```sh
   /home/qin/Work/language-dev-20261103/compiler/build/woozy-stdlib-native/woozyc \
     check --syntax-only <fixture>/src/main.wz --format json
   ```

   Result: all exit `0`, `status=ok`, no diagnostics. This proves parsing only,
   not type/effect/ownership checking or execution.

2. **Project check as baseline only.** Every fixture was also checked with
   `woozyc check --project <fixture>/lang.toml --format json` (no
   `--std-binding`). All 24 currently exit `1`:

   - closure/function/thread capture fixtures:
     `E_CAPABILITY_UNAVAILABLE` — “capturing a local binding is unavailable”;
   - all Weak fixtures: `E_IMPORT_NOT_FOUND` — `std/weak` is not provided;
   - bound-method fixtures: `E_TYPE_MISMATCH` — “unknown struct field”, because
     method values are not implemented.

   The corrected `fn-throws-defer-capture` and `indirect-callee-arg-throw`
   baseline outputs contain only the capture-unavailable diagnostics, with no
   `E_EFFECT_UNHANDLED` and no captured/inout-parameter error.
   `closure-move-owning-and-scalar` likewise reports only capture-unavailable
   diagnostics.

3. **Effect-boundary control.** A separate non-capturing control retaining the
   throwing Fn type and the same post-catch call shape was checked read-only:
   the version with `main() i32 throws Failure` is `status=ok`, while the
   version without the declaration reports exactly `E_EFFECT_UNHANDLED`.
   This confirms the corrected `throws Failure` boundary is legal and that the
   old unhandled call was a source error, not a feature gap.

   These diagnostics are recorded as the current baseline, not as expected
   results. In particular, the `bound-inout-reject` baseline failure is the
   same missing-method-value error as the positive bound cases and does **not**
   count as the required inout rejection.

Raw syntax/project JSON, effect controls, command log, and file hashes are
kept under `/tmp/reference-source-cases-correction-20261112/` by the correction
run.

### Planned, not executed

- Native O0/O2 build and execution of all positive fixtures.
- Python/64 generation and execution of the same projects.
- The expected rejections in cases 3, 5, 14, 22, 23, and 24.
- Drop/cleanup observation beyond what source-level checks can express.

No `passed` status is claimed for any runtime or reject case. When the feature
lands, these cases must be run on both backends before they are counted.

## 5 Pending CABI or binding fixtures

These behaviors are specified but intentionally not fabricated here:

- `capture-noncopyable-c-resource`: ordinary capture of a C resource without
  `retain_symbol` must be rejected.
- `bound-noncopyable-c-resource`: binding such a receiver must be rejected once
  its concrete type is known.
- `weak-foreign-python-object`: `Weak[T]` for a foreign Python/C object must be
  rejected explicitly.

They are listed in `cases.json` under `pending_cabi_or_binding_cases`. A real
CABI/external binding owner should add those fixtures with the actual
declaration; this source package must not invent a mock ABI.

## 6 Runner policy if one is added later

A runner may call only the existing CLI. It must:

- require `status=ok` before attempting a positive build/run;
- require the expected `main` return and checks, not just process exit `0`;
- use a finite timeout for thread/time cases;
- for `kind=reject`, require the specific stated rejection family/behavior
  after the feature exists; do not accept every compile failure as a pass;
- report unavailable/unimplemented features as `failed`/`not_implemented` or
  `unsupported`, never as `passed`;
- keep `not_run` distinct from a passing result when a backend or environment
  is absent;
- not parse stdout for positive assertions and not treat panic as normal
  completion.

## 7 Drop and cleanup observations

Source has no user-defined destructor, so direct drop counts are not generally
observable. The cases therefore use:

- repeated move-Self bound calls plus the shared `Box` cell value to detect an
  adapter that empties its environment (`bound-move-repeat-copy`);
- `defer` event logs to observe typed-error cleanup and post-throw reuse
  (`fn-throws-defer-capture`, `indirect-callee-arg-throw`); the indirect case
  uses a shared `Box[i32]` log so the closure captures only ordinary values.
- Weak `upgrade` to observe final strong-handle release for acyclic `Vec`,
  `Box`, and captured Fn values (`weak-*-lifecycle`, `weak-fn-captured-dead`);
- an immortal non-capturing Fn to ensure a null environment is not mistaken
  for dead (`weak-fn-immortal`).

IR-level begin/end borrow and drop-pairing checks remain the responsibility of
the implementation’s native IR negative tests; they are not replaced by these
source cases.

## 8 Files

- `tests/reference_values/fixtures/<id>/lang.toml`
- `tests/reference_values/fixtures/<id>/src/main.wz`
- `tests/reference_values/cases.json`
- `tests/reference_values/CONTRACT.md` (this file)

No shared CMake, compiler source, runtime source, stdlib source, or existing
test registration is changed.

Root final review: the successful indirect call also has throws Failure; its
main now declares that effect. Numeric and cleanup expectations are unchanged.
No runtime result is claimed for these planned cases.
