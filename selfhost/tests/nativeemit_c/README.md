# nativeemit C layer (SH08c)

Scope: `selfhost/src/wzcc/nativeemit/c_values.wz`, `c_weak.wz`,
`c_instructions.wz`.  These three files are the C group of the frozen
`nativeemit` package described in `development/docs/SELFHOST_NATIVEEMIT.md`:
source value operations, function environments, thunks, bound receivers,
trait vtables/thunks, thread bridges, Weak runtime operations and the
value-shaped NativeIR instructions.  They are a direct port of
`src/backend/native/value_ops.inc`, `weak_ops.inc`, `concurrency.inc` and the
environment/thunk/bound-adapter/Vec/iterator/trait sections of
`src/backend/native/emit.cpp`, expressed as LLVM text through the Root
`state.wz`/`text.wz` helpers.

Root owns `state.wz`/`text.wz` and the public Verifier/emit wrapper; B, D and E
own their own `b_*.wz`/`d_*.wz`/`e_*.wz`/`system.wz`/`c_ffi.wz`.  C only writes
the C caches and `frame.function_values`; aggregate layout stays behind B's
thin `nativelayout` wrappers, GC registration stays in D, C-resource
retain/release and `stdlib_variant`/`value_address` stay in E.  The tests here
never modify production sources and never manufacture a `VerifiedModule`.

## Layout

```text
c_values.wz       owning/value_op/copy_value/destroy_slot, env_record_type/op,
                  make_environment, function_thunk, bound_adapter,
                  function_value(+_info), trait_box_op/trait_method_thunk/
                  trait_vtable, thread_bridge/thread_spawn, vec RC helpers
c_weak.wz         CWeakProjection and the canonical Vec/Box/handle projection,
                  weak_empty/downgrade/upgrade encodings (weak_system_call)
c_instructions.wz emit_value_instruction: Copy/Move/Take/Destroy/Borrow/Function,
                  Vec init/read/set/slice/length/check/element address,
                  iteration begin/end/test/read/next, TraitObject/TraitCall
driver.wz         package-private cache/Vec/Take driver
branch_driver.wz  package-private raw HIR/NativeIR helpers plus formal
                  `emit_instruction` group-dispatch fixtures
main.wz           app entry that prints one assembled module
corpus/<case>/    source fixtures used for the separate C++ oracle stage
run.py            syntax / oracle / Wz module / branch stages
```

## Checks

```sh
python3 selfhost/tests/nativeemit_c/run.py \
  --woozyc <build>/woozy-stdlib-native/woozyc \
  --helper <build>/woozy-stdlib-native/native/lang_native_helper \
  --toolchain-root <toolchains> \
  --source-root <real-root>/selfhost/src/wzcc \
  --b-source <accepted-B-worktree> \
  --work-dir <unique-dir-outside-the-repo>
```

The default request is `syntax,oracle,wz,branches`.  A missing real dependency
fails the stage; there is no stand-in fallback.  `--b-source` may name the
accepted B worktree (or its `selfhost/src/wzcc/nativeemit`); only `b_*.wz` are
copied from there and every other package comes from `--source-root`.
`results.json` records the source/target SHA-256 of every copied file.

Stage `syntax` uses the real stage0 frontend (`woozyc check --syntax-only`) on
every C file, `driver.wz`, `branch_driver.wz` and `main.wz`.

Stage `oracle` compiles and executes every fixture in `corpus/` with the
existing C++ native backend at O0 and O2 (`woozyc build`), then captures the
pre-optimization IR with `lang_native_helper --out-llvm-before` and requires the
C-owned helper symbols that fixture exists to exercise.  This remains a
source-level differential oracle, not a Wz emission proof.

Stage `wz` copies every real app dependency from `--source-root`
(`model/types/hir/nativeir/valueops/nativelayout` plus the whole `nativeemit`
package) and runs `driver.wz` through the real Wz pipeline.  The printed
cache/Vec/Take module is checked for the C symbols and passed to
`opt -passes=verify` and `opt -passes=verify,default<O2>,verify`.

Stage `branches` uses `branch_driver.wz`.  Each branch is emitted twice
(`Options.optimization` 0 and 2); the two texts must be byte-identical.  The
original module is verified at O0 and O2, then each verified module is compiled
by `llc` with the matching `-O` level, linked against the published
`liblang_native_runtime.a` with the same `clang`/`ld.lld` flags as
`nativebuild.wz`, and both executables are run:

| branch | paths covered | runtime assertions |
| --- | --- | --- |
| `fn` | noncapturing thunk/`function_value`; dynamic capture `FunctionValue` through the real HIR origin/capture schema and `function_value` -> `make_environment` (memcpy, Vec TBAA, `gc_register_fields`); bound ordinary/move-Self `FunctionValue` -> `bound_adapter` + `make_environment`; direct helper env copy/drop controls | closure result, `FunctionValueInfo`, copy/drop refcounts, repeated bound calls |
| `weak` | direct `weak_system_call` 201/202/203 helper paths; immortal/live/dead encodings; Weak copy/drop | Option `None`/`Some` tags and strong-env counts |
| `dyn` | direct `trait_vtable`, `trait_box_op`, `trait_method_thunk`, TraitObject helper copy/drop | vtable dispatch and box refcounts |
| `thread` | direct `thread_bridge`, `thread_spawn` helper path | spawned result, wait, handle release, reverse context cleanup |
| `ops` | formal B `emit_instruction` group loop: `Op.TraitObject`, `Op.TraitCall`, `Op.SystemCall` 201/202/203 (E forwards Weak), and `Op.SystemCall` 13 (ThreadSpawn) | raw-op dispatch result, Weak Option tags, thread result/wait/cleanup |

