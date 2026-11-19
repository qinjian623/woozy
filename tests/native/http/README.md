# Native std/http tests

`run_http.py` builds `tests/native/http/lang.toml` with the supplied `woozyc`
at Native `--opt 0` and `--opt 2`, runs the Woozy codec self-test, and drives
real loopback TCP fixtures from Python.

Covered:

- POST with binary request/response bodies and Unicode/obs-text header values.
- Raw `0x80`/`0xff` header values and reason phrases round-trip as bytes; NUL
  and CRLF are rejected; UTF-8 text helpers reject invalid obs-text bytes.
- Initial headers and chunk trailers remain separate. A trailer
  `Authorization` is never visible through initial-header lookup; encoder
  rejects a non-empty trailers vector explicitly.
- Duplicate headers, case-insensitive lookup, identical and conflicting
  `Content-Length`.
- Chunk extensions (including quoted obs-text), final zero chunk, trailers,
  forbidden trailers.
- HTTP/1.1 exactly-one-Host and HTTP/1.0 optional-Host rules: missing,
  duplicate, userinfo, malformed IPv6 and nondecimal ports are rejected; URI registered names,
  IPv4 and `[IPv6]:port` are accepted.
- Two pipelined requests after byte-by-byte fragmentation; the second request
  proves the pending buffer survives the first.
- Nonblocking socketpair plus `net.poll` retry after `WouldBlock` for partial
  headers, chunked, fixed-length and close-delimited messages. Retrying a
  chunked body after the first chunk does not reinterpret the final zero chunk
  as a new request.
- Incremental fixed-length request bodies and fragmented responses.
- `TE` + `CL`, malformed headers, obs-fold, control injection, header/body
  limits, decimal and hexadecimal length overflow, truncated chunks and EOF.
- HEAD, 1xx, 204, 304 rules and close-delimited HTTP/1.0 responses.
- A Woozy client (`write_request`/`read_response`) against a Python server
  fixture, and a Woozy server (`read_request`/`write_response`) against a
  Python client fixture.

Run from the repository root:

```sh
python3 -B -s tests/native/http/run_http.py \
  --cli build/woozy-stdlib-native/woozyc \
  --out /tmp/http-results
```

The prebuilt compiler used for this worker has its standard-library source
root baked to the main worktree. While validating an unintegrated
`stdlib/http`, the command was run inside an ephemeral mount namespace that
bind-mounted this worktree's `stdlib/` over that root. No compiler, runtime,
CMake file, or main-worktree source was rebuilt or changed.

The driver writes `http-results.json` under `--out`. A successful run prints:

```json
{"status": "passed", "runs": 2, "report": ".../http-results.json"}
```
