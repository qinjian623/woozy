# std/http_server

Async HTTP/1.1 server built on `std/http`, `std/net`, and the native executor.
This module is Linux x86_64 Native only; the Python backend rejects async
functions before this module can run. TLS is not part of this module.

## Public model

```wz
pub struct Config {
    pub limits http.Limits;
    pub backlog i32;
    pub max_connections usize;      // 0 = unlimited, default 1024
    pub max_requests usize;         // per connection, 0 = unlimited
    pub keep_alive bool;
    pub tcp_nodelay bool;
    pub reuse_address bool;
    pub header_timeout_ms i64;      // 0 disables, default 10000
    pub body_timeout_ms i64;        // 0 disables, default 30000
    pub idle_timeout_ms i64;        // 0 disables, default 60000
    pub write_timeout_ms i64;       // 0 disables, default 30000
    pub drain_timeout_ms i64;       // 0 waits forever, default 10000
    pub timer_tick_ms i64;          // default 100
    pub large_body_threshold usize; // default 16384
}
pub struct Stats {
    pub active_connections usize;
    pub accepted_connections u64;
    pub requests u64;
    pub bytes_sent u64;
    pub errors u64;
    pub timeouts u64;
    pub rejected u64;
    pub stopping bool;
}
pub struct Server { /* private listener, handler, settings, state */ }

pub func default_config() Config
pub func listen(address net.Address,
                handler func(http.Request) http.Response throws http.Error)
                Server throws os.Error
pub func listen_with(address net.Address,
                     handler func(http.Request) http.Response throws http.Error,
                     config Config) Server throws os.Error
pub func local_address(server Server) net.Address throws os.Error
pub func active_connections(server Server) usize
pub func accepted_connections(server Server) u64
pub func stats(server Server) Stats
pub func config(server Server) Config
pub async func serve(server Server) Unit throws os.Error
pub func stop(server Server)
pub func abort(server Server)
pub func close(server Server)
pub async func drain(server Server, timeout_ms i64) bool
pub func text_response(status i32, body string) http.Response
pub func bytes_response(status i32, body Vec[u8]) http.Response
```

`listen_with` creates a nonblocking, close-on-exec TCP listener with
`SO_REUSEADDR` by default. Server state, counters and the connection table are
not synchronized; use a `Server` only from the executor thread that owns it.
`Server` copies share the listener and counters; `stop`/`abort`/`close` on any
copy affects all copies. Call `serve` once per `Server`.

The handler is a non-capturing or capturing function value. It runs
synchronously in the connection task; a thrown `http.Error` produces a 500
response and closes that connection. Applications with their own error type
should catch it in the handler and return a response. `text_response` and
`bytes_response` build a response with the canonical reason phrase.

## Lifecycle

- `serve` accepts until stopped, aborted, or cancelled. Each accepted
  connection becomes one task. Dropping the returned Task handle does not stop
  the server. Returning (including cancellation) closes the listener and
  starts graceful drain.
- `stop` closes the listener, shuts down idle keep-alive connections
  immediately, stops starting new requests, and lets in-flight requests finish
  until `drain_timeout_ms`. The watchdog aborts anything still active at that
  deadline.
- `abort`/`close` close the listener and every active connection immediately.
- `drain(server, timeout_ms)` calls `stop` and waits until no connection task
  is active. `timeout_ms <= 0` waits until idle. It returns false only when a
  positive timeout expires first. Do not call it from a connection task.
- The watchdog only calls `shutdown(2)` on the owning descriptor; the
  connection task always remains the sole closer.
- `stats` is a single-threaded snapshot. `requests` counts handler responses
  that were fully written, `bytes_sent` counts those response bytes plus any
  100 Continue bytes, `errors` counts protocol, expectation, handler, framing
  and write failures, `timeouts` counts watchdog expirations, and `rejected`
  counts 503 capacity rejections. Protocol rejection responses (400/413/417/
  431/501/505) and 408 responses are not included in `requests` or
  `bytes_sent`.

## Wire behavior

- Request and response framing is the `std/http` codec. Requests may be
  fixed-length or chunked; initial headers and chunk trailers stay separate.