The `fn` module's dynamic capture/bound construction uses
`make_environment`; its LLVM contains `llvm.memcpy`, Vec TBAA operands and
`lang_gc_program_register_v1`.  The raw `ops` fixture is not a
`VerifiedModule`; it is raw HIR/NativeIR fed to the real instruction dispatch.

## Result semantics

* A requested stage that is blocked, not run or failed makes the overall result
  non-passing (`blocked`/`partial`) and returns non-zero.  There is no
  `allow-partial` switch that turns this into a full pass.
* `--only syntax` or `--only oracle` may report the requested subset as passed
  (exit 0), but still writes `"status": "partial"`, `"C_complete": false` and
  `not_run` entries for stages that were not requested.
* The complete requested set `syntax,oracle,wz,branches`, all passed, writes
  `"status": "passed"` and `"C_test_stages_passed": true`.
* `C_complete` stays `false` and `language_backend_complete` stays `false`.
  Raw fixture and raw-op success is not a claim that the public
  NativeVerifier->emit entry or the full CPP-vs-Wz corpus has passed.

## Latest recorded run (2026-11-13)

Real packages:

```text
Root: /home/qin/Work/language-dev-20261103/compiler/selfhost/src/wzcc
B:    /home/qin/.local/state/cli-agent-bridge/worktrees/7eb82b7efb6eb2e477f28b9a
```

Frozen hashes:

```text
C (unchanged)
fe6865b7ca8518ca188e5252187b9440d5f2d310d3c87b22b49ad4a7a9b672cf  c_values.wz
39e8615daeaa2b817622a674d5ac911396ba430c1e3342e528bee7563bb44815  c_weak.wz
05ea6d275d360483bbcbb21036e2f6bc52cc23a9361b022408ca098eb5d2bec2  c_instructions.wz

B (accepted worktree, also present byte-identical in Root)
2fc84f442359429e6abe6db015e326718da806f0aef63cb2a0aaa141dcc96d5d  b_function.wz
d96702589c045703fd3d345b40cf83a9226b9352142b6c09e59e02472a397a91  b_ops.wz
d5713db0c3a7fba5cd74ff961f2501bdc14c443b68a2bc64f577906e875e3498  b_type.wz

19149ef4e0cb4a76d6dfd52425eb4c3a552004bf04d84577aa9dd7785723a460  valueops.wz
e8a6f64eeedda2a833a905a46f73416ae87c54c7c8ec094d0c841719cc02608c  nativelayout.wz
```

Latest full-run work directory:
`/tmp/sh08c_latest_default_20261113_v2/woozy-nativeemit-c-xvu2o7cs`.

The latest full run writes:

```text
status: passed
stage_status: syntax=passed oracle=passed wz=passed branches=passed
C_complete: false
C_test_stages_passed: true
language_backend_complete: false
not_run: []
```

Wz cache/Vec/Take module: 3266 bytes,
`a6aed5aea84359b851814ff029bb7e4e9838e54dc84f0eac65107bfcd2902ca6`;
both opt pass sequences exit 0.

Branch modules and both real executable levels:

| branch | LLVM bytes | LLVM SHA-256 | O0 exit | O2 exit |
| --- | ---: | --- | ---: | ---: |
| fn | 21983 | `86172f1248d0fa47814f8d78ece0c86290876b87ad733b999dadc14cbef29238` | 0 | 0 |
| weak | 11472 | `512b01a96e28080949b77b2a81d7192027222e4bbee34b05afecf7e494ed22a9` | 0 | 0 |
| dyn | 4841 | `53de4bdace93d249ee0d01bfdfdede8dc3754ff945e04cc989559e0622052cbe` | 0 | 0 |
| thread | 7291 | `e85de2560ad6320d404ff1895e15bd2ee45e7f21f9944512edf32ad3817e8f90` | 0 | 0 |
| ops | 16701 | `d35d6fc73c5d4155afa5c20ededbc551f4d47296a0a05399be5a7cc7718ce391` | 0 | 0 |

Every branch module was verified at both opt settings, and both separately
linked `-O0`/`-O2` executables exited 0 with empty output.  The branch stage
reuses the real `wz-out/program` built by the preceding `wz` stage.

## Known interface issue retained

`declare_function` still cannot spell `ExternalWeakLinkage`.  C emits the
test-only Vec observer as an extern_weak *object* address probe
(`@"lang_vec_observe_v1" = extern_weak global i8`, compared against null).
Resolution is identical (non-null when the observer is linked, null when it is
not); the existing driver checks the probe.  This is a shared-API limitation,
not a reason to add a second declaration API here.

## Not run here

* Public `nativeemit.emit` / NativeVerifier -> VerifiedModule integration and
  the full CPP-vs-Wz differential over the whole Native O0/O2 corpus.  Those
  are separate Root tasks; this suite does not claim them.
* A complete language-backend acceptance: `C_complete` and
  `language_backend_complete` remain false.
* Generic HIR construction or a verifier stub: the branch fixtures are raw,
  explicit, fixture-specific records only.
