# Weak source Native verification results

Operation: `native-source-weak-operations-20261112`

Environment: Linux x86_64, LLVM/Clang/LLD `22.1.8`
(`${WOOZY_TOOLCHAIN_ROOT}/llvm-22.1.8`). The legacy
`ld.lld` driver needs
`LD_LIBRARY_PATH=${WOOZY_TOOLCHAIN_ROOT}/libxml2-legacy-2.13.9/usr/lib`.
Build timeout 300 s, program timeout 120 s, no C++ ASAN.

## Source Weak runner

```sh
: "${WOOZY_TOOLCHAIN_ROOT:?set WOOZY_TOOLCHAIN_ROOT to the external toolchains directory}"
python3 tests/native/weak_source/run_weak_source.py \
  --cli /tmp/weak-test-build/woozyc \
  --helper /tmp/weak-test-build/native/lang_native_helper \
  --root . \
  --ld-library-path "${WOOZY_TOOLCHAIN_ROOT}/libxml2-legacy-2.13.9/usr/lib" \
  --out /tmp/weak-source-final
```

Result: `status=passed`. Every positive built and returned `0` at O0 and O2,
with empty stdout/stderr, and every default link had no `readelf -d` `NEEDED`
entry (static freestanding ELF). Full per-command records and 100+ artifact
hashes are in `/tmp/weak-source-final/summary.json` and `commands.json`.

| fixture | O0 return | O2 return | program sha256 (O0) |
| --- | ---: | ---: | --- |
| `root-weak-empty-upgrade-none` | 0 | 0 | `2c8c473f33b66d2d252e8fa5e3f6c1169adcba181c87a95201d2e2fca91249e4` |
| `root-weak-vec-lifecycle` | 0 | 0 | `3dbf2d70c61d9a8d3a5ffa17620a7dab6c473aa7e6cb3c887b08eed991b619be` |
| `root-weak-box-lifecycle` | 0 | 0 | `280ec1d99a3fb37f3581b0099d5e00414f97c7b8e285412632fa32b1d6ab6863` |
| `root-weak-fn-immortal` | 0 | 0 | `132e5cbc3a48bf8c0a4e5c4e8ff3299f3ecaa6fe2fe85907f517527122e853d9` |
| `alias-liveness` | 0 | 0 | `32ef0640000f7a91943499f531943bee59418629dd26e98bdc6e3aa046b51560` |
| `canonical-handles` | 0 | 0 | `447f595b316090e6b52372bd908fc1c8a98d6389615354e22ade089e32304333` |
| `generic-fields` | 0 | 0 | `43eef0e8091c161f141f002cf74b71d1b03aec2632f268844a201c1fe193eea4` |

NativeIR hashes are identical at O0 and O2 per fixture (for example
`root-weak-vec-lifecycle` `5a20db18cf1e184824a7cff57e535f7df10172b721f93e8cc710aa33e0bbdf33`),
confirming the Weak operations are the existing `system_call` path. Sample
LLVM facts checked for every positive:

- `lang_weak_downgrade_v1` / `lang_weak_upgrade_v1` calls are present;
- `LangVec.weak_control` is addressed at struct field 9 (byte 72) and
  `LangHandle.weak_control` at field 3 (byte 24);
- `weak.fn.env` / `weak.fn.immortal` / `weak.fn.dead` branches preserve the
  `{null, code}` immortal encoding and the `{null, null}` dead encoding;
- `upgrade` builds the canonical `std/option.Option[T]` `None`/`Some` payload
  and never calls the stored code on the dead path;
- the NativeIR dump contains `system_call` and no new Weak opcode.

## Reject / unsupported results

| fixture | result |
| --- | --- |
| `root-weak-reject-user-struct` | `E_TYPE_MISMATCH`, `Weak target must be ...`, no program |
| `root-weak-reject-string` | `E_TYPE_MISMATCH`, `Weak target must be ...`, no program |
| `root-weak-reject-file` | `E_TYPE_MISMATCH`, `Weak target must be ...`, no program |
| `weak-reject-slice` | `E_TYPE_MISMATCH`, `Weak target must be ...`, no program |
| `weak-reject-ipc-wrapper` | `E_TYPE_MISMATCH`, `Weak target must be ...`, no program |
| `weak-reject-fake-box` | `E_TYPE_MISMATCH`, `Weak target must be ...`, no program |
| `weak-32bit` | `E_CAPABILITY_UNAVAILABLE` (native/64 ABI only), no program |
| `root-weak-fn-captured-dead` | `unsupported`: `E_CAPABILITY_UNAVAILABLE`, `capturing a local binding is unavailable`; not counted as passed |

Not run: foreign Python/C Weak target (no accepted binding fixture;
`pending_binding_fixture`), Source automatic GC poll/register/trace (separate
phase), and Python backend Weak emission/execution (separate owner).

## Regressions

Built with the same LLVM 22.1.8 toolchain and run with CTest:

```sh
ctest --output-on-failure -R '^(reference_frontend\.weak|hir\.reference_values_schema|sema\.concurrency|native\.reference_values_backend|native\.trivial_vec|native\.ownership|native\.c_ffi|native\.concurrency|native\.threadpool|native\.weak_objects|native\.weak_objects_hosted|native\.vec_runtime|native\.thread_runtime|native\.sync_runtime|native\.arc_concurrency|native\.alloc_concurrency|native\.ir|native\.aggregates|native\.vec_scratch|native\.vec_constant|native\.vec_negative_constant|native\.box_source|native\.generics|native\.cleanup|native\.system_ir|native\.c_ffi_forged)$'
```

Result: `26/26` passed (`125.35 s`), including the existing Fn ABI, C FFI,
ownership, Vec, Box, concurrency and thread suite. No failures or skips in that
selected set.

## Files / delta from the Root snapshot

`reference-source-integration-snapshot.json` still matches all 28 files except:

- changed: `src/backend/native/emit.cpp`
  `701ec6e3c439512ebe5d03514535ba8781f16eb763b9d3fe54dcec9186595ec0`
  (was `8b760410e1958fee91e8c45bb69b8f4b47dea6afe0161efedb87d8968aad3ddb`)

Added (owned):

- `src/backend/native/weak_ops.inc`
  `df106ed8666ea52453aee5350d625b9bfcef0ecb3fccf8fc4ce3be412a841180`
- `tests/native/weak_source/` (runner, README, fixtures, Root registration patch)
- `tests/native/weak_source/run_weak_source.py`
  `b09678382f7f8a7106067d7147c80051b6aa8c5bb858e6a8fdf8c2c49ecccb0e`

No Sema/HIR/IR/module/stdlib/runtime/Python/shared-CMake file was changed.
`tests/native/CMakeLists.txt` is untouched; `CMake-registration.patch` is the
proposed Root-owned registration.
