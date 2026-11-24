# std/http_server

Async HTTP/1.1 server built on `std/http`, `std/net`, and the native executor.
This module supports native Linux x86_64 and macOS arm64 programs. The
Python backend rejects async functions. TLS is not part of this module.

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
// Async handler execution deadline. Non-positive disables it.
pub struct AsyncSettings {
    pub handler_timeout_ms i64;
}
// Stream handler clocks. Non-positive disables the corresponding clock.
pub struct StreamSettings {
    pub read_timeout_ms i64;
    pub processing_timeout_ms i64;
    pub write_timeout_ms i64;
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
pub struct Server { /* private listener, handlers, settings, state */ }
pub struct Stream { /* private per-request state */ }

pub func default_config() Config
pub func default_async_settings() AsyncSettings
pub func default_stream_settings() StreamSettings
pub func listen(address net.Address,
                handler func(http.Request) http.Response throws http.Error)
                Server throws os.Error
pub func listen_with(address net.Address,
                     handler func(http.Request) http.Response throws http.Error,
                     config Config) Server throws os.Error
pub func listen_async(address net.Address,
                      handler func(http.Request) Task[http.Response,http.Error])
                      Server throws os.Error
pub func listen_async_with(address net.Address,
                           handler func(http.Request) Task[http.Response,http.Error],
                           config Config) Server throws os.Error
pub func listen_async_with_settings(
    address net.Address,
    handler func(http.Request) Task[http.Response,http.Error],
    config Config,
    settings AsyncSettings) Server throws os.Error
pub func listen_stream(address net.Address,
                       handler func(http.Request, Stream) Task[Unit,http.Error])
                       Server throws os.Error
pub func listen_stream_with(address net.Address,
                            handler func(http.Request, Stream) Task[Unit,http.Error],
                            config Config) Server throws os.Error
pub func listen_stream_with_settings(
    address net.Address,
    handler func(http.Request, Stream) Task[Unit,http.Error],
    config Config,
    settings StreamSettings) Server throws os.Error
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
pub async func read(stream Stream, max_bytes usize) http.BodyPart throws http.Error
pub async func start(stream Stream, response http.Response) Unit throws http.Error
pub async func start_chunked(stream Stream, response http.Response) Unit throws http.Error
pub async func write_chunk(stream Stream, data Vec[u8]) Unit throws http.Error
pub async func finish(stream Stream, trailers Vec[http.Header]) Unit throws http.Error
```

`listen`/`listen_with`, `listen_async`/`listen_async_with`, and
`listen_stream`/`listen_stream_with` create the same nonblocking,
close-on-exec TCP listener with `SO_REUSEADDR` by default and share the
connection, framing, timeout, stats and lifecycle paths. Only the handler kind
differs. Server state, counters and the connection table are not synchronized;
use a `Server` only from the executor thread that owns it. `Server` copies share
the listener and counters; `stop`/`abort`/`close` on any copy affects all
copies. Call `serve` once per `Server`.

A synchronous handler is a non-capturing or capturing function value. It runs
synchronously in the connection task; a thrown `http.Error` produces a 500
response and closes that connection. Applications with their own error type
should catch it in the handler and return a response. `text_response` and
`bytes_response` build a response with the canonical reason phrase.

An asynchronous handler is a function value that returns
`Task[http.Response,http.Error]`. The function body that *builds* the task runs
on the connection task; the connection task then awaits it before reading the
next request from that connection. Because async functions cannot be used as
function values, return a task from an ordinary function (see the example). A
thrown or returned `http.Error` produces the same 500 response and connection
close as the synchronous handler. Async handler execution starts only after the
request body has been fully decoded, and uses its own `AsyncSettings`
deadline from that point. It does not inherit the remaining body deadline.
`listen_async_with` uses the default 30 s handler deadline;
`listen_async_with_settings` supplies an independent value in a settings value
built from `default_async_settings()` by assigning the named
`handler_timeout_ms` field. At most one request is active per connection:
pipelined requests wait for the active handler. Other connections continue to
run while the handler is suspended.

A streaming handler is `func(http.Request, Stream) Task[Unit,http.Error]`.
It is called after the request head and `Expect` processing, before the body is
read. The `Request` contains the method, target, version and initial headers;
its body and trailers are empty. The handler reads decoded body pieces with
`read(stream, max_bytes)` and writes either one fixed response with
`start(stream, response)` or a chunked response with
`start_chunked`/`write_chunk`/`finish`. `read` returns an `http.BodyPart`
(`data`, `done`, `trailers`); a non-final piece may be shorter than
`max_bytes` when only that much decoded input is available, and trailers are
present only on the final piece. Retrying after `WouldBlock` resumes the same
reader and chunk cursor. A `Stream` copy shares the per-request state. After
the handler Task returns, that state is invalidated and retained copies throw
`http.Error.ReadOrder`.
Stream handlers have three independent clocks from `StreamSettings`: `read`
covers one body-part wait, `processing` covers handler work between operations,
and `write` covers one call that sends a response. A completed read or write starts a fresh
processing clock, so a slow body read is not charged to processing and a pause
between response chunks is not charged to the write clock. `listen_stream_with`
keeps `read` tied to `Config.body_timeout_ms`, `write` tied to
`Config.write_timeout_ms`, and uses the default 30 s processing clock;
`listen_stream_with_settings` sets all three fields explicitly. Start from
`default_stream_settings()` and assign the named mutable fields; no positional
settings factory is provided. `Config` keeps its existing layout.

```wz
var settings = default_stream_settings();
settings.read_timeout_ms = 5000;
settings.processing_timeout_ms = 10000;
settings.write_timeout_ms = 5000;
let listener = listen_stream_with_settings(address, handler, config, settings);
```

The stream handler no longer keeps a body deadline while writing a response or
doing user work. Only one write operation may be in flight; duplicate start,
duplicate finish, empty chunks, and writes after `finish` are rejected.
`finish` with trailers is required exactly once for a chunked response; after a
fixed `start`, `finish` is a no-op when `trailers` is empty, and non-empty
trailers are rejected.

## Lifecycle

- `serve` accepts until stopped, aborted, or cancelled. Each accepted
  connection becomes one task. Dropping the returned Task handle does not stop
  the server. When `serve` returns (including on cancellation), it closes the
  listener and starts a graceful drain.
- `stop` closes the listener, shuts down idle keep-alive connections
  immediately, stops starting new requests, and lets in-flight requests finish
  until `drain_timeout_ms`. The watchdog aborts anything still active at that
  deadline.
- `abort`/`close` close the listener and every active connection immediately;
  an active async/stream handler task is cancelled as part of the abort.
- `drain(server, timeout_ms)` calls `stop` and waits until no connection task
  is active. `timeout_ms <= 0` waits until idle. It returns false only when a
  positive timeout expires first. Do not call it from a connection task.
- The watchdog calls `shutdown(2)` on the connection's socket descriptor and
  cancels an active async/stream handler task when that connection's deadline
  expires. The connection task always remains the sole closer. A stream
  connection keeps a request active until its handler returns; an early
  response with an unconsumed request body is followed by a close, never by
  parsing the next pipelined request.
- Cancelling a handler runs its `defer`/owned-value cleanup at the next async
  suspension point. That caller-owned cleanup is scheduled separately; the
  connection cancel/close path does not run it inline. No response is written
  for a cancelled handler and `requests` is not incremented. `timeouts` is
  incremented only when the watchdog expires a deadline; an explicit `abort`
  cancellation does not increment it. Task cancellation is cooperative and not
  a structured task tree: a handler that starts additional tasks must cancel
  those tasks in its own `defer`; cancellation of the registered handler task
  does not reach independent tasks it spawned.
- A suspended stream read/write retains the executor descriptor wait. When the
  connection task unwinds with that operation still in flight on a deadline,
  drain, or abort cancellation, it cancels the wait synchronously while it
  still owns the socket, so `close(2)` cannot orphan the epoll registration;
  then it shuts the socket down and closes it. The aliased operation and its
  `defer` resume on a separately scheduled turn, not inside the cancel or
  unregister call, so that cleanup may run after the socket is closed. A
  normal handler return with an alias still pending first shuts the socket
  down and waits for the operation to leave its read/write section before
  closing. The server never joins or adopts caller-spawned tasks.
- `stats` is a single-threaded snapshot. `requests` counts handler responses
  that were fully written (including a fixed `start` and a chunked response
  after its single `finish`); generated failure responses never increment it.
  `bytes_sent` counts completed handler response bytes, generated stream
  failure status bytes, and any 100 Continue bytes. `errors` counts one event
  per protocol, expectation, handler, framing, or write failure. `timeouts`
  counts watchdog expirations, so a timeout increments `timeouts` instead of
  `errors`. `rejected` counts 503 capacity rejections. An incomplete chunked
  response, a cancelled handler, and a response that fails during a write are
  not counted by `requests`; the partial bytes of an incomplete stream
  response are not added to `bytes_sent`. One-shot protocol rejection
  responses sent before a stream handler runs (for example 400/408/417/431/
  501) are not included in `bytes_sent`; stream failures converted to a status
  response by the handler path (including 413/500/505) are included.

## Wire behavior

- Request and response framing is the `std/http` codec. Requests may be
  fixed-length or chunked; initial headers and chunk trailers stay separate.
- Legacy and async handlers always use `Content-Length`. A handler
  `Transfer-Encoding`, non-empty trailers, or an invalid status is rejected by
  the codec and becomes a 500 response. Stream handlers may use the fixed
  `start` path or the HTTP/1.1 chunked path.
- Chunked stream responses emit one chunked head, zero or more non-empty data
  chunks, and exactly one terminal chunk with optional trailers. Handler
  `Content-Length`/`Transfer-Encoding` headers, non-empty `response.trailers`
  on `start_chunked`, and empty `write_chunk` data are rejected before commit.
- `Expect: 100-continue` is answered before the body is read when the request
  has a body. An unsupported expectation receives 417 and the connection
  closes. `Expect` on a request without a body is ignored.
- The server removes every handler `Connection` field and emits its own
  `keep-alive` or `close` decision. Handler `Connection: close` forces close
  for both `start` and `start_chunked`. A chunked response with an explicit
  close sends the close header and does not parse a pipelined followup.
- HTTP/1.1 defaults to keep-alive. HTTP/1.0 defaults to close unless the
  request contains the `keep-alive` token. Connection tokens are compared
  case-insensitively and may appear in comma-separated values.
- HEAD, 1xx, 204, and 304 responses never carry a body. For HEAD and 304, a
  missing `Content-Length` is filled from the handler body's length, which
  describes the selected representation. Use the fixed `start` path for these
  responses; `start_chunked` rejects them before commit.
- For ordinary responses, a handler `Content-Length` is ignored and replaced
  with the actual body length.
- The response version follows the request version (`HTTP/1.0` or
  `HTTP/1.1`). Chunked responses require HTTP/1.1; `start_chunked` on an
  HTTP/1.0 request fails before commit and the server sends a 505 response.
- A fixed stream response commits the whole response before any write. An
  error before commit becomes one status response (protocol read failures such
  as `BodyTooLarge` map to their status, otherwise 500). After any response
  bytes are committed, an error closes the connection without appending a
  second HTTP response.
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
connection records its current phase (`idle`, `header`, `body`, `write`,
`handler`, `stream-read`, `stream-process`) and an absolute monotonic deadline.
Expiry shuts down the read side for a read/handler timeout or both directions
for a write timeout. Retrying a WouldBlock operation keeps the same operation
deadline.

- `header_timeout_ms` starts when a request head begins; it also bounds the
  first byte on a fresh or idle connection.
- `body_timeout_ms` starts when the head is complete and a body is expected.
  For legacy synchronous handlers it remains the body deadline. For a
  non-streaming async handler the body deadline ends when the complete request
  body has been decoded; `AsyncSettings.handler_timeout_ms` then starts a
  dedicated handler deadline. For stream handlers the same setting supplies
  the per-read clock unless `listen_stream_with_settings` overrides it.
  A non-positive value disables the corresponding clock.
- `AsyncSettings.handler_timeout_ms` starts when async handler execution
  begins after request parsing/body decode. Expiry cancels the handler task;
  it does not increment `errors`.
- `StreamSettings.read_timeout_ms` starts at each `read` operation (including
  its WouldBlock retries) and covers waiting for decoded request pieces.
- `StreamSettings.processing_timeout_ms` starts at handler start and is
  restarted whenever a read or response-write operation completes. It covers
  user computation between operations.
- `StreamSettings.write_timeout_ms` covers each public write call. A large
  fixed response shares one deadline across its head and body. Each chunk
  and the terminal chunk have separate deadlines.
- `idle_timeout_ms` applies only between requests on a keep-alive connection.
- `write_timeout_ms` starts when a legacy/async response write begins. For
  stream responses, use `StreamSettings.write_timeout_ms`; a client that stops
  reading one frame cancels the handler instead of consuming the processing
  clock.
- `drain_timeout_ms` is an overall graceful-stop deadline; it never extends a
  shorter per-operation deadline. At that deadline the watchdog cancels an
  active asynchronous handler, which then runs its cleanup.

Header, legacy body, write, and drain deadlines are absolute. Stream reads and
writes each start a new deadline, and WouldBlock retries do not extend it. A
processing deadline starts at handler entry and restarts when a stream read or
write ends. The total duration of a stream can exceed any one of those
limits. Keep a body size limit, and use a monotonic deadline in the handler
if the complete stream needs a time limit. Timeouts use
`CLOCK_MONOTONIC`; if the clock call fails, the deadline is disabled rather
than firing immediately.

## Performance

Reads and writes use nonblocking descriptors; `WouldBlock` parks the
connection task on the executor's I/O wait instead of blocking a thread. Many
connections and large slow responses therefore do not block accept or other
connections. After 32 accepts and after every 1024 keep-alive responses, a
task takes a 1 ms timer turn so one busy listener or pipeline cannot starve
ready descriptors.

Responses larger than `large_body_threshold` send `encode_response_head` and
the handler body separately, avoiding one full-body copy. Smaller responses
use one encoded buffer and one send. The first send uses the whole buffer;
partial writes resume at the exact offset with slices capped at 1 MiB.

Handlers run on the executor thread. Long CPU work, blocking file I/O, or a
blocking C call in a handler stalls every connection. Async and stream handlers
help only when they suspend on an executor await (`sleep`, descriptor waits,
channels, or another async task); a CPU-bound task still blocks the thread. Use
a worker pool or split that work before calling into an HTTP handler.

Streaming request bodies are not buffered as a whole. Decoded pieces are
bounded by the handler's `read` argument and by `Config.limits.body_bytes`;
`WouldBlock` parks the task and preserves the codec cursor. Chunked responses
send one encoded frame per `write_chunk`, so a slow client applies backpressure
to that connection while other connections keep running.

## Not provided

- TLS/HTTPS. Terminate TLS in a reverse proxy or extend the transport.
- HTTP/2, WebSocket/`Upgrade` protocol switching (101 is returned, then the
  connection closes).
- Response compression and `Date`/`Server` headers.
- Streaming over HTTP/1.0 (use fixed responses) and streamed 1xx/204/304/HEAD
  bodies (use the fixed `start` path).
- Request routing, authentication, cookies, or content decoding. These belong
  to the application layer.
- Multi-core execution; the language executor is single-threaded.

## Example

```wz
package app;
import "std/async" as tasks;
import "std/http" as http;
import "std/http_server" as server;
import "std/io" as io;
import "std/net" as net;
import "std/os" as os;
import "std/strconv" as strconv;
import "std/vec" as vec;

func route(request http.Request) http.Response throws http.Error {
    return server.text_response(200,"hello");
}

async func delayed_route(request http.Request) http.Response throws http.Error {
    await tasks.sleep(50);
    return server.text_response(200,"hello later");
}

// Async function values are not allowed, so an ordinary wrapper returns
// the Task that listen_async will await on the connection task.
func route_task(request http.Request) Task[http.Response,http.Error] {
    return delayed_route(request);
}

async func main() i32 throws os.Error {
    let listener = server.listen_async(net.any4(8080), route_task);
    let address = server.local_address(listener);
    io.println("listening on "+strconv.format_i64(i64(address.port)));
    await server.serve(listener);
    return 0;
}
```

Use `listen`/`listen_with` with `route` when the handler does not need to
await. The async registration does not change request parsing, response
framing, keep-alive decisions, protocol errors, stats, or timeout values.

A streaming handler reads request pieces and emits chunked response pieces:

```wz
func stream_route(
    request http.Request,
    stream server.Stream) Task[Unit,http.Error] {
    return echo_stream(request,stream);
}

async func echo_stream(
    request http.Request,
    stream server.Stream) Unit throws http.Error {
    let head = http.Response(
        version:"HTTP/1.1",
        status:200,
        reason:vec.empty[u8](),
        headers:[http.text_header("X-Stream","echo")],
        body:vec.empty[u8](),
        trailers:vec.empty[http.Header]());
    await server.start_chunked(stream,head);
    for true {
        let part = await server.read(stream,16384);
        if vec.len(part.data) != 0 {
            await server.write_chunk(stream,part.data);
        }
        if part.done {
            await server.finish(stream,vec.empty[http.Header]());
            return;
        }
    }
}

async func main() i32 throws os.Error {
    let listener = server.listen_stream(net.any4(8080), stream_route);
    await server.serve(listener);
    return 0;
}
```

For a response with a known complete body, replace `start_chunked` with
`start`; it sends the fixed `Content-Length` response and needs no
`write_chunk`/`finish` calls.
