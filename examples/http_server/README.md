# Async HTTP server example

A small HTTP/1.1 server written in Woozy on top of `std/http_server`. It uses
the native async executor, keep-alive, pipelining, nonblocking backpressure,
per-operation timeouts, and graceful stop/drain from the library.

## Build and run

From the repository root:

```sh
woozyc build --project examples/http_server/lang.toml --backend native \
  --opt 2 --out-dir build/http-server
build/http-server/program 127.0.0.1 8080
```

The program accepts `[HOST] [PORT]` and defaults to `127.0.0.1:8080`. `HOST` is
a numeric IPv4/IPv6 literal (or `0.0.0.0` / `::`); DNS names are not resolved.
Port `0` lets the kernel choose; the program prints the actual port:

```text
LISTENING 127.0.0.1 44677
```

## Routes

| Route | Method | Result |
| --- | --- | --- |
| `/` | any | `Woozy HTTP server\n` |
| `/hello` | any | `hello world` |
| `/health` | any | `ok` |
| `/echo` | any | request body, with `X-Method` set to the request method |
| anything else | any | `not found\n` (HTTP 404) |

```sh
curl -v http://127.0.0.1:8080/hello
curl -X POST --data-binary 'hello' http://127.0.0.1:8080/echo
```

The server is deliberately plaintext. Put it behind a TLS terminator for
HTTPS. It does not implement HTTP/2, WebSocket upgrades, streaming responses,
or application routing beyond this small example.

## Load test

With the server running, from another terminal:

```sh
python3 -B -s examples/http_server/bench.py 127.0.0.1 8080 \
  --connections 8 --requests 20000
```

It uses keep-alive and three-digit batches, checks every response, and prints
a `requests_per_second` line. The library regression suite has the deeper
benchmark and behavior checks:

```sh
python3 -B -s tests/native/http_server/run_http_server.py \
  --cli build/woozyc-cpp/woozyc --out /tmp/http-server-bench \
  --opt 2 --benchmark
```

It reports sequential, pipelined, and eight-client request rates and writes
`http-server-results.json`. Numbers depend on the machine and are not a fixed
gate.

## Production notes

- The executor is single-threaded. Keep handlers short and avoid blocking
  file I/O or C calls inside them.
- Tune `header_timeout_ms`, `body_timeout_ms`, `idle_timeout_ms`,
  `write_timeout_ms`, `max_connections`, and `max_requests` for the workload.
  This example leaves `max_requests` unlimited; set a finite value when clients
  should reconnect periodically.
- Call `stop`/`drain` for graceful shutdown, or `abort`/`close` to drop active
  connections. `std/http_server/README.md` documents the exact lifecycle.
- The server caps request bodies with `http.Limits` and exposes counters
  through `stats`.
