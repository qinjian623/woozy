# Native std/http_server tests

`run_http_server.py` builds `tests/native/http_server/lang.toml` with the
supplied `woozyc` at Native `--opt 0` and `--opt 2`, starts the real async
server on loopback port 0, and drives raw HTTP/1.1 bytes from Python.

Covered at both optimization levels:

- A capturing handler closure keeps shared state across requests.
- Keep-alive, pipelined requests, byte-by-byte fragmentation, request body
  echo, binary bodies, and custom header text.
- Chunked request bodies with extensions, a trailer field, and a following
  pipelined request.
- HEAD, 204, and 304 response rules, including representation
  `Content-Length` with no body.
- HTTP/1.0 default close, HTTP/1.0 explicit keep-alive, HTTP/1.1
  `Connection: close`, and mixed-case comma-separated tokens.
- Handler `Connection: close`, handler errors, invalid handler framing,
  invalid status, and non-empty response trailers.
- 400/413/431/501/505 protocol responses and EOF on truncated bodies.
- A 4 MiB response followed by another request on the same connection, and a
  1 MiB response with `large_body_threshold = 0` (head/body split path).
- A 16 MiB slow response does not delay an unrelated fast request.
- A 50,000-request pipeline on one connection does not starve another
  connection.
- Header timeout with no bytes and with a partial head; body timeout after a
  complete head and partial body; idle timeout between keep-alive requests.
- `Expect: 100-continue` is answered before the body, an unsupported
  expectation gets 417, and a request without a body gets no interim 100.
- Graceful `stop`/`drain` through a control fixture: idle connections
  close, in-flight requests finish, an incomplete body is aborted at the drain
  deadline with 408, all connection tasks exit, and `stats` reports the final
  counters.
- `max_connections` 503 rejection and `max_requests` connection close.
- 32 concurrent clients with 40 requests each.

Run from the repository root:

```sh
python3 -B -s tests/native/http_server/run_http_server.py \
  --cli build/woozyc-cpp/woozyc --out /tmp/http-server-results
```

Add `--opt 2 --benchmark` to print sequential, pipelined, and eight-client
request rates. The benchmark is informational; it is not a fixed performance
gate. The runner stores `http-server-results.json` under `--out`. A successful
run prints:

```json
{"status": "passed", "runs": 46, "report": ".../http-server-results.json"}
```

The server fixture prints `LISTENING <port>` and serves until the test process
kills it. The runner treats any server stderr output as a failure.
