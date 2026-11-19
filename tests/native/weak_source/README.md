# weak_source

Real Source `Weak[T]` execution on the Native backend.

This test drives the published `woozyc build --backend native` driver at O0 and
O2, executes every produced program, checks the default link is a static ELF
with no dynamic `NEEDED` entry, and inspects the emitted verified LLVM/NativeIR
through `lang_native_helper`. It does not construct private C++/IR shapes and
does not bypass the source checker.

## Root fixtures (verbatim)

The four planned positive cases from the Root
`tests/reference_values/cases.json` Weak subset are copied verbatim from
`<dev-work-dir>/integration/reference-source-20261112/tests/reference_values/fixtures`
and run as actual Source O0/O2:

| fixture | source sha256 | expected |
| --- | --- | --- |
| `root-weak-empty-upgrade-none` | `7cb5356f92338e3e29c72eaf44cbeafb3c681934b5b41e4f205cf53299cc5249` | `upgrade(empty)` is `None`, main returns 0 |
| `root-weak-vec-lifecycle` | `df1451da1f1a0e6d35e8261415c1a41df0f3805cd72faab8c63238529d1e0df4` | alive `Some` observes mutation; after the last strong Vec leaves scope `None` |
| `root-weak-box-lifecycle` | `438d16c7fab172831122a87d2f275b15f8b8abb99dcf6033e3776ec3e98b0b19` | alive `Some` observes the shared cell; after both Box aliases leave scope `None` |
| `root-weak-fn-immortal` | `158747ad8c2be51fa04c5b2a7eaab105d2eb848863496f431cf4901fdef581e0` | non-capturing Fn is immortal `{null, code}`; two upgrades both call it |

The three Root target-eligibility rejects are also copied verbatim:

| fixture | sha256 | expected |
| --- | --- | --- |
| `root-weak-reject-user-struct` | `3de4874ffa4ce8ec6decac5fced6b56a31ba61e6114ada32c5abe0cf838d61aa` | `Weak[Payload]` rejected with `E_TYPE_MISMATCH` |
| `root-weak-reject-string` | `4e6794647b4679db5127e745b443087e448f9066f5c650f1495af5aff4d40b5f` | `Weak[string]` rejected with `E_TYPE_MISMATCH` |
| `root-weak-reject-file` | `7a2b5ca03fd2f1b7af8f0a71a666a715dcf2a2bd6aebf538a37c9b15a9c76199` | `Weak[os.File]` rejected with `E_TYPE_MISMATCH` |

`root-weak-fn-captured-dead` (`e4f86db5e56c610de816ba2134af7d2eaf7bd6886115e3a8f93d8bcfbbc651ef`)
is retained for provenance but recorded as **unsupported**: Source capture is
still rejected by the frontend until LANG01. It is never counted as a pass.

Each `E_TYPE_MISMATCH` reject must carry the explicit source message
`Weak target must be Vec, function value, std/box.Box, ...`; an unrelated
compile failure is not accepted as the negative result.

## Additional real Source cases

- `alias-liveness`: Vec and Box aliases keep the canonical slot open until the
  last strong handle leaves its lexical block; then `upgrade` is `None`.
- `canonical-handles`: `std/thread.Thread`, `std/sync.Mutex`,
  `std/sync.Condvar`, `std/sync.AtomicI64` and `std/ipc.Memory` are downgraded,
  upgraded while alive, then observed dead after the final owner drops. The
  Thread case drops the only source `Thread` before upgrading and relies on the
  runner keeping the state alive until the finite `thread.wait_for`/`join`.
- `generic-fields`: generic functions over `Vec[T]`, plus a struct field and an
  enum payload holding `Weak[Vec[i32]]`, are copied and dropped field-wise.
- `weak-reject-slice`, `weak-reject-ipc-wrapper`, `weak-reject-fake-box`:
  borrowed `Slice`, the multi-field `ipc.Mutex` wrapper, and a user struct with
  the same single `cell Vec[i32]` layout are rejected with `E_TYPE_MISMATCH`.
- `weak-32bit`: a valid Weak program in a `word_bits=32` project is rejected
  with `E_CAPABILITY_UNAVAILABLE`; no second 16-byte ABI is defined.

## LLVM / NativeIR assertions

- `weak.empty/downgrade/upgrade` lower to the existing `system_call`; the
  NativeIR dump contains no new Weak opcode.
- The emitter calls `lang_weak_downgrade_v1`/`lang_weak_upgrade_v1` and uses
  the canonical slots: `LangVec.weak_control` at field 9 (byte 72) and
  `LangHandle.weak_control` at field 3 (byte 24).
- `Weak[Fn]` keeps the env-first `code` pointer; `env == null` encodes
  `{null, code}`, a failed downgrade of a real env encodes `{null, null}`, and
  the dead path never calls the stored code.
- `upgrade` builds the canonical `std/option.Option[T]` `None`/`Some` shape and
  transfers the one new strong reference from `lang_weak_upgrade_v1` exactly
  once.

## Running

```sh
: "${WOOZY_TOOLCHAIN_ROOT:?set WOOZY_TOOLCHAIN_ROOT to the external toolchains directory}"
LD_LIBRARY_PATH="${WOOZY_TOOLCHAIN_ROOT}/libxml2-legacy-2.13.9/usr/lib" \
python3 tests/native/weak_source/run_weak_source.py \
  --cli build/woozyc --helper build/native/lang_native_helper \
  --root . --out build/weak-source-results
```

`LD_LIBRARY_PATH` is only needed by the LLVM 22.1.8 `ld.lld` driver in this
environment. Builds use a 300 s timeout; produced programs run with a 120 s
timeout. Results, per-command records, and artifact hashes are written to
`summary.json` and `commands.json` under `--out`.

`CMake-registration.patch` is the proposed registration for the Root-owned
`tests/native/CMakeLists.txt`; this package does not modify that shared file.

## Out of scope (not claimed)

- captured-Fn Weak dead encoding (Source capture blocked until LANG01) —
  recorded `unsupported`;
- foreign Python/C Weak targets — no accepted binding fixture in this
  operation (`pending_binding_fixture`);
- Source automatic GC poll/register/trace — separate phase;
- Python backend Weak emission/execution — separate owner.
