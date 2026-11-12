# Woozy NET02: explicitly selected hosted TLS

This directory validates the low-level hosted TLS ABI in
`runtime/hosted/tls.hpp` / `runtime/hosted/tls.cpp`.  It is an explicitly
selected hosted runtime component: a program that uses it links a normal CRT,
pthreads, `libssl` and `libcrypto`.  A program that does not select it keeps the
existing freestanding native runtime and does not link libc/OpenSSL.

The implementation uses OpenSSL 3 for protocol, certificate and cipher work.
No cryptographic algorithm is implemented here.  The tests below are real
loopback TLS tests against independent Python `ssl` and OpenSSL `s_client`
peers; they are not mocks.

This task adds only:

```text
runtime/hosted/tls.hpp
runtime/hosted/tls.cpp
tests/native/tls/
```

It does not change `runtime/native/`, `runtime/hosted/start.*`,
`runtime/hosted/thread.*`, CMake, the compiler or the standard library.  It also
does not add a Woozy source-level type or an HTTPS client.  Root still owns the
hosted C ABI/link-mode integration.

## C ABI

All declarations are in `runtime/hosted/tls.hpp`, are usable from C11, and use
the exported prefix `lang_tls_*_v1`.  Every C++ function is `noexcept`; exceptions
are caught before crossing the ABI.

### Opaque objects and references

```c
typedef struct LangTlsContext LangTlsContext;
typedef struct LangTlsStream LangTlsStream;
```

Both objects are opaque.  Each has an internal `uint64_t` atomic reference
count, but callers must not depend on the layout and these objects deliberately
do not use `LangHandle`.

```c
void lang_tls_context_retain_v1(LangTlsContext *) noexcept;
void lang_tls_context_release_v1(LangTlsContext *) noexcept;
void lang_tls_stream_retain_v1(LangTlsStream *) noexcept;
void lang_tls_stream_release_v1(LangTlsStream *) noexcept;
```

Creation returns one reference.  `retain` adds one; `release` drops one and
destroys at zero.  Null retain/release is a no-op.  A stream takes its own
context reference at creation and releases it only after `SSL_free` on the last
stream reference.  Releasing all caller-held context references while a stream
is alive is therefore valid.  Releasing a stream frees the `SSL`/custom BIO but
does **not** close the borrowed fd.

### Context creation

```c
LangTlsContext *lang_tls_context_client_default_v1(LangTlsError *) noexcept;
LangTlsContext *lang_tls_context_client_v1(
    const uint8_t *ca_file, uint64_t ca_file_length,
    LangTlsError *) noexcept;
LangTlsContext *lang_tls_context_server_v1(
    const uint8_t *certificate_chain_file, uint64_t certificate_chain_length,
    const uint8_t *private_key_file, uint64_t private_key_length,
    LangTlsError *) noexcept;
```

* `client_default` calls `SSL_CTX_set_default_verify_paths` (the platform
  OpenSSL trust store; `SSL_CERT_FILE`/`SSL_CERT_DIR` still follow OpenSSL).
* `client` loads exactly the supplied PEM CA file.
* `server` loads a PEM certificate chain and PEM private key with
  `SSL_CTX_use_certificate_chain_file`, `SSL_CTX_use_PrivateKey_file` and
  `SSL_CTX_check_private_key`.  A missing file, bad PEM, or mismatched key is a
  `CERTIFICATE` failure.
* Both roles set `SSL_CTX_set_min_proto_version(TLS1_2_VERSION)`.
* Client contexts set `SSL_VERIFY_PEER`; server contexts do not request client
  certificates.  No insecure verification mode is exposed.

### Stream creation

```c
LangTlsStream *lang_tls_stream_client_v1(
    LangTlsContext *context, int32_t fd, LangTlsNameKind name_kind,
    const uint8_t *name, uint64_t name_length,
    LangTlsError *) noexcept;

LangTlsStream *lang_tls_stream_server_v1(
    LangTlsContext *context, int32_t fd,
    LangTlsError *) noexcept;
```

`fd` is borrowed for the stream lifetime and is never closed by this ABI.
Negative fds and context/stream role mismatches are rejected.  A client stream
requires a non-empty expected name of at most 255 bytes without embedded NUL.
`LANG_TLS_NAME_AUTO` parses syntactic IPv4/IPv6 literals and otherwise treats
the name as DNS.  Explicit `DNS` and `IP` kinds are available.

