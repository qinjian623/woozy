# NET02 source TLS/HTTPS validation

This directory contains the real Woozy source-level tests for `std/tls` and
`std/https`. They run on Linux x86_64 only and use the explicitly hosted
OpenSSL 3 addon; nothing here is part of the freestanding runtime.

## Run

Use a fixed-LLVM build of `woozyc` whose source root contains these modules:

```sh
cd <worktree>
python3 tests/native/tls_source/run_tls_source.py \
  --cli build/woozy-stdlib-native/woozyc --out /tmp/woozy-tls-tests
```

Add `--http-regression` to also run the original `tests/native/http/run_http.py`
suite at O0/O2. Every child process is wrapped in a finite timeout.

The runner:

1. builds `stdlib/tls/addon` with the fixed Clang 22.1.8 toolchain;
2. generates throwaway CA, server and expired certificates under
   the output directory using the `openssl` CLI;
3. builds four Woozy projects at `-O0` and `-O2`
   (`tls_client`, `tls_server`, `https_client`, `https_server`);
4. starts independent Python `ssl` peers and real Woozy servers on loopback;
5. writes `summary.txt` and `results.json` under the output directory.

`.build/` is ignored by git. No private key or certificate is committed.
`--out` selects all generated files, including addon, certificates and reports.
CTest supplies a fresh directory through `tests/run_isolated.py`; its total
timeout is 900 seconds. Without `--out`, the default remains `.build/`.

Each project manifest declares exactly:

```toml
[build.native]
runtime = "hosted"

[dependencies.woozy_tls]
kind = "native"
binding = "../../../../../stdlib/tls/bindings/tls.json"
libraries = ["ssl", "crypto", "stdc++"]
library_dirs = ["../../.build/addon"]
```

For each run the runner creates a manifest with absolute source, binding and
addon paths under the chosen output directory. The committed manifests retain
the default `.build/addon` location for manual use.

## Covered cases

At each optimization level:

* Woozy client ↔ Python `ssl` server:
  * 64 KiB deterministic binary payload with clean `close_notify`;
  * TCP truncation without `close_notify` must be `UnexpectedEof`;
  * wrong hostname, wrong CA and expired certificate must be
    `Certificate` failures (observed `verify_result` values are printed);
  * default trust through `SSL_CERT_FILE`;
  * IPv4 SAN verification with `NameKind.Ip`, auto IP detection with no SNI;
* non-blocking client: explicit `WantRead`, observed partial write, poll-based
  retry, 1 MiB echo;
* empty and embedded-NUL host names are rejected as `InvalidArgument`;
* 256 stream resource copies/releases with repeated sockets, checking the
  borrowed descriptor;
* Woozy TLS server ↔ Woozy TLS client: 1 MiB echo;
* Woozy TLS server ↔ Python `ssl` client: 64 KiB echo;
* Woozy HTTPS client ↔ Python HTTPS server:
  * chunked response split into small chunks with a trailer;
  * 204 response with no body;
  * two requests on one persistent connection;
* Woozy HTTPS server ↔ Python HTTPS client:
  * fragmented chunked request with a trailer and binary body;
  * request body echoed byte-for-byte;
  * 204 second response on the same connection;
* Woozy HTTPS server ↔ Woozy HTTPS client: persistent reconnect reuse;
* TLS executables have `libssl.so.3`, `libcrypto.so.3` and `libstdc++.so.6`
  `NEEDED` entries;
* a non-TLS freestanding program built by the same compiler still has no
  `INTERP`/`NEEDED`;
* focused error-cleanup regression (`focused_cleanup.py`): valid 204 followed by
  TCP truncation closes the owned fd exactly once with an fd-reuse proof, an
  HTTP parser failure with the peer holding the connection returns within the
  bounded child timeout without TLS shutdown, and a fatal TLS record skips
  `SSL_shutdown`.

The original low-level TLS ABI tests remain in `tests/native/tls/` and the
original HTTP tests remain in `tests/native/http/`; this directory does not
replace them.

## Files

```text
run_tls_source.py             reproducible runner
python_peer.py                independent Python TLS/HTTP server and client
projects/tls_client/          source client modes: python/echo/nonblock/invalid/resources
projects/tls_server/          source TLS echo server
projects/https_client/        source HTTPS client modes: single/reuse
projects/https_server/        source HTTPS server
projects/request_probe/       https.request error-cleanup probe
focused_cleanup.py            instrumented close-count/no-shutdown regression
focused_close_wrapper.c       fd-reuse and exact-close instrumentation
focused_tls_wrapper.c         fatal-TLS/no-shutdown instrumentation
```

The four project sources and manifests are ordinary Woozy projects. They can be
opened independently once the addon archive exists; the runner is only needed
to generate certificates and orchestrate peers.

`token_checks.py` additionally checks invalid/expired IDs, non-reuse, concurrent
lookup/final-release, fork table locks, and public typed errors at O0/O2. Its
C++ checks also run with AddressSanitizer and UndefinedBehaviorSanitizer.
