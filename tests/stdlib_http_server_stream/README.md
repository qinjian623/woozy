# Linux Native streaming HTTP tests

The fixture serves a loopback socket. The Python runner sends HTTP/1.1 bytes and
checks the response, task cleanup, and server counters.

```sh
python3 -B tests/stdlib_http_server_stream/run_stream_server.py \
  --cli /path/to/woozyc --out /path/to/test-results
```

The runner builds with O0 and O2. Use `--only-opt` to select one level or
`--only-scenario` to select one case. `--build-timeout` sets the build limit in
seconds. The executable needs the checkout's standard library and matching
toolchain and runtime. See the result JSON for the programs and individual runs.

The tests check incremental reads, chunked responses, pipelining, limit
errors, nonblocking writes, and the read, processing, write, and drain
deadlines. They check that a cancelled operation leaves no bytes after a
partial response, and that the same descriptor number can serve a new socket
while an old stream handle is rejected with `ReadOrder`. The descriptor-reuse
cases read Linux `/proc` to distinguish sockets by inode.
