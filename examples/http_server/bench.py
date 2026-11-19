#!/usr/bin/env python3
"""Small keep-alive HTTP benchmark for examples/http_server.

Start the example server first, then run:

    python3 examples/http_server/bench.py 127.0.0.1 8080

It is a load generator, not a correctness oracle for the server library. The
regression suite remains tests/native/http_server/run_http_server.py.
"""
import argparse
import socket
import threading
import time


def worker(host, port, total, batch, errors, completed_out, lock):
    request = b"GET /hello HTTP/1.1\r\nHost: bench\r\n\r\n"
    marker = b"hello world"
    completed = 0
    try:
        connection = socket.create_connection((host, port), timeout=30)
        connection.settimeout(30)
        while completed < total:
            count = min(batch, total - completed)
            connection.sendall(request * count)
            got = 0
            carry = b""
            while got < count:
                data = connection.recv(65536)
                if not data:
                    raise RuntimeError("server closed early")
                window = carry + data
                got += window.count(marker)
                carry = window[-(len(marker) - 1):]
            completed += count
        connection.close()
    except BaseException as error:
        with lock:
            errors.append(repr(error))
    finally:
        with lock:
            completed_out.append(completed)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("host", nargs="?", default="127.0.0.1")
    parser.add_argument("port", nargs="?", type=int, default=8080)
    parser.add_argument("--connections", type=int, default=8)
    parser.add_argument("--requests", type=int, default=20000)
    parser.add_argument("--batch", type=int, default=32)
    args = parser.parse_args()
    if args.connections <= 0 or args.requests <= 0 or args.batch <= 0:
        parser.error("connections, requests and batch must be positive")
    per_worker = args.requests // args.connections
    if per_worker == 0:
        parser.error("requests must be at least one per connection")
    errors = []
    completed = []
    lock = threading.Lock()

    started = time.monotonic()
    workers = [threading.Thread(
        target=worker,
        args=(args.host, args.port, per_worker, args.batch,
              errors, completed, lock))
        for _ in range(args.connections)]
    for thread in workers:
        thread.start()
    for thread in workers:
        thread.join(timeout=120)
    elapsed = time.monotonic() - started
    total = per_worker * args.connections
    if any(thread.is_alive() for thread in workers):
        raise SystemExit("benchmark timed out")
    if errors:
        raise SystemExit("benchmark failed: " + "; ".join(errors[:3]))
    if sum(completed) != total:
        raise SystemExit("benchmark lost requests: %d/%d" % (sum(completed), total))
    print("requests=%d connections=%d seconds=%.3f requests_per_second=%d" % (
        total, args.connections, elapsed, int(total / elapsed)))


if __name__ == "__main__":
    main()