* DNS: `SSL_set1_host` performs the identity check (wildcard rules are
  OpenSSL's standard policy), and `SSL_set_tlsext_host_name` sends SNI.
* IPv4/IPv6: `X509_VERIFY_PARAM_set1_ip` performs the IP identity check; SNI is
  never sent for an IP.
* Bracketed URL-authority syntax such as `[::1]` is rejected.  IDNA conversion,
  zones and URL parsing are not performed by this ABI.

### Handshake, I/O and shutdown

```c
LangTlsStatus lang_tls_handshake_v1(LangTlsStream *, LangTlsError *) noexcept;

LangTlsStatus lang_tls_read_v1(
    LangTlsStream *, void *data, uint64_t capacity, uint64_t *out_count,
    LangTlsError *) noexcept;

LangTlsStatus lang_tls_write_v1(
    LangTlsStream *, const void *data, uint64_t length, uint64_t *out_count,
    LangTlsError *) noexcept;

LangTlsStatus lang_tls_shutdown_v1(
    LangTlsStream *, LangTlsError *) noexcept;
```

Status values:

| Value | Meaning |
| --- | --- |
| `LANG_TLS_STATUS_OK` | handshake complete, bytes transferred, or shutdown complete |
| `LANG_TLS_STATUS_WANT_READ` | caller must poll for readability and retry the same call |
| `LANG_TLS_STATUS_WANT_WRITE` | caller must poll for writability and retry the same call |
| `LANG_TLS_STATUS_EOF` | **read only**: peer sent a clean TLS `close_notify` |
| `LANG_TLS_STATUS_ERROR` | inspect `LangTlsError` |

`out_count` is always written for read/write.  On `OK`, a read count is positive
and a write count may be a partial count.  Zero-length read/write is a no-op
with count 0.  A single call accepts lengths representable by `SIZE_MAX`.

Partial-write contract:

* `SSL_MODE_ENABLE_PARTIAL_WRITE` and
  `SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER` are enabled.
* After `OK` with a short count, advance by `out_count` and call again.
* After `WANT_READ`/`WANT_WRITE`, do not advance.  Retry with the same requested
  length and same unwritten bytes.  The moving-buffer mode permits the pointer
  to change, but not the data or length.

`shutdown` is bidirectional: it sends our `close_notify`, then waits for the
peer's.  `OK` means both directions completed.  `WANT_READ` means our
`close_notify` was sent and the peer's is still owed; the caller may retry after
polling or abandon the stream.  `WANT_WRITE` retries the close-notify write.

Blocking and non-blocking fds are supported.  `EAGAIN`/`EWOULDBLOCK` and `EINTR`
are propagated as `WANT_READ`/`WANT_WRITE`, never as EOF.  The implementation
does not sleep or busy-poll; the caller owns `poll`/`select` policy.

### Errors

```c
typedef struct LangTlsError {
  int32_t category;          /* LangTlsErrorCategory */
  int32_t system_errno;      /* only for a real socket syscall failure */
  int32_t ssl_error;         /* SSL_get_error classification */
  int32_t reserved;          /* always 0 */
  int64_t verify_result;     /* X509_V_OK or X509_V_ERR_* */
  uint64_t openssl_error;    /* last OpenSSL error code */
  uint64_t openssl_error_count;
} LangTlsError;
```

The struct is frozen at 40 bytes with `verify_result` at offset 16 and
`openssl_error` at offset 24.  Categories:

| Category | Main use |
| --- | --- |
| `NONE` | success, retry, or clean EOF |
| `INVALID_ARGUMENT` | null/length/NUL/role/fd validation |
| `CONTEXT` | SSL_CTX/SSL setup, minimum version/mode, default trust setup |
| `CERTIFICATE` | CA/chain/key load or `SSL_get_verify_result() != X509_V_OK` |
| `HANDSHAKE` | TLS protocol/alert failure during connect/accept |
| `PROTOCOL` | TLS protocol failure during read/write |
| `IO` | socket syscall failure such as `EPIPE`/`ECONNRESET` |
| `UNEXPECTED_EOF` | TCP EOF/truncation before `close_notify` |
| `SHUTDOWN` | TLS failure during shutdown |
| `INTERNAL` | allocation or unsupported internal retry state |

An error is never represented only by `errno`.  `verify_result` carries
hostname mismatch (`62`), expired certificate (`10`), wrong trust chain, etc.
`openssl_error`/`openssl_error_count` carry OpenSSL's queue; `system_errno` is
secondary I/O context.  Each call clears OpenSSL's thread-local error queue at
entry and drains the errors it observes.

### Socket BIO and process signals

The SSL object uses a custom OpenSSL BIO whose read/write callbacks call
`recv(2)` and `send(2, ..., MSG_NOSIGNAL)`.  EAGAIN/EWOULDBLOCK/EINTR set the
correct BIO retry flags.  `BIO_set_shutdown(BIO_NOCLOSE)` and a no-op destroy
callback make the borrowed-fd contract explicit.

This prevents a peer reset from raising SIGPIPE without changing the process's
SIGPIPE disposition.  The implementation never calls `signal(SIGPIPE, SIG_IGN)`
or a process-wide OpenSSL cleanup.  One process-lifetime custom `BIO_METHOD` is
reused by all streams because `BIO_get_new_index()` has only 127 custom indices
per process; creating a method per stream would fail at stream 128.

## Validation

Run:

```sh
timeout 900 tests/native/tls/run_tls_tests.sh
```

The script uses the project LLVM 22.1.8 `clang++` (override with `CXX`), builds
and runs everything at both `-O0` and `-O2`, generates throwaway certificates
under `.build/certs` with the `openssl` CLI, and runs every child under
`timeout`.  `.build/` is ignored by git.  Certificates are test fixtures with no
real secret; no external network service is contacted.

Coverage:

* C11 compile of the public header.
* Exported-symbol check for all 13 `lang_tls_*_v1` functions.
* 4 MiB binary request/response containing NULs, clean TLS close, context and
  stream copy/release lifetime, and borrowed fd still valid after release.
* Non-blocking handshake/read/write with explicit `WANT_READ`/`WANT_WRITE` and
  `poll`; observed partial writes; no sleep-based loop.
* Blocking `recv` interrupted by SIGALRM with no `SA_RESTART`: returns
  `WANT_READ`, not EOF; data is received after retry.
* Peer reset with default SIGPIPE disposition: process survives and records an
  `IO` error.
* Validation of null pointers, `out_count`, embedded NUL, bracket/IP/DNS names,
  role mismatch, missing files, mismatched private key, and 300 repeated stream
  creates/releases (BIO index exhaustion regression).
* Independent Python `ssl` server: clean EOF and 64 KiB binary data; TCP
  truncation without `close_notify` must be `UNEXPECTED_EOF`; wrong hostname,
  wrong CA and expired certificate must be `CERTIFICATE` rejections; default
  trust via `SSL_CERT_FILE`; IPv4 and IPv6 SAN verification with no SNI; auto
  IP detection with no SNI; DNS SNI recorded as `localhost`.
* Independent `openssl s_client` verifies our server and exchanges
  `ping`/`pong` over loopback.
* Dynamic dependency and untouched-core checks.

### Latest recorded run

Environment: Linux x86_64, HEAD `148db60`, LLVM/Clang `22.1.8`, system
OpenSSL/Python OpenSSL `3.6.5`, Python `3.14.8`.

Representative actual output (identical PASS set at `-O0` and `-O2`):

```text
PASS header: tls.hpp compiles as C11 and LangTlsError is 40 bytes
=== build/run O0 ===
PASS exports O0: 13 lang_tls_*_v1 symbols
  NEEDED: libssl.so.3, libcrypto.so.3, libstdc++.so.6, libm.so.6,
          libgcc_s.so.1, libc.so.6
PASS self: 4 MiB binary both directions, clean close_notify, ARC and borrowed fd
PASS nonblocking: WANT_READ/WANT_WRITE + poll, partial write=yes
PASS EINTR: interrupted recv returned WANT_READ, not EOF
PASS SIGPIPE: default disposition survived peer reset; category=io errno=104 ssl_error=5 verify=0 openssl=2147483752 errors=1
PASS validation: null/NUL/length/role/path/context errors
PASS client-python clean EOF after 65536 binary bytes
PASS client-python truncation: category=unexpected-eof errno=0 ssl_error=1 verify=0 openssl=167772454 errors=1
PASS client-python certificate rejection: category=certificate errno=0 ssl_error=1 verify=62 openssl=167772294 errors=1
PASS client-python certificate rejection: category=certificate errno=0 ssl_error=1 verify=19 openssl=167772294 errors=1
PASS client-python certificate rejection: category=certificate errno=0 ssl_error=1 verify=10 openssl=167772294 errors=1
PASS client-python clean EOF after 65536 binary bytes   # default trust
PASS client-python clean EOF after 65536 binary bytes   # IPv4 SAN, SNI=null
PASS client-python clean EOF after 65536 binary bytes   # AUTO IP, SNI=null
PASS client-python clean EOF after 65536 binary bytes   # IPv6 SAN, SNI=null
PASS server-once: independent client exchanged ping/pong
PASS openssl-client-O0: independent openssl s_client verified our TLS server
=== build/run O2 ===
... same PASS set ...
PASS core: freestanding runtime and hosted start/thread have no tracked change
ALL PASS: hosted TLS ABI, real loopback TLS, Python ssl and openssl CLI
```

The `libssl.so.3` / `libcrypto.so.3` NEEDED entries prove the test executable
is an explicitly hosted link.  The freestanding core was not part of that link.
The script also checks that `runtime/native/` and the existing hosted
start/thread/sources files have no tracked change relative to `HEAD`; the
project-level `git status` is recorded for the integrator.  Full logs and
`summary.txt` are under `tests/native/tls/.build/`.

## What this does and does not establish

Established:

* the low-level `lang_tls_*_v1` C ABI behaves as described above;
* real TLS 1.2/1.3 loopback interoperability with Python `ssl` and OpenSSL
  `s_client`;
* certificate chain, validity, DNS/IP identity and SNI behavior;
* explicit non-blocking retry, partial writes, EINTR, truncation and SIGPIPE
  behavior;
* context/stream lifetime and borrowed-fd ownership.

Not established / not included:

* a Woozy source-level TLS type, `.wz` bindings, or `std::net` TLS API;
* HTTPS, HTTP framing, ALPN, proxies, redirects or URL parsing;
* client certificates, password-protected keys, CRL/OCSP policy, custom cipher
  policy or session-resumption policy beyond OpenSSL defaults;
* async I/O, cancellation, cross-thread concurrent use of one stream;
* Root's hosted compiler link mode, CMake integration or packaging changes;
* a claim that the freestanding runtime has TLS.

In short: **low-level hosted TLS passed; the Woozy-facing interface and HTTPS
remain future work.**

## Primary references

Checked against the installed OpenSSL 3.6.5 headers and the official OpenSSL
manual pages (the implementation uses only documented public APIs):

* [`SSL_CTX_new` / `TLS_client_method` / `TLS_server_method`](https://docs.openssl.org/3.0/man3/SSL_CTX_new.html)
* [`SSL_CTX_set_min_proto_version`](https://docs.openssl.org/3.0/man3/SSL_CTX_set_min_proto_version.html)
* [`SSL_CTX_set_verify`](https://docs.openssl.org/3.0/man3/SSL_CTX_set_verify.html)
* [`SSL_CTX_load_verify_locations`](https://docs.openssl.org/3.0/man3/SSL_CTX_load_verify_locations.html)
* [`SSL_CTX_set_default_verify_paths`](https://docs.openssl.org/3.0/man3/SSL_CTX_set_default_verify_paths.html)
* [`SSL_CTX_use_certificate`](https://docs.openssl.org/3.0/man3/SSL_CTX_use_certificate.html)
* [`SSL_set1_host` / `SSL_set_tlsext_host_name`](https://docs.openssl.org/3.0/man3/SSL_set1_host.html)
* [`X509_VERIFY_PARAM_set1_ip`](https://docs.openssl.org/3.0/man3/X509_VERIFY_PARAM_set1_ip.html)
* [`SSL_read_ex` / `SSL_write_ex`](https://docs.openssl.org/3.0/man3/SSL_read.html)
* [`SSL_get_error`](https://docs.openssl.org/3.0/man3/SSL_get_error.html)
* [`SSL_shutdown`](https://docs.openssl.org/3.0/man3/SSL_shutdown.html)
* [`SSL_CTX_set_mode`](https://docs.openssl.org/3.0/man3/SSL_CTX_set_mode.html)
* [`SSL_get_verify_result`](https://docs.openssl.org/3.0/man3/SSL_get_verify_result.html)
* [`BIO_meth_new` and custom BIOs](https://docs.openssl.org/3.0/man3/BIO_meth_new.html)
* [`ERR_get_error` / `ERR_peek_last_error`](https://docs.openssl.org/3.0/man3/ERR_get_error.html)
* [`SSL_free`](https://docs.openssl.org/3.0/man3/SSL_free.html) and
  [`SSL_CTX_free`](https://docs.openssl.org/3.0/man3/SSL_CTX_free.html)
