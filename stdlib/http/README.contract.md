# std/http contract

Status: candidate source-level module for integration. This file is the
worker-owned contract; the root integrator moves the final wording into
`development/docs`.

Scope: HTTP/1.1 request and response message syntax, framing, encoding, and
buffered reads over the existing `std/net.Socket` transport. TLS, HTTPS, URI
resolution, connection pools, redirects, authentication, and application
routing are outside this module.

The codec is written in Woozy and uses only the existing Native standard
library. It does not add intrinsics or compiler/runtime changes.

## Public data model

```woozy
pub struct Header {
    pub name string;      // token, ASCII
    pub value Vec[u8];    // field-value bytes (VCHAR / HTAB / SP / obs-text)
}
pub struct Request {
    pub method string;
    pub target string;
    pub version string;
    pub headers Vec[Header];
    pub body Vec[u8];
    pub trailers Vec[Header];
}
pub struct Response {
    pub version string;
    pub status i32;
    pub reason Vec[u8];   // reason-phrase bytes, not forced UTF-8
    pub headers Vec[Header];
    pub body Vec[u8];
    pub trailers Vec[Header];
}
pub struct Limits {
    pub head_bytes usize;
    pub body_bytes usize;
    pub header_count usize;
}
// One incremental decoded request-body piece. data is non-empty unless done.
// trailers is populated only on the final piece.
pub struct BodyPart {
    pub data Vec[u8];
    pub done bool;
    pub trailers Vec[Header];
}
pub struct Reader { /* private socket, pending bytes, parser state, limits */ }
```

Header names are ASCII tokens. Header values and response reason phrases are
raw bytes and are not decoded as UTF-8 by the parser, encoder, or `header`.
They may contain HTAB, SP, VCHAR and obs-text (`0x80..0xff`); NUL, CR, LF and
other controls are rejected. Bodies remain opaque `Vec[u8]`.

Initial header fields and chunked trailer fields are separate:

- `headers` contains only the initial header section.
- `trailers` contains only fields decoded from the chunked trailer section.
- `header`, `header_values`, `has_header` and the text lookup helpers inspect
  only `headers`. A trailer such as `Authorization` can never masquerade as an
  initial header.
- `parse_request`, `parse_response`, `read_request`, and `read_response`
  preserve this separation.
- `encode_request`/`encode_response` currently reject a non-empty `trailers`
  vector with `UnsupportedTrailers`. They never silently discard trailers.
  The chunked frame encoders below send trailers explicitly through
  `encode_response_chunk_end`.

## Text helpers

The wire codec is byte-oriented. Explicit helpers opt into UTF-8 text:

```woozy
pub func text_header(name string, value string) Header
pub func value_text(value Vec[u8]) string throws Error
pub func header_text(headers Vec[Header], name string) option.Option[string] throws Error
pub func header_text_values(headers Vec[Header], name string) Vec[string] throws Error
```

`text_header` converts UTF-8 source text to a byte-valued `Header`; parse and
encode still apply byte-level token/control validation. `value_text`,
`header_text`, and `header_text_values`
decode with `std/text.from_bytes`; invalid UTF-8 throws `Error.InvalidUtf8`.
Use the raw `Header` constructor when the application needs obs-text bytes.

## Limits and lookup helpers

```woozy
pub func default_limits() Limits
pub func limits(head_bytes usize, body_bytes usize, header_count usize) Limits
pub func reader(socket net.Socket, limits Limits) Reader
pub func pending(reader Reader) usize
pub func header(headers Vec[Header], name string) option.Option[Vec[u8]]
pub func header_values(headers Vec[Header], name string) Vec[Vec[u8]]
pub func has_header(headers Vec[Header], name string) bool
```

`default_limits()` is `8192` header-section bytes, `1048576` decoded body
bytes, and `100` headers. `MAX_CHUNK_LINE_BYTES` is 4096.

The public limits mean:

- `head_bytes`: maximum wire bytes from the start of a request/status line
  through the complete terminating `CRLF CRLF` of the initial header section.
  The same cap applies to the chunked trailer section, measured from the first
  trailer line through its terminating empty line. A section that reaches the
  cap without a terminator is rejected.
- `body_bytes`: maximum decoded body bytes. Chunk sizes, chunk extensions and
  trailers do not count as body bytes, but a chunk whose decoded size would
  push the body over the cap is rejected before its data is allocated or
  stored.
- `header_count`: maximum total header fields, including all chunked trailer
  fields. The start line is not a header field.

