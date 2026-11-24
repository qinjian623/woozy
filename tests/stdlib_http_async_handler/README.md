# std/http_server async handler tests

This fixture exercises `listen_async` and `listen_async_with` with the released
Native compiler. Python only starts the staged compiler, drives raw HTTP/1.1
sockets, reads response bytes and fixture lifecycle lines, and checks process
exit status and stats. It never evaluates a Woozy program or derives protocol
expectations from server internals.

## Prerequisites

Use an isolated staging directory that contains the compiler, its toolchain and
runtime, and a `stdlib/http_server` copy of the tree under test. The installed
release package must remain unchanged. For example:

```sh
SRC=/tmp/woozy-install-linux.mGq1t1xr/prefix/opt/woozy-0.1.0-rc.2
STAGE=/tmp/woozy-async-stage
rm -rf "$STAGE"
mkdir -p "$STAGE/stdlib"
cp --reflink=auto "$SRC/woozyc" "$STAGE/woozyc"
chmod 755 "$STAGE/woozyc"
for d in "$SRC"/stdlib/*; do
  name=$(basename "$d")
  [ "$name" = http_server ] || ln -s "$d" "$STAGE/stdlib/$name"
done
ln -s "$SRC/runtime" "$STAGE/runtime"
ln -s "$SRC/toolchain" "$STAGE/toolchain"
ln -s "$SRC/release-metadata" "$STAGE/release-metadata"
cp -a stdlib/http_server "$STAGE/stdlib/http_server"
```

## Run

From the repository root:

```sh
python3 -B -s tests/stdlib_http_async_handler/run_async_handler.py \
  --cli /tmp/woozy-async-stage/woozyc \
  --out /tmp/http-async-handler-results
```

The runner builds `lang.toml` at Native `--opt` 0 and `--opt` 2, then runs the
same scenarios against each binary. A successful run prints a compact JSON
summary and writes `http-async-handler-results.json` under `--out`.

## Covered at both optimization levels

- `listen_async_with` fast, echo, keep-alive, chunked request body with a
  trailer, `Expect: 100-continue`, HEAD without a body, and async handler error
  (500).
- A fast async request completes while another connection's slow handler is
  awaiting, proving separate connections participate in the same executor.
- Two pipelined requests on one connection run serially: the second handler
  starts only after the first handler's `SLOW-DONE` line.
- `/stop` with a 500 ms drain deadline lets a 150 ms in-flight handler finish
  and counts its response.
- `/stop` with a 250 ms drain deadline cancels a 5 s handler; the handler's
  `defer` prints cleanup, `HOLD-AFTER` never appears, and `timeouts` is 1.
- `/abort` cancels a 5 s handler immediately, prints cleanup, and reports
  `timeouts = 0` because no watchdog deadline fired.
- A 150 ms `body_timeout_ms` expires while a 5 s handler is active; the watchdog
  cancels the handler, the connection reaches EOF, and `timeouts` is 1.
- `listen_async` (default config), `listen` (default config), and
  `listen_with` (custom config) still serve fast/echo/stop correctly.
- The server process writes no stderr and exits 0 after each control request.

All scenarios assert `active=0` and `stopping=true` after drain, and the basic
scenario also asserts that async handler failures increment `errors`.

## Files

- `src/main.wz`: async/sync route fixture and stop/abort/stats controller.
- `lang.toml`: Native fixture manifest.
- `run_async_handler.py`: build and wire-test driver.
