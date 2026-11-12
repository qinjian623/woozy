# C ABI binding contract (schema v2)

This directory is the executable contract for FFI01. It is independent of the
Python binding schema v1 and of Python's `ForeignBinding.read_error`/host
attribute model. The frozen implementation is Linux x86_64 only, target
`x86_64-linux-gnu`, LLVM/Clang 22.1.8.

## Project manifest

C libraries are declared as native dependencies. Unknown keys are rejected.

```toml
[build]
backend = "native"
word_bits = 64
[build.native]
runtime = "hosted"          # absent or "freestanding" keeps the old link mode

[dependencies.fixture]
kind = "native"
binding = "bindings/fixture.json"
libraries = ["ffi_fixture"] # validated C library names, never shell flags
library_dirs = ["../../.build"]
```

`[build.native] runtime` is `freestanding` (default) or `hosted`. A native
dependency must use a schema-v2 `abi:"c"` bundle. A bundle with
`requires_hosted:true` is rejected with `E_CAPABILITY_UNAVAILABLE` unless the
project selected hosted runtime. Names containing `/`, `\`, embedded NUL, a leading
`-`, or characters outside `[A-Za-z0-9_+.-]` are rejected for libraries.
Directories are resolved relative to the manifest directory, must exist, and
are passed to the toolchain as `-L<canonical-path>` argv elements. No shell or
`sh -c` is used. Duplicate libraries/directories are collapsed in first-use
order.

The default freestanding link remains `-nostdlib -static` with the existing
runtime archive. Hosted links use the normal Clang CRT/driver, the hosted
archive, `-Wl,-u,lang_hosted_startup_v1` and `-pthread`; no TLS or C++ runtime
library is linked implicitly. The generated C adapter is compiled by the fixed
Clang 22 before the final link; failures are reported, leaving no program.

## Bundle shape

```json
{
  "schema_version": 2,
  "contract": "compiler-dev/1",
  "abi": "c",
  "target": "x86_64-linux-gnu",
  "requires_hosted": true,
  "modules": [
    {"logical_path": "fixture/lib", "provider": "native_library", "library": "ffi_fixture"}
  ],
  "types": [
    {"id": "fixture.Point", "module": "fixture/lib", "name": "Point", "kind": "record",
     "size": 16, "align": 8,
     "fields": [
       {"name": "x", "type": "i32", "offset": 0},
       {"name": "y", "type": "i32", "offset": 4},
       {"name": "weight", "type": "f64", "offset": 8}
     ]},
    {"id": "fixture.OwnedResource", "module": "fixture/lib", "name": "OwnedResource",
     "kind": "resource", "release_symbol": "cf_resource_release",
     "retain_symbol": "cf_resource_retain"},
    {"id": "fixture.Callback", "module": "fixture/lib", "name": "Callback",
     "kind": "callback", "result": "i32",
     "parameters": [{"name": "value", "type": "i32"},
                    {"name": "context", "type": "u64"}]}
  ],
  "callables": [
    {"id": "fixture.cf_add_i32", "module": "fixture/lib", "name": "cf_add_i32",
     "symbol": "cf_add_i32",
     "parameters": [{"name": "a", "type": "i32"},
                    {"name": "b", "type": "i32"}],
     "result": {"type": "i32", "ownership": "value"}}
  ]
}
```

Top-level keys are exactly the eight shown; every nested object is also
strictly checked. A callable parameter is always a fixed positional
`{name,type}`; labels, optional parameters and C varargs are rejected. The
source spelling in `name` must be an identifier and `symbol` a C identifier.

### Modules

`logical_path` is the full imported module path (the dependency head is the
first component, for example `fixture/lib`). `provider` is `native_library`.
`library` may be empty; it is added to the link libraries. `std` source
modules are resolved separately and are not affected by this schema.

### Types

* Scalars: `Unit`, `bool`, `i8/i16/i32/i64`, `u8/u16/u32/u64`, `isize`,
  `usize`, `f32`, `f64`.
* Pointers: `Ptr[T]` is read-only, `PtrMut[T]` is writable. The pointee is
  static. `Ptr[Unit]` is an opaque `void *`; it cannot be indexed.
* Buffers: `Buffer[T]` and `BufferMut[T]` are 16-byte values containing a
  typed pointer plus a `u64` length (natural align 8). `T` must be a fixed
  scalar. A `Buffer` is not a `String`, `Vec`, or a raw address. Buffer fields
  inside a C record therefore contribute 16 bytes and natural padding, not 8. It is constructed only by `std/ffi` from a typed
  pointer; there is no implicit conversion from `String`/`Vec`. Indexing a
  buffer checks the length; indexing a pointer does not.
* Named C declarations: `record`, `resource`, `callback`, `opaque`. A record
  needs natural `size`/`align` and field `offset` values; they are checked
  against the schema's natural C layout. Packed records, bitfields and arrays
  are not representable and are rejected. The generated adapter contains
  `_Static_assert(sizeof/align/offsetof)`. A resource is an owning pointer
  with `release_symbol`; `retain_symbol` is required for copies that need
  separate references. A callback is a fixed non-throwing C signature. An
  opaque declaration may only appear behind `Ptr`/`PtrMut`.
* Buffers/functions/records are non-owning values. A resource is an owning
  value: a normal copy retains it, and the last live holder calls the release
  wrapper once. Borrowed pointers are never released.

C type expressions are not implicitly converted to `Any`, `usize`, `String`
or `Vec`; pointer permission is part of the type. A writable pointer may be
passed where a read-only pointer of the same pointee is expected, but not vice
versa. Pointer/buffer indexing and writes are only legal inside `unsafe { ... }`
and writes additionally require `PtrMut`/`BufferMut`.

### Callables and results

`parameters` are required and ordered. `result` is
`{"type": ..., "ownership": "value"|"borrowed"|"owned"|"unit"}` plus optional
`release_symbol`/`retain_symbol`. `unit` requires `Unit`. `owned` requires a
resource type and a release symbol. No C callable has a typed `throws` effect;
C errors are observed through declared values/errno/pointers. The HIR has no
Python `read_error` or host attribute for C symbols.

## ABI lowering

Every C callable gets one generated adapter with a uniform, stable ABI:

```c
void lang_c_adapter_v1_<id>(void *out, const void *a0, ... aN);
```

The adapter copies each argument from Woozy storage using the declared C
layout and calls the real symbol using its real C prototype. Clang/LLVM
therefore performs SysV small-integer extension and C record-by-value
classification; Woozy LLVM never guesses those rules. `out` points to Woozy
storage and the adapter copies the result back. `Unit` is void and passes a
null output pointer. Records use natural layout checked by static asserts.
The adapter is compiled as C11 (`-std=c11 -O2 -fPIC`) with the fixed Clang.

Callbacks are limited to uncaptured, non-throwing source function values whose
complete signature exactly matches the named callback declaration. The callback
ABI is two-sided:

1. The generated C adapter contains a Clang-compiled C trampoline with the real
   callback signature. Clang therefore performs the real SysV classification
   for narrow integers, `bool`, floats, small/large records and record returns.
2. The trampoline stores each C argument and the result in storage and calls a
   stable LLVM-exported bridge `void bridge(void *out, void *const *args)`. The
   bridge loads Woozy storage and calls the language function with the frozen
   Woozy ABI. No record-by-value classification is guessed in LLVM.

There is no closure address, no exception translation and no retained callback
registry in this batch.

## Core IR, ownership and verification

HIR `Call.kind == c_foreign` stores `c_callable` (1-based frozen index),
`signature`, and source-ordered ordinary arguments. Native IR has an
independent `Op::foreign_call` with `c_callable`; no callee value is present.
HIR verification re-resolves the callable from the frozen project, recomputes
the C signature, validates argument order/type/callback identity and the source
call. Native IR verification independently checks the same metadata plus its
HIR origin and evaluation order. `CFieldRead`, pointer/buffer reads and writes
are separately checked against the frozen declaration/permission.

## Supported and not supported here

Covered by `projects/scalars`, `projects/libc` and `projects/threads`:

* every fixed integer width including negative values, `f32/f64`, `bool`,
  `Unit`/void;
* read-only/writable pointers, binary data with NUL, length-bounded buffers,
  unsafe reads/writes;
* C records by value and by field read, with size/align/offset checks,
  including Buffer fields (16-byte `{ptr,u64}` layout);
* owned resources with retain-on-copy and one final release, borrowed values
  without release;
* synchronous closure-free callback with an explicit `u64` context, including
  record parameters/returns, tiny and large records, narrow signed/unsigned
  integers, `bool`, `f32/f64`;
* hosted libc `getenv`, `errno`, `malloc`/`free`, OpenSSL SHA-256, and C TLS
  under concurrent Woozy `std/thread` threads (using the hosted thread timeout
  ABI updated by the runtime owner);
* hosted `std/unicode` NFC/NFD/casefold (the hosted archive includes the
  unicode runtime and vendored utf8proc adapter).

In-tree O0/O2 oracle coverage in `projects/scalars` uses nonzero exit codes
`54` through `62` for Point callback parameter, Point callback return, tiny
record callback, large record callback, narrow signed/unsigned/bool/f32
callback, float callback, Buffer-record field length, Buffer-record field
by-value sum, and narrow `i16` callback return respectively. The same process checks the bound panic marker and
all negative builds.

Not claimed in this batch: C varargs, arrays/unions/packed/bitfield records,
record construction or field assignment, retained callbacks or Rust/Woozy
closures as C function pointers, callbacks that throw, callback registries,
TLS-specific hosted ABI (Root-owned), Windows/macOS/ARM, or C++ symbols.
These remain explicit rejections/not_run rather than silent fallbacks.