All decimal `Content-Length` and hexadecimal chunk-size values are parsed with
explicit overflow checks before any length-based allocation. When a
`Content-Length` or chunk size frames an actual message body, a value larger
than `body_bytes` is rejected as `BodyTooLarge`; arithmetic overflow is
rejected as `LengthOverflow`. A `Content-Length` on a HEAD or 304 response
describes the selected representation rather than a message body, so it is
validated syntactically but is not capped by `body_bytes`.

Header lookup is ASCII case-insensitive and preserves order and duplicates.
Values are returned as their original bytes.

## Complete-message parsing

```woozy
pub func parse_request(data Vec[u8]) Request throws Error
pub func parse_request_limited(data Vec[u8], limits Limits) Request throws Error
pub func parse_response(data Vec[u8], request_method string) Response throws Error
pub func parse_response_limited(data Vec[u8], request_method string, limits Limits) Response throws Error
```

The non-`limited` functions use `default_limits()`.

A complete-message parse accepts exactly one message. Bytes after a
length-delimited or chunked message are `TrailingBytes`. The one exception is
a response whose body is close-delimited (no `Transfer-Encoding` and no
`Content-Length`, and whose status/method permits a body): all remaining bytes
are the body. Callers that need pipelining use `Reader` instead.

Accepted versions are exactly `HTTP/1.0` and `HTTP/1.1`. Request lines require
`method SP request-target SP version CRLF`; methods are RFC tokens, and targets
must be non-empty and contain no SP, HTAB, or other control byte. Status lines
require `version SP 3DIGIT SP reason CRLF`; status must be 100 through 599.
Reason phrases are raw field-value bytes and are not decoded as UTF-8.

Header sections require strict CRLF, a legal token field name, and one colon.
Leading/trailing SP/HTAB around the value are removed. Interior SP/HTAB,
VCHAR and obs-text are retained. A line beginning with SP/HTAB (obs-fold), a
bare LF, a bare CR, NUL, DEL, or another control byte is rejected. Duplicate
header fields are never collapsed.

### Host validation

RFC 9112 section 3.2 Host validation is part of `parse_request`,
`read_request`, and `encode_request`:

- HTTP/1.1 requires exactly one `Host` field. Missing is `MissingHost`,
  duplicate is `DuplicateHost`, and malformed is `InvalidHost`.
- HTTP/1.0 may omit `Host`; when present, the same duplicate/malformed checks
  apply.
