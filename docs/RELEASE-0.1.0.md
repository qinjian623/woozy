# Woozy 0.1.0 original release draft

This draft records the original Linux release scope and the rc.1 checks. For rc.2 installation and supported platforms, see the [current README](../README.md#install).

Woozy is a statically typed programming language with local type inference, structs, enums, generics, trait constraints, modules, typed `throws`, and block-scoped `defer`. This is the first user-facing release; the language and library APIs are still pre-1.0.

## Supported platform

The 0.1.0 release targets Linux x86_64. The final package is not published yet; validation on Ubuntu 24.04 x86_64 is still pending, and no other platform is part of this release scope. The archive contains:

- the `woozyc` self-hosted compiler and the standard library;
- the Native and hosted runtime archives;
- the Python runtime files (`langrt.py` and `unicode18.py`);
- the pinned LLVM tools (`opt`, `llc`, `clang`, and `ld.lld`), Clang builtin headers, and compatibility libraries when needed;
- Woozy's Apache-2.0 `LICENSE` and the third-party notices under `licenses/`.

A system LLVM installation is not required. Keep the extracted `woozy/` directory together, because the compiler locates the standard library, runtimes, and tools relative to itself.

## Install

Download the archive and checksum from the [releases page](https://github.com/qinjian623/woozy/releases). Because this draft targets the unpublished `v0.1.0` release, the commands below take the release tag in `tag`: use `v0.1.0-rc.1` for the original Linux pre-release, or `v0.1.0` (or the published final tag) once the final release is available.

```sh
# Use v0.1.0 after publication or v0.1.0-rc.1 for the original Linux package.
tag=v0.1.0
base="https://github.com/qinjian623/woozy/releases/download/$tag"
curl -fLO "$base/woozy-linux-x86_64.tar.gz"
curl -fLO "$base/woozy-linux-x86_64.tar.gz.sha256"
sha256sum -c woozy-linux-x86_64.tar.gz.sha256
tar -xzf woozy-linux-x86_64.tar.gz
./woozy/woozyc --version
```

The `v0.1.0` package reports its full version and source commit with `woozyc --version`. The published `v0.1.0-rc.1` package reports `woozyc 0.1.0-selfhost (compiler-dev/1)`.

To put `woozyc` on your `PATH`, symlink it into a directory already on your `PATH`, or add the extracted `woozy/` directory directly. For example:

```sh
mkdir -p "$HOME/.local/bin"
ln -s "$PWD/woozy/woozyc" "$HOME/.local/bin/woozyc"
export PATH="$HOME/.local/bin:$PATH"   # Add this line to your shell profile.
woozyc --version
```

The symlink must point into the intact `woozy/` directory; moving only the compiler binary is not supported.

## Hello World

The release archive does not include the repository examples. From a checkout of this repository, build the Hello World example and run the resulting Native program:

```sh
woozyc build --project examples/hello/lang.toml --backend native --out-dir build/hello
./build/hello/program
```

The program prints:

```text
Hello, world!
```

`examples/hello/lang.toml` selects the Native backend and the `app` entry package. The same source can be checked without building with:

```sh
woozyc check --project examples/hello/lang.toml --backend native
```

This example uses `std/os` and `std/io`, so it targets the Linux x86_64 Native backend. The Python backend does not support the Linux system modules.

## Requirements and backend differences

- **Native backend:** produces Linux x86_64 programs and uses the bundled LLVM tools. It supports system interfaces, threads, processes, async/await, C bindings, and standalone executables. Advanced native package builds still use the system Make or Ninja; `--source-make` specifically requires GNU Make.
- **Python backend:** emits `program.py` and `langrt.py`; executing that output requires Python 3.12. The Python backend uses the same language rules but does not support the Linux system modules or async/await.
- **Hosted programs:** programs that use libc-dependent C libraries must select the hosted runtime. Hosted builds need a host C development environment.
- **TLS:** `std/tls` and `std/https` are not part of the default runtime. They require the hosted runtime plus a separately built `libwoozy_tls.a` addon and declared `ssl`, `crypto`, and `stdc++` dependencies, as described in the [standard library guide](STDLIB.md#stdtls-和-stdhttps). OpenSSL 3 is required. Ordinary Native programs do not link OpenSSL or the C++ runtime.

## What is included

- Static types with local inference, integer and floating-point types, `bool`, immutable UTF-8 `string`, and Unicode code point indexing.
- Functions, named arguments, function values, capturing closures, `move`, `inout`, and bound methods.
- Structs, enums, pattern matching, `Vec`, `Slice`, `Box[T]`, `Weak[T]`, and automatic collection of pure Woozy reference cycles.
- Generic functions and types, nominal traits, static dispatch, and `dyn Trait` runtime dispatch.
- Packages, visibility, project manifests, dependency locks, typed `throws`, `try`/`catch`, and `defer`.
- A standard library covering I/O, filesystems, text and collections, JSON, math, time and time zones, random numbers, networking, HTTP/1.1, an async HTTP/1.1 server, Unicode 18 normalization and grapheme clusters, threads and synchronization, channels, tasks, processes, signals, and shared-memory IPC.
- Native async/await with a single-threaded executor, and typed C bindings for records, pointers, buffers, resources, and callbacks.

See the [quick start](QUICKSTART.md), [tutorial](TUTORIAL.md), [standard library guide](STDLIB.md), and [C library guide](FFI.md) for details.

## Verification

This release note is a draft for the unpublished v0.1.0 package. The final archive has not been verified. Before publication, the following checks are still pending on Ubuntu 24.04 x86_64:

- SHA-256 verification and extraction of the final v0.1.0 archive;
- the installed-bundle Native Hello World build and its output;
- Python generation and execution of the iterator fixture;
- dependency-lock checks and project test discovery, execution, and reporting;
- LSP completion, hover, go-to-definition, and project diagnostics;
- GDB checks for Native builds, including source-level Make builds;
- TCP chat and HTTP server tests on loopback;
- language regression tests and the source-level Make bootstrap.

The published v0.1.0-rc.1 check in [GitHub Actions run 44698760308](https://github.com/qinjian623/woozy/actions/runs/44698760308) passed, but it applies only to the RC archive and does not verify the final v0.1.0 package. Other Linux distributions, CPU architectures, and host configurations remain outside this release scope.

## Known limitations

- Native programs target Linux x86_64 only. Windows, macOS, and ARM targets are not provided.
- Default arguments, associated types, template specialization, and non-type or variadic template parameters are not supported.
- Async/await and the Linux system modules are available only on the Native backend, not the Python backend.
- C bindings target the Linux x86_64 C ABI. C++ ABI and name-mangled symbols are not supported directly; C++ libraries need an `extern "C"` interface. C variadic calls, arrays, unions, packed or bitfield records, record construction, and record field assignment are not supported.
- `std/http_server` serves HTTP/1.1 without TLS. TLS and HTTPS use the separate `std/tls` and `std/https` addon described above.
- There is no raw syscall API.
- This release note does not establish or promise a compatibility policy for 0.1.x releases.

Compiler build instructions, implementation notes, tests, and contributor guides are in [development/](../development/README.md).