- Responses always use `Content-Length`. A handler `Transfer-Encoding`,
  non-empty trailers, or an invalid status is rejected by the codec and
  becomes a 500 response.
- `Expect: 100-continue` is answered before the body is read when the request
  has a body. An unsupported expectation receives 417 and the connection
  closes. `Expect` on a request without a body is ignored.
- The server removes every handler `Connection` field and emits its own
  `keep-alive` or `close` decision. Handler `Connection: close` forces close.
- HTTP/1.1 defaults to keep-alive. HTTP/1.0 defaults to close unless the
  request contains the `keep-alive` token. Connection tokens are compared
  case-insensitively and may appear in comma-separated values.
- HEAD, 1xx, 204, and 304 responses never carry a body. For HEAD and 304, a
  missing `Content-Length` is filled from the handler body, which describes the
  selected representation.
- For ordinary responses, a handler `Content-Length` is ignored and replaced
  with the actual body length.
- The response version follows the request version (`HTTP/1.0` or
  `HTTP/1.1`).
- A protocol error is answered with a small `Connection: close` response:
  400 for malformed messages, 408 for header/body/drain timeouts, 413 for an
  oversized body, 417 for an unsupported expectation, 431 for oversized
  headers or too many fields, 501 for an unsupported transfer coding, and 505
  for an unsupported version. EOF and I/O failures close without a response.
- `max_connections` is checked after `accept`; excess connections receive a
  503 `Connection: close` response and are closed.
- `max_requests` closes a connection after the last allowed response.

## Timeouts

A single watchdog task scans the connection table every `timer_tick_ms`. A
connection records its current phase (`idle`, `header`, `body`, `write`) and an
absolute monotonic deadline. Expiry shuts down the read side for a request
timeout (so a 408 can still be written) or both directions for a write
timeout. Retrying a WouldBlock operation keeps the same deadline.

- `header_timeout_ms` starts when a request head begins; it also bounds the
  first byte on a fresh or idle connection.
- `body_timeout_ms` starts when the head is complete and a body is expected.
- `idle_timeout_ms` applies only between requests on a keep-alive connection.
- `write_timeout_ms` starts when a response write begins.
- `drain_timeout_ms` is an overall graceful-stop deadline; it never extends a
  shorter per-operation deadline.

These are absolute deadlines, not inactivity reset timers. A slowloris that
delivers one byte slowly still hits the header deadline. Timeouts use
`CLOCK_MONOTONIC`; if the clock call fails, the deadline is disabled rather
than firing immediately.

## Performance

Reads and writes use nonblocking descriptors; `WouldBlock` parks the
connection task on the executor's wait instead of blocking a thread. Many
connections and large slow responses therefore do not block accept or other
connections. After 32 accepts and after every 1024 kept-alive responses, a
task takes a 1 ms timer turn so one busy listener or pipeline cannot starve
ready descriptors.

Responses larger than `large_body_threshold` send `encode_response_head` and
the handler body separately, avoiding one full-body copy. Smaller responses
use one encoded buffer and one send. The first send uses the whole buffer;
partial writes resume at the exact offset with slices capped at 1 MiB.

Handlers run on the executor thread. Long CPU work, blocking file I/O, or a
blocking C call in a handler stalls every connection. Use a worker pool or
split the work before calling into an HTTP handler.

## Not provided

- TLS/HTTPS. Terminate TLS in a reverse proxy or extend the transport.
- HTTP/2, WebSocket/`Upgrade` protocol switching (101 is returned, then the
  connection closes).
- Streaming/chunked responses, response compression, `Date`/`Server` headers.
- Request routing, authentication, cookies, or content decoding. These belong
  to the application layer.
- Multi-core execution; the language executor is single-threaded.

## Example

```wz
package app;
import "std/http" as http;
import "std/http_server" as server;
import "std/io" as io;
import "std/net" as net;
import "std/os" as os;
import "std/strconv" as strconv;

func route(request http.Request) http.Response throws http.Error {
    return server.text_response(200,"hello");
}

async func main() i32 throws os.Error {
    let listener = server.listen(net.any4(8080), route);
    let address = server.local_address(listener);
    io.println("listening on "+strconv.format_i64(i64(address.port)));
    await server.serve(listener);
    return 0;
}
```
