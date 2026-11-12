# std/tls — explicitly selected hosted TLS

`std/tls` is the Woozy source API over the accepted `lang-tls/1` C ABI and
OpenSSL 3. It is available only to programs that explicitly select the hosted
runtime and declare the `woozy_tls` native dependency. A freestanding program
does not import this module and does not link `libssl`, `libcrypto` or
libstdc++ implicitly.

```toml
[build.native]
runtime = "hosted"

[dependencies.woozy_tls]
kind = "native"
binding = "<repo>/stdlib/tls/bindings/tls.json"
libraries = ["ssl", "crypto", "stdc++"]
library_dirs = ["<directory containing libwoozy_tls.a>"]
```

The addon archive and development build steps are documented in
[`addon/README.md`](addon/README.md). The public API below is the language
surface; it does not expose raw OpenSSL objects.

## Public types

```wz
pub struct Context { /* opaque resource */ }
pub struct Stream {
    /* opaque resource; the socket descriptor is borrowed */
    pub descriptor i32;
}

pub enum NameKind { Auto; Dns; Ip; }
pub enum Status { Ok; WantRead; WantWrite; Eof; }

pub struct Transfer {
    pub status Status;
    pub count usize;
}

pub struct ErrorInfo {
    pub category i32;
    pub system_errno i32;
    pub ssl_error i32;
    pub verify_result i64;
    pub openssl_error u64;
    pub openssl_error_count u64;
}

pub enum Error {
    InvalidArgument(info ErrorInfo);
    Context(info ErrorInfo);
    Certificate(info ErrorInfo);
    Handshake(info ErrorInfo);
    Protocol(info ErrorInfo);
    Io(info ErrorInfo);
    UnexpectedEof(info ErrorInfo);
    ShutdownFailed(info ErrorInfo);
    Internal(info ErrorInfo);
    Wait(error os.Error);
    NoProgress(completed usize);
}
```

`Context` and `Stream` are C resources. A normal Woozy copy shares the same
underlying object and retains it; the last copy releases it. The `Stream`
borrows its socket descriptor exactly like `lang-tls/1`: releasing a stream
does **not** close the socket. The caller owns the socket and must close it
once, after it is finished with the stream.

`token(stream)` returns a checked borrowed ID for transport callbacks. Keep a
Stream owner alive for the whole transport lifetime. Unknown or expired IDs
produce a typed `os.Error` from `transport_read_token`/`transport_write_token`;
IDs are never interpreted as addresses or recycled. A temporary reference
protects each call's lifetime, but does not make concurrent SSL reads/writes
safe. Synchronize access to a shared stream.

## Contexts

```wz
func client_default() Context throws Error
func client_with_ca(ca_file string) Context throws Error
func server(certificate_chain_file string, private_key_file string) Context throws Error
```

* `client_default` uses OpenSSL's platform default trust paths.
* `client_with_ca` uses exactly the supplied PEM CA file.
* `server` loads a PEM certificate chain and a matching PEM private key. Client
  certificates are not requested.

Client contexts always verify the certificate chain and validity. There is no
insecure/skip-verification entry point. Host names must be non-empty, at most
255 bytes, and contain no embedded NUL. File paths must be non-empty and contain
no embedded NUL.

Both roles require TLS 1.2 or newer. Cipher suites, groups and certificate
algorithms remain OpenSSL policy.

## Streams and identity

```wz
func client(context Context, socket net.Socket, name string, kind NameKind) Stream throws Error
func server_stream(context Context, socket net.Socket) Stream throws Error
func connect(context Context, socket net.Socket, name string, kind NameKind) Stream throws Error
```

`client`/`server_stream` wrap the borrowed socket descriptor without performing
a handshake. `connect` creates a client stream and drives the handshake to
completion, using `net.poll` for retry waits.

Identity behavior follows the verified low-level ABI:

* `NameKind.Auto` detects a syntactic IPv4/IPv6 literal and uses IP
  verification; otherwise it uses DNS verification and SNI.
* `NameKind.Dns` uses `SSL_set1_host` and sends SNI.
* `NameKind.Ip` uses IP address verification and never sends SNI.
* Bracketed URL authority syntax such as `[::1]` is rejected. No IDNA
  conversion or URL parsing is performed.

## Progress and I/O

```wz
func handshake(stream Stream) Status throws Error
func handshake_all(stream Stream) throws Error
func wait(stream Stream, current Status) throws Error

func read(stream Stream, data Vec[u8], capacity usize) Transfer throws Error
func write(stream Stream, data Vec[u8], offset usize, length usize) Transfer throws Error
func write_all(stream Stream, data Vec[u8]) throws Error

func shutdown(stream Stream) Status throws Error
func close(stream Stream) throws Error
```

The progress API distinguishes `Ok`, `WantRead`, `WantWrite` and `Eof` without
sleeping or busy-looping. After `WantRead`/`WantWrite`, call `wait` (which polls
the stream descriptor) and retry the same operation. On `Ok`, `count` may be a
partial byte count; advance `offset` and continue. On a retry wait, do not
advance: the API retries with the same unwritten bytes and length.

`read` returns `Eof` only for a clean TLS `close_notify`. A TCP EOF or
truncation without `close_notify` is an `UnexpectedEof` error. `shutdown` sends
our `close_notify`; `close` waits for the peer's close and returns after a clean
bidirectional close. `write_all` and `connect` are convenience helpers over the
same progress operations.

Zero-length reads and writes are successful no-ops with count 0. Binary data,
including embedded NUL bytes, is handled as bytes; only text arguments
(host name and file paths) reject NUL.

## Errors

Errors expose the frozen low-level categories. `Certificate` includes chain,
hostname, validity and CA failures; `verify_result` carries the OpenSSL
`X509_V_ERR_*` value. `UnexpectedEof` is distinct from `Io`. Socket writes use
the low-level `MSG_NOSIGNAL` BIO, so a broken peer does not raise SIGPIPE and the
process-wide SIGPIPE disposition is not changed.

`Error.Wait` is used only by the convenience polling path when `net.poll`
itself fails. `Error.NoProgress` is used by convenience loops if a write makes
no progress.

## HTTP

Use [`std/https`](../https/README.md) for HTTP/1.1 over this stream. `std/http`
also exposes `reader_external` for the same purpose; it is not a second TLS API.
