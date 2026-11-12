# std/tls addon build (development)

This directory is the standalone addon build description for the explicitly
hosted `std/tls` and `std/https` modules. It is not part of the compiler's root
CMake build and it is not added to the freestanding native runtime.

The archive contains:

* `runtime/hosted/tls.cpp` — the accepted `lang-tls/1` OpenSSL 3 C ABI;
* `runtime/hosted/tls_source.cpp` — small C-owned byte/count/error resources,
  resource wrappers and transport token callbacks for the schema-v2 bindings;
* `runtime/hosted/tls_source.hpp` — the C11-compatible helper header.

## Build

Use the fixed LLVM 22.1.8 Clang toolchain from the project development notes:

```sh
tc=/home/qin/Work/language-dev-20261103/toolchains/llvm-22.1.8/bin
cmake -S stdlib/tls/addon -B build/tls-addon -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER="$tc/clang++" \
  -DCMAKE_C_COMPILER="$tc/clang"
cmake --build build/tls-addon --target woozy_tls -j4
cmake --build build/tls-addon --target woozy_tls_header_probe -j4
build/tls-addon/woozy_tls_header_probe
```

The result is `build/tls-addon/libwoozy_tls.a`. The header probe compiles the
public helper header as C11 and checks the frozen `LangTlsError` size.

## Project manifest

The binding module declares the library name `woozy_tls`. A user project must
select hosted runtime and provide the remaining dynamic dependencies exactly,
without shell flags:

```toml
[build.native]
runtime = "hosted"

[dependencies.woozy_tls]
kind = "native"
binding = "<repo>/stdlib/tls/bindings/tls.json"
libraries = ["ssl", "crypto", "stdc++"]
library_dirs = ["<repo>/build/tls-addon"]
```

`libwoozy_tls.a` contains C++ (the accepted C ABI catches exceptions), so
`stdc++` is an explicit addon dependency. OpenSSL is provided by `ssl` and
`crypto`. The compiler's hosted link mode does not add TLS or C++ libraries
implicitly for programs that do not declare these dependencies.

## Binding ownership

The schema-v2 bundle in `bindings/tls.json` declares only C resources and
callables. Woozy `String`/`Vec` values are never passed as C arrays: path/name
text is copied into a C-owned byte resource, I/O uses C-owned byte/count/error
resources, and only the required copies occur inside `unsafe` blocks.

Resource symbols used by normal Woozy copies:

* `wts_error_retain` / `wts_error_release`
* `wts_count_retain` / `wts_count_release`
* `wts_bytes_retain` / `wts_bytes_release`
* `wts_context_retain` / `wts_context_release` (delegate to `lang-tls/1`)
* `wts_stream_retain` / `wts_stream_release` (wrapper owns one low-level stream)

The low-level stream still borrows the socket descriptor and never closes it.
The helper stream wrapper stores the same descriptor for `net.poll` and drops
the low-level stream exactly once on the last wrapper release.

## Verification

The source-level runner builds this addon itself:

```sh
python3 tests/native/tls_source/run_tls_source.py \
  --cli build/woozy-stdlib-native/woozyc --out /tmp/woozy-tls-tests
```

See [`tests/native/tls_source/README.md`](../../../tests/native/tls_source/README.md)
for the coverage and commands. Certificates stay under the selected output
directory (`.build/` beside the runner by default); no private key is tracked.