- Host uses [RFC 9110 section 7.2](https://www.rfc-editor.org/rfc/rfc9110.html#section-7.2)
  and [RFC 3986 section 3.2.2](https://www.rfc-editor.org/rfc/rfc3986.html#section-3.2.2)
  URI grammar: registered names retain percent-encoded bytes, unreserved and
  sub-delimiter characters; IP literals support IPv6 and IPvFuture.
- An empty Host field is retained for requests without a defined authority;
  it differs from a missing field. Decimal ports may be empty. The codec does
  not resolve DNS names or restrict a syntactically valid port to a socket range.
  A higher-level connection API must check its supported addresses and ports.
- Userinfo, bare IPv6, malformed percent escapes, nondecimal ports, malformed
  IP literals and extra authority syntax are rejected. Zone identifiers are
  not part of this HTTP Host grammar.

### Framing

Requests:

- `Transfer-Encoding` must consist of exactly the single coding `chunked`.
  Other transfer codings are rejected as `UnsupportedTransferEncoding`.
- `Transfer-Encoding` together with any `Content-Length` is rejected as
  `TransferEncodingWithContentLength`.
- Multiple `Content-Length` fields or comma-list values are accepted only when
  every value has the same decimal value.
- With neither header, a request has an empty body. Requests are never
  close-delimited.

Responses:

- `request_method == "HEAD"` means no message body, while framing headers may
  describe the corresponding GET representation.
- Status 1xx, 204, and 304 have no message body. Status 1xx is returned to the
  caller as a response; the caller may read the next response, except after
  `101 Switching Protocols`, where the connection is no longer HTTP.
- For 1xx and 204, `Content-Length` and `Transfer-Encoding` are rejected. For
  304 and HEAD responses they are allowed to describe the selected
  representation, but no body is read.
- `Transfer-Encoding` plus `Content-Length` is always rejected.
- Supported transfer coding is exactly `chunked`. Multiple codings, repeated
  chunked, and other codings are rejected.
- With no body framing and a body-permitting status, the response is
  close-delimited: `parse_response` consumes the rest of `data`, and
  `read_response` reads until EOF. This framing is inherently non-pipelined.
- A `Content-Length` body is read exactly. EOF before that count, a truncated
  chunk, or a truncated trailer section is `UnexpectedEof`.

Chunked decoding accepts RFC chunk extensions after `;`, preserving none of
them. Extension text is validated as bytes, so quoted obs-text is legal
without forcing the whole line through UTF-8. A chunk-size line, including
extensions, is limited to `MAX_CHUNK_LINE_BYTES` (4096) bytes. Decoding
requires the final zero-size chunk and its terminating empty trailer line.
Trailer field lines use the same byte validation as initial header lines.
Trailer fields whose names are `Content-Length`, `Transfer-Encoding`, `Host`,
`Connection`, `Trailer`, `TE`, `Upgrade`, `Keep-Alive`, or
`Proxy-Connection` are rejected as `ForbiddenTrailer`.

Trailers are returned in `Request.trailers` / `Response.trailers` and are not
appended to `headers`.

## Incremental head and body reads

```woozy
pub func read_request_head(reader Reader) Request throws Error
pub func read_request_body(reader Reader) Request throws Error
pub func read_request_body_part(reader Reader, max_bytes usize) BodyPart throws Error
pub func read_response_head(reader Reader, request_method string) Response throws Error
pub func read_response_body(reader Reader) Response throws Error
```

`read_request_head` and `read_response_head` parse only the start line and
initial header section. The returned value has populated headers, method,
target, version, status and reason, but empty body and trailers. The reader
remains in the body stage. `read_request_body`/`read_response_body` finish the
same message and return the complete value. `read_request`/`read_response`
also finish a head-only message if called after `read_*_head`.

A body read requires an in-progress head of the matching kind. Calling it with
no active message, after a complete message, while the other message kind is
active, or before the head has been parsed throws `ReadOrder`. Retrying a
WouldBlock body read after `poll` is safe and resumes the saved body state.

`read_response_head` uses `request_method` only for framing rules (HEAD and
status-specific body rules); `read_response_body` does not need it again.

### Incremental request body

`read_request_body_part(reader, max_bytes)` decodes the fixed-length or chunked
request body selected by `read_request_head` without buffering the whole body:

- `max_bytes` must be positive. Zero throws `ReadOrder` and leaves the reader
  unchanged.
- `max_bytes` is a hard upper bound on the decoded bytes in `BodyPart.data`
  for that call. Decoded bytes already buffered are returned immediately, even
  when fewer than `max_bytes`; the call does not wait to fill the bound. A
  receive is attempted only when no decoded bytes are currently available. A
  non-final piece never has empty data. A final piece may be empty when only
  the chunk terminator and trailers remain.
- `BodyPart.done == true` is the precise end of the message. It is set only
  after a fixed-length body has reached its exact length or after the chunked
  zero-size chunk and the complete trailer section have been consumed.
- `BodyPart.trailers` is empty unless `done` is true. On the final piece it
  contains only the decoded chunked trailer fields, never initial headers.
- The reader stores parsing progress and decoded bytes accumulated in the
  current call. `WouldBlock` during a header, chunk-size line, chunk data,
  chunk terminator, or trailer line can therefore be retried with the same
  bound after `poll` reports readability; already consumed bytes and the chunk
  phase/cursor are not lost or replayed.
- Changing the bound between calls is allowed. Any decoded bytes already held
  are returned at the new bound before more wire input is parsed.
- The cumulative `body_bytes`, trailer `head_bytes`, and total `header_count`
  limits are enforced across all calls, including multiple chunks. A chunk
  whose size would exceed the remaining body budget is rejected as
  `BodyTooLarge` before its data is stored.
- Streaming and whole-message reads cannot be mixed for one message. After a
  streaming call, `read_request_body`/`read_request` throw `ReadOrder`; after a
  whole-body read has started, `read_request_body_part` throws `ReadOrder`.
- After `done`, the reader is clear for the next pipelined message. Bytes of
  that message are never returned as body data.

The whole-body `read_request_body`/`read_request` APIs remain unchanged for
callers that want one complete `Request`. A caller that abandons a stream
before `done` must close the connection rather than parse the next message.

## Encoding

```woozy
pub func encode_request(request Request) Vec[u8] throws Error
pub func encode_response(response Response, request_method string) Vec[u8] throws Error
pub func encode_response_head(response Response, request_method string) Vec[u8] throws Error
```

`encode_response_head` validates the same fields and emits the status line,
headers and canonical `Content-Length`, but not the body. A caller that sends
exactly `response.body` bytes after this head avoids copying a large body into
one encoded buffer. `encode_response` is equivalent to
`encode_response_head` followed by appending the body.

Encoding validates all names, values, versions, methods, targets, statuses,
reason bytes and Host fields using the same rules as parsing. It emits strict
CRLF and preserves non-framing header order, duplicates and raw value bytes.

A non-empty request or response `trailers` vector is rejected as
`UnsupportedTrailers`; encoding never silently drops trailers.

For requests and ordinary body-permitting responses, the encoder removes all
incoming `Content-Length`/`Transfer-Encoding` fields, rejects
`Transfer-Encoding`, checks that any supplied `Content-Length` values agree
with `body.len()`, and emits exactly one canonical
`Content-Length: <body.len()>`. A missing body is encoded as length zero, never
as a close-delimited body.

For HEAD and 304 responses the body must be empty; `Transfer-Encoding` is
rejected, and a supplied `Content-Length` must be well formed and is emitted
once, because it describes the selected GET representation. For 1xx and 204
responses the body must be empty and framing headers are rejected, so the
encoder emits none. A non-empty body in any no-body context is
`UnexpectedBody`.

### Chunked response frames

```woozy
pub func encode_response_chunked_head(response Response, request_method string) Vec[u8] throws Error
pub func encode_response_chunk_data(data Vec[u8]) Vec[u8] throws Error
pub func encode_response_chunk_end(trailers Vec[Header]) Vec[u8] throws Error
```

These are frame-only encoders for a streamed HTTP/1.1 chunked response. The
caller sends exactly one head, zero or more non-empty data frames, and exactly
one end frame:

- `encode_response_chunked_head` validates the same reason, status, header
  names/values, method and status body policy as `encode_response_head`. It
  requires `HTTP/1.1`, a body-permitting request/status combination, and no
  supplied `Content-Length` or `Transfer-Encoding`; those cases throw
  `UnexpectedFraming`. A non-empty `response.body` is rejected as
  `UnexpectedBody` rather than silently dropped; send body bytes only through
  `encode_response_chunk_data`. A non-empty `response.trailers` is rejected as
  `UnsupportedTrailers`; pass trailers to the end encoder. The head emits one
  canonical `Transfer-Encoding: chunked`.
- `encode_response_chunk_data` emits one lowercase hexadecimal size line,
  data and CRLF. Empty data would encode a zero-size chunk and terminate the
  response prematurely, so it throws `InvalidChunkSize`. There is no
  zero-length data frame.
- `encode_response_chunk_end` emits the terminal `0` chunk followed by the
  supplied fields and the final empty line. Trailer names/values use the same
  token/field-byte validation as initial headers. Framing and
  connection-control names (`Content-Length`, `Transfer-Encoding`, `Host`,
  `Connection`, `Trailer`, `TE`, `Upgrade`, `Keep-Alive`,
  `Proxy-Connection`) throw `ForbiddenTrailer`.

The standard `encode_response`/`encode_response_head` remain
`Content-Length`-only and continue to reject trailers. HEAD, 1xx, 204 and 304
responses must use the length-framed head, not the chunked head.

## Socket reader and writer

```woozy
pub func read_request(reader Reader) Request throws Error
pub func read_response(reader Reader, request_method string) Response throws Error
pub func write_request(socket net.Socket, request Request) throws Error
pub func write_response(socket net.Socket, response Response, request_method string) throws Error
```

`reader()` wraps one `net.Socket`. Its private buffer is the pending/inbound
byte stream. Reads use bounded multi-byte `net.recv` calls, not one syscall per
byte. Header reads are capped by `head_bytes`. Body reads request only the
bytes still needed for a fixed-length body; chunked reads may read ahead, but
every extra byte remains in the reader buffer.

The reader keeps its in-progress parse state in the `Reader` value: selected
message kind, parsed start line and initial headers, framing length, chunk
phase/cursor, decoded body and trailers. Progress is copied into those
persistent containers before consumed wire bytes are compacted or discarded.
Therefore a nonblocking `Error.Io(os.Error.WouldBlock)` during header, fixed
length, chunked, or close-delimited reading is retryable: after `poll` reports
readability, call the same reader function again with the same message kind
and request method. Fixed-length, chunked and close-delimited retry cases are
covered by a real nonblocking socketpair plus `net.poll` test.

A protocol error leaves the reader in its in-progress state; callers should
close the connection. Mixing `read_request` and `read_response` while one
message is in progress is `ReaderBusy`. A clean EOF between messages is
`UnexpectedEof`; callers that treat connection close as an end condition must
stop before calling the reader again. The reader does not take ownership of
the socket and never closes it.

Chunk metadata is not accumulated without bound: each chunk-size line is
capped, consumed chunk metadata is compacted, and the trailer section is
capped by `head_bytes`. Retry resumes from the saved chunk cursor instead of
reparsing all previous bytes.

`read_request`/`read_response` parse one message from the front and retain all
bytes following it. A second call therefore observes pipelined messages
without loss. `pending(reader)` returns the retained byte count.

### Server retry, drain and finalize sequence

A streaming server follows this sequence for one request:

1. Call `read_request_head`. On `Error.Io(os.Error.WouldBlock)`, wait for
   readability (for example with `net.poll`) and call `read_request_head`
   again with the same reader. Do not create a new reader or drop buffered
   bytes.
2. Choose a fixed positive bound and repeatedly call
   `read_request_body_part(reader, bound)`. A returned piece may be shorter
   than the bound when only that much decoded input is available; consume its
   `data` before the next call. On `WouldBlock` (which occurs only when no
   decoded bytes are available), wait for readability and retry the same call
   with the same reader; the framing cursor/phase and pending state are
   retained. Do not call `read_request_body` or `read_request` for that
   message once streaming has started.
3. Stop only when `done` is true. Use `trailers` from that final piece. The
   reader now starts at the next pipelined request, so a later
   `read_request_head`/`read_request` observes it without loss. If the
   connection is closed or a protocol error occurs before `done`, close the
   connection; do not parse another request from that stream.
4. For a chunked response, send exactly one
   `encode_response_chunked_head`, one non-empty
   `encode_response_chunk_data` frame per piece, and exactly one
   `encode_response_chunk_end` with the final trailers. Use `net.send_all` for
   each frame. Never send an empty data frame as a terminator; only the end
   encoder emits the zero-size chunk.

All I/O failures are `Error.Io(os.Error)`. The reader retries
`os.Error.Interrupted`; `WouldBlock` is surfaced through `Error.Io` so
nonblocking callers can decide when to poll. `write_*` uses `net.send_all` and
surfaces short-write and OS errors as `Error.Io`. `read_request_head` and
`read_response_head` do not consume a body; a caller must finish it with
`read_request_body_part`, `read_request_body`, the combined read function, or
close the connection.

## Errors

```woozy
pub enum Error {
    Io(value os.Error);
    ReaderBusy;
    ReadOrder;
    UnexpectedEof;
    TrailingBytes;
    HeadTooLarge;
    BodyTooLarge;
    TooManyHeaders;
    LengthOverflow;
    InvalidRequestLine;
    InvalidMethod;
    InvalidTarget;
    InvalidStatusLine;
    InvalidStatus;
    InvalidReason;
    UnsupportedVersion;
    InvalidHeaderLine;
    InvalidHeaderName;
    InvalidHeaderValue;
    InvalidUtf8;
    ObsoleteLineFolding;
    InvalidContentLength;
    ConflictingContentLength;
    TransferEncodingWithContentLength;
    UnsupportedTransferEncoding;
    InvalidChunkSize;
    ChunkLineTooLong;
    InvalidChunkExtension;
    InvalidChunkTerminator;
    InvalidTrailer;
    ForbiddenTrailer;
    MissingHost;
    DuplicateHost;
    InvalidHost;
    UnexpectedBody;
    UnexpectedFraming;
    UnsupportedTrailers;
}
```

Every public operation that can fail throws only `http.Error`; I/O is wrapped
in `Io` rather than mixing `os.Error` and codec errors in one signature.

`ReadOrder` also covers misuse of the incremental API: a zero `max_bytes`, a
body-part call without an in-progress request head, and mixing
`read_request_body_part` with `read_request_body`/`read_request` on the same
message.

## Known integration notes

- TLS/HTTPS is intentionally absent. A TLS layer should decrypt bytes before
  `parse_*`/`Reader`, or expose the same byte stream to this module.
- The module does not implement `Expect: 100-continue`, connection reuse
  policy, proxy handling, content-codings, `CONNECT` tunnels, upgraded `101`
  byte streams, or HTTP/2. It returns `101` as a no-body response; the caller
  must stop HTTP parsing and switch protocols explicitly.
- Request-target validation here is lexical (non-empty, no SP/CTL). URI
  grammar, routing, and method semantics are application responsibilities; the
  codec preserves the target and all headers for that layer.
- Header values and reasons are bytes. Applications that expect UTF-8 text
  must use `value_text`, `header_text`, or `header_text_values` and handle
  `InvalidUtf8`.
