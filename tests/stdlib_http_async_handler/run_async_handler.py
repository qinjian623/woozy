#!/usr/bin/env python3
"""Build and drive std/http_server async handlers with raw HTTP/1.1 clients.

Python only starts the staged compiler and the resulting server process, sends
bytes, reads response bytes, and checks the fixture's lifecycle lines. It does
not evaluate Woozy or derive protocol expectations from server internals.

The compiler must be the isolated staging copy whose stdlib/http_server is the
tree under test. Both Native O0 and O2 builds are exercised by default.
"""

import argparse
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import threading
import time


ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / "tests/stdlib_http_async_handler/lang.toml"
LOOPBACK = "127.0.0.1"


class Failure(AssertionError):
    pass


def check(condition, message):
    if not condition:
        raise Failure(message)


class LineSource:
    """Collect stdout lines while allowing the test to wait for a new one."""

    def __init__(self, stream):
        self.stream = stream
        self.lines = []
        self.scan = 0
        self.eof = False
        self.condition = threading.Condition()
        self.thread = threading.Thread(target=self._read, daemon=True)
        self.thread.start()

    def _read(self):
        while True:
            raw = self.stream.readline()
            if not raw:
                break
            line = raw.decode("utf-8", "replace").rstrip("\r\n")
            with self.condition:
                self.lines.append(line)
                self.condition.notify_all()
        with self.condition:
            self.eof = True
            self.condition.notify_all()

    def wait_for(self, predicate, timeout=8.0, after=None):
        deadline = time.monotonic() + timeout
        with self.condition:
            start = self.scan if after is None else after
            while True:
                for index in range(start, len(self.lines)):
                    if predicate(self.lines[index]):
                        if after is None:
                            self.scan = index + 1
                        return index
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise Failure("timed out waiting for server output; lines=%r" % self.lines)
                if self.eof:
                    raise Failure("server stdout ended while waiting; lines=%r" % self.lines)
                self.condition.wait(remaining)

    def wait_text(self, text, timeout=8.0, after=None):
        return self.wait_for(lambda line: line == text, timeout=timeout, after=after)

    def join(self, timeout=5.0):
        self.thread.join(timeout)
        with self.condition:
            return list(self.lines)


class ServerProcess:
    def __init__(self, program, mode, body_timeout=30000, drain_timeout=1000,
                 tick=20, handler_timeout=30000):
        self.argv = [
            str(program), "0", mode,
            str(body_timeout), str(drain_timeout), str(tick),
            str(handler_timeout),
        ]
        self.mode = mode
        self.process = subprocess.Popen(
            self.argv,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0,
            env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1", PYTHONNOUSERSITE="1"),
        )
        self.lines = LineSource(self.process.stdout)
        index = self.lines.wait_for(
            lambda line: line.startswith("LISTENING "), timeout=10.0)
        line = self.lines.lines[index]
        match = re.fullmatch(r"LISTENING ([0-9]+)", line)
        check(match is not None, "bad LISTENING line: %r" % line)
        self.port = int(match.group(1))
        check(0 < self.port <= 65535, "bad port: %r" % line)

    def finish(self, timeout=8.0, expect_stderr_empty=True):
        timed_out = False
        try:
            status = self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            self.process.kill()
            status = self.process.wait(timeout=5)
        lines = self.lines.join(2.0)
        stderr = self.process.stderr.read()
        if timed_out:
            raise Failure("server did not exit; lines=%r stderr=%r" % (lines, stderr))
        if expect_stderr_empty and stderr:
            raise Failure("server wrote stderr: %r; lines=%r" % (stderr, lines))
        return status, lines, stderr

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=5)
        try:
            self.process.stdout.close()
        except OSError:
            pass
        try:
            self.process.stderr.close()
        except OSError:
            pass

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, trace):
        self.close()


class Reader:
    def __init__(self, connection):
        self.connection = connection
        self.buffer = bytearray()

    def fill(self):
        chunk = self.connection.recv(65536)
        if not chunk:
            return False
        self.buffer.extend(chunk)
        return True

    def until(self, marker):
        while True:
            index = self.buffer.find(marker)
            if index >= 0:
                end = index + len(marker)
                data = bytes(self.buffer[:end])
                del self.buffer[:end]
                return data
            if not self.fill():
                raise Failure("EOF before %r; buffer=%r" % (
                    marker, bytes(self.buffer[:160])))

    def take(self, count):
        while len(self.buffer) < count:
            if not self.fill():
                raise Failure("EOF with %d/%d bytes buffered" % (
                    len(self.buffer), count))
        data = bytes(self.buffer[:count])
        del self.buffer[:count]
        return data

    def at_eof(self, timeout=2.0):
        if self.buffer:
            return False
        self.connection.settimeout(timeout)
        try:
            chunk = self.connection.recv(1)
        except (socket.timeout, TimeoutError):
            return False
        if not chunk:
            return True
        self.buffer.extend(chunk)
        return False


def connect(port, timeout=8.0):
    connection = socket.create_connection((LOOPBACK, port), timeout=timeout)
    connection.settimeout(timeout)
    return connection


def send_request(connection, target, method="GET", body=b"", headers=()):
    head = [
        "%s %s HTTP/1.1" % (method, target),
        "Host: 127.0.0.1",
        "Content-Length: %d" % len(body),
    ]
    for name, value in headers:
        head.append("%s: %s" % (name, value))
    connection.sendall(("\r\n".join(head) + "\r\n\r\n").encode("ascii") + body)


def send_chunked(connection, target, chunks, trailers=()):
    head = [
        "POST %s HTTP/1.1" % target,
        "Host: 127.0.0.1",
        "Transfer-Encoding: chunked",
    ]
    request = ("\r\n".join(head) + "\r\n\r\n").encode("ascii")
    for chunk in chunks:
        request += ("%x\r\n" % len(chunk)).encode("ascii") + chunk + b"\r\n"
    request += b"0\r\n"
    for name, value in trailers:
        request += ("%s: %s\r\n" % (name, value)).encode("ascii")
    request += b"\r\n"
    connection.sendall(request)


def read_response(reader, method="GET"):
    head = reader.until(b"\r\n\r\n")
    lines = head[:-4].split(b"\r\n")
    parts = lines[0].split(b" ", 2)
    check(len(parts) == 3, "bad status line: %r" % lines[0])
    status = int(parts[1])
    headers = {}
    for line in lines[1:]:
        check(b":" in line, "bad header line: %r" % line)
        name, value = line.split(b":", 1)
        headers.setdefault(name.strip().lower(), []).append(value.strip())
    no_body = method == "HEAD" or 100 <= status < 200 or status in (204, 304)
    if no_body:
        body = b""
    else:
        lengths = headers.get(b"content-length", [])
        check(lengths, "response lacks Content-Length: %r" % lines)
        body = reader.take(int(lengths[0]))
    return status, headers, body


def parse_stats(lines):
    for line in lines:
        if line.startswith("STATS "):
            fields = {}
            for item in line[6:].split():
                name, value = item.split("=", 1)
                fields[name] = value
            return fields
    raise Failure("STATS line missing; lines=%r" % lines)


def check_drained(lines, expect_timeouts=None):
    check("DRAINED true" in lines, "graceful drain did not finish: %r" % lines)
    stats = parse_stats(lines)
    check(stats.get("active") == "0", "active connections remain: %r" % stats)
    check(stats.get("stopping") == "true", "stopping flag not set: %r" % stats)
    if expect_timeouts is not None:
        check(int(stats.get("timeouts", "-1")) == expect_timeouts,
              "timeout count mismatch: %r" % stats)
    return stats


def require_order(lines, first, second):
    check(first in lines and second in lines,
          "missing lifecycle line: %r in %r" % ((first, second), lines))
    check(lines.index(first) < lines.index(second),
          "wrong lifecycle order: %r before %r in %r" % (first, second, lines))


def scenario_basic(program, opt):
    server = ServerProcess(program, "async")
    try:
        connection = connect(server.port)
        reader = Reader(connection)
        send_request(connection, "/fast")
        status, headers, body = read_response(reader)
        check((status, body) == (200, b"fast"), "fast response: %r/%r" % (status, body))
        check(headers.get(b"connection") == [b"keep-alive"], "keep-alive header: %r" % headers)
        send_request(connection, "/echo", method="POST", body=b"abc\x00def")
        status, headers, body = read_response(reader)
        check((status, body) == (200, b"abc\x00def"), "echo response: %r/%r" % (status, body))
        send_request(connection, "/fast?tag=keepalive")
        status, headers, body = read_response(reader)
        check((status, body) == (200, b"fast"), "second keep-alive response: %r/%r" % (status, body))
        connection.close()

        chunked = connect(server.port)
        chunked_reader = Reader(chunked)
        send_chunked(chunked, "/echo", [b"ab", b"cdef"], [("X-Trailer", "yes")])
        status, headers, body = read_response(chunked_reader)
        check((status, body) == (200, b"abcdef"), "chunked echo response: %r/%r" % (status, body))
        chunked.close()

        expect = connect(server.port)
        expect_reader = Reader(expect)
        expect.sendall(
            b"POST /echo HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 5\r\n"
            b"Expect: 100-continue\r\n\r\n")
        status, headers, body = read_response(expect_reader)
        check((status, body) == (100, b""), "100-continue response: %r/%r" % (status, body))
        expect.sendall(b"hello")
        status, headers, body = read_response(expect_reader)
        check((status, body) == (200, b"hello"), "expect echo response: %r/%r" % (status, body))
        expect.close()

        head = connect(server.port)
        head_reader = Reader(head)
        send_request(head, "/fast", method="HEAD")
        status, headers, body = read_response(head_reader, method="HEAD")
        check((status, body) == (200, b""), "HEAD response: %r/%r" % (status, body))
        check(headers.get(b"content-length") == [b"4"], "HEAD content length: %r" % headers)
        head.close()

        failed = connect(server.port)
        failure_reader = Reader(failed)
        send_request(failed, "/fail")
        status, headers, body = read_response(failure_reader)
        check((status, body) == (500, b""), "handler error response: %r/%r" % (status, body))
        check(failure_reader.at_eof(), "handler error connection stayed open")
        failed.close()

        slow = connect(server.port)
        slow_reader = Reader(slow)
        send_request(slow, "/slow/600")
        start = server.lines.wait_text("SLOW-START /slow/600")
        fast = connect(server.port)
        fast_reader = Reader(fast)
        started = time.monotonic()
        send_request(fast, "/fast?tag=concurrent")
        status, headers, body = read_response(fast_reader)
        latency = time.monotonic() - started
        check((status, body) == (200, b"fast"), "concurrent fast response: %r/%r" % (status, body))
        check(latency < 0.4, "fast request blocked behind slow handler: %.3fs" % latency)
        fast_start = server.lines.wait_text("FAST-START /fast?tag=concurrent", after=start)
        slow_done = server.lines.wait_text("SLOW-DONE /slow/600", after=start)
        check(fast_start < slow_done,
              "fast handler did not run while slow handler awaited: %r" % server.lines.lines)
        status, headers, body = read_response(slow_reader)
        elapsed = time.monotonic() - started
        check((status, body) == (200, b"slow"), "slow response: %r/%r" % (status, body))
        check(elapsed >= 0.5, "slow handler returned too early: %.3fs" % elapsed)
        fast.close()
        slow.close()

        pipelined = connect(server.port)
        pipelined_reader = Reader(pipelined)
        started = time.monotonic()
        send_request(pipelined, "/slow/450")
        send_request(pipelined, "/fast?tag=pipeline")
        status, headers, body = read_response(pipelined_reader)
        first_latency = time.monotonic() - started
        check((status, body) == (200, b"slow"), "pipeline first response: %r/%r" % (status, body))
        check(first_latency >= 0.35,
              "pipelined request was handled concurrently with the slow request: %.3fs" % first_latency)
        status, headers, body = read_response(pipelined_reader)
        check((status, body) == (200, b"fast"), "pipeline second response: %r/%r" % (status, body))
        pipeline_start = server.lines.wait_text("SLOW-START /slow/450")
        pipeline_done = server.lines.wait_text("SLOW-DONE /slow/450", after=pipeline_start)
        pipeline_fast = server.lines.wait_text("FAST-START /fast?tag=pipeline", after=pipeline_start)
        check(pipeline_done < pipeline_fast,
              "second request started before the first handler finished: %r" % server.lines.lines)
        pipelined.close()

        stop = connect(server.port)
        stop_reader = Reader(stop)
        send_request(stop, "/stop", headers=[("Connection", "close")])
        status, headers, body = read_response(stop_reader)
        check((status, body) == (200, b"stopping"), "stop response: %r/%r" % (status, body))
        stop.close()

        status, lines, stderr = server.finish()
        check(status == 0, "server exit %r; lines=%r stderr=%r" % (status, lines, stderr))
        stats = check_drained(lines, expect_timeouts=0)
        check(int(stats["requests"]) >= 9, "request counter too small: %r" % stats)
        check(int(stats["errors"]) >= 1, "handler error was not counted: %r" % stats)
        return {"scenario": "basic", "opt": opt, "stats": stats, "latency_ms": round(latency * 1000)}
    finally:
        server.close()


def scenario_stop(program, opt):
    server = ServerProcess(program, "async", drain_timeout=250, tick=20)
    try:
        hold = connect(server.port)
        send_request(hold, "/hold/stop")
        server.lines.wait_text("HOLD-START /hold/stop")
        stop = connect(server.port)
        send_request(stop, "/stop", headers=[("Connection", "close")])
        try:
            read_response(Reader(stop))
        except Failure:
            pass
        slow_status, lines, stderr = server.finish(timeout=6)
        check(slow_status == 0, "stop server exit %r; lines=%r stderr=%r" % (
            slow_status, lines, stderr))
        check("HOLD-CLEANUP /hold/stop" in lines, "cancelled handler cleanup missing: %r" % lines)
        check("HOLD-AFTER /hold/stop" not in lines, "cancelled handler kept running: %r" % lines)
        require_order(lines, "HOLD-START /hold/stop", "HOLD-CLEANUP /hold/stop")
        stats = check_drained(lines)
        check(int(stats["timeouts"]) >= 1, "drain deadline did not count a timeout: %r" % stats)
        hold.close()
        stop.close()
        return {"scenario": "stop-cancels-handler", "opt": opt, "stats": stats}
    finally:
        server.close()


def scenario_stop_graceful(program, opt):
    server = ServerProcess(program, "async", drain_timeout=500, tick=20)
    try:
        slow = connect(server.port)
        slow_reader = Reader(slow)
        send_request(slow, "/slow/150")
        server.lines.wait_text("SLOW-START /slow/150")
        stop = connect(server.port)
        stop_reader = Reader(stop)
        send_request(stop, "/stop", headers=[("Connection", "close")])
        status, headers, body = read_response(stop_reader)
        check((status, body) == (200, b"stopping"), "stop response: %r/%r" % (status, body))
        status, headers, body = read_response(slow_reader)
        check((status, body) == (200, b"slow"), "in-flight async response: %r/%r" % (status, body))
        slow.close()
        stop.close()
        slow_status, lines, stderr = server.finish(timeout=6)
        check(slow_status == 0, "graceful stop server exit %r; lines=%r stderr=%r" % (
            slow_status, lines, stderr))
        check("SLOW-DONE /slow/150" in lines, "in-flight handler did not finish: %r" % lines)
        stats = check_drained(lines, expect_timeouts=0)
        check(int(stats["requests"]) >= 2, "in-flight response was not counted: %r" % stats)
        return {"scenario": "stop-allows-handler-finish", "opt": opt, "stats": stats}
    finally:
        server.close()


def scenario_abort(program, opt):
    server = ServerProcess(program, "async", tick=20)
    try:
        hold = connect(server.port)
        send_request(hold, "/hold/abort")
        server.lines.wait_text("HOLD-START /hold/abort")
        abort = connect(server.port)
        send_request(abort, "/abort", headers=[("Connection", "close")])
        try:
            read_response(Reader(abort))
        except Failure:
            pass
        slow_status, lines, stderr = server.finish(timeout=6)
        check(slow_status == 0, "abort server exit %r; lines=%r stderr=%r" % (
            slow_status, lines, stderr))
        check("HOLD-CLEANUP /hold/abort" in lines, "aborted handler cleanup missing: %r" % lines)
        check("HOLD-AFTER /hold/abort" not in lines, "aborted handler kept running: %r" % lines)
        require_order(lines, "HOLD-START /hold/abort", "HOLD-CLEANUP /hold/abort")
        stats = check_drained(lines, expect_timeouts=0)
        hold.close()
        abort.close()
        return {"scenario": "abort-cancels-handler", "opt": opt, "stats": stats}
    finally:
        server.close()


def scenario_timeout(program, opt):
    # The body deadline must not leak into async handler execution. This
    # handler would have been cancelled by the 150 ms body deadline under the
    # old behavior; now it remains active until the explicit stop/drain.
    server = ServerProcess(
        program, "async", body_timeout=150, drain_timeout=1000,
        tick=20, handler_timeout=2000)
    try:
        hold = connect(server.port)
        reader = Reader(hold)
        send_request(hold, "/hold/timeout")
        server.lines.wait_text("HOLD-START /hold/timeout")
        time.sleep(0.4)
        check("HOLD-CLEANUP /hold/timeout" not in server.lines.lines,
              "body timeout leaked into async handler: %r" % server.lines.lines)
        check(not reader.at_eof(timeout=0.3),
              "async handler connection closed at the body deadline")
        hold.close()

        stop = connect(server.port)
        stop_reader = Reader(stop)
        send_request(stop, "/stop", headers=[("Connection", "close")])
        status, headers, body = read_response(stop_reader)
        check((status, body) == (200, b"stopping"),
              "stop response: %r/%r" % (status, body))
        stop.close()

        slow_status, lines, stderr = server.finish(timeout=6)
        check(slow_status == 0, "handler-deadline server exit %r; lines=%r stderr=%r" %
              (slow_status, lines, stderr))
        stats = check_drained(lines)
        check(int(stats["timeouts"]) >= 1,
              "drain timeout was not counted: %r" % stats)
        check("HOLD-CLEANUP /hold/timeout" in lines,
              "handler cleanup missing: %r" % lines)
        return {"scenario": "body-deadline-does-not-cancel-async-handler",
                "opt": opt, "stats": stats}
    finally:
        server.close()


def scenario_handler_timeout(program, opt):
    server = ServerProcess(
        program, "async", body_timeout=2000, drain_timeout=1000,
        tick=20, handler_timeout=150)
    try:
        connection = connect(server.port)
        reader = Reader(connection)
        connection.sendall(
            b"POST /hold/handler-timeout HTTP/1.1\r\nHost: 127.0.0.1\r\n"
            b"Content-Length: 6\r\n\r\nabc")
        time.sleep(0.2)
        connection.sendall(b"def")
        started_index = server.lines.wait_text(
            "HOLD-START /hold/handler-timeout", timeout=3)
        cleanup_index = server.lines.wait_text(
            "HOLD-CLEANUP /hold/handler-timeout", timeout=4)
        check(cleanup_index > started_index, "handler timeout order")
        check("HOLD-AFTER /hold/handler-timeout" not in server.lines.lines,
              "timed-out async handler kept running: %r" % server.lines.lines)
        check(reader.at_eof(timeout=3), "handler-timeout connection stayed open")
        connection.close()

        stop = connect(server.port)
        stop_reader = Reader(stop)
        send_request(stop, "/stop", headers=[("Connection", "close")])
        status, headers, body = read_response(stop_reader)
        check((status, body) == (200, b"stopping"),
              "handler-timeout stop response: %r/%r" % (status, body))
        stop.close()
        status, lines, stderr = server.finish(timeout=8)
        check(status == 0, "handler-timeout server exit %r; lines=%r stderr=%r" %
              (status, lines, stderr))
        stats = check_drained(lines)
        check(int(stats["timeouts"]) >= 1,
              "async handler timeout was not counted: %r" % stats)
        check(int(stats["errors"]) == 0, "async handler timeout counted an error: %r" % stats)
        return {"scenario": "async-handler-timeout-after-body", "opt": opt,
                "stats": stats}
    finally:
        server.close()


def scenario_registration(program, opt, mode):
    server = ServerProcess(program, mode, tick=20)
    try:
        connection = connect(server.port)
        reader = Reader(connection)
        send_request(connection, "/fast")
        status, headers, body = read_response(reader)
        check((status, body) == (200, b"fast"), "%s fast response: %r/%r" % (mode, status, body))
        send_request(connection, "/echo", method="POST", body=b"registered")
        status, headers, body = read_response(reader)
        check((status, body) == (200, b"registered"), "%s echo response: %r/%r" % (mode, status, body))
        send_request(connection, "/stop", headers=[("Connection", "close")])
        status, headers, body = read_response(reader)
        check((status, body) == (200, b"stopping"), "%s stop response: %r/%r" % (mode, status, body))
        connection.close()
        status, lines, stderr = server.finish()
        check(status == 0, "%s server exit %r; lines=%r stderr=%r" % (
            mode, status, lines, stderr))
        stats = check_drained(lines, expect_timeouts=0)
        check(int(stats["requests"]) >= 3, "%s request counter: %r" % (mode, stats))
        return {"scenario": "registration-" + mode, "opt": opt, "stats": stats}
    finally:
        server.close()


def build(cli, out_dir, opt):
    command = [
        str(cli), "build", "--project", str(MANIFEST),
        "--backend", "native", "--opt", str(opt), "--out-dir", str(out_dir),
    ]
    print("BUILD " + " ".join(command), flush=True)
    result = subprocess.run(
        command,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1", PYTHONNOUSERSITE="1"),
        timeout=600,
    )
    if result.returncode != 0:
        raise Failure(
            "build --opt %s failed (%d):\n%s\n%s" % (
                opt, result.returncode,
                result.stdout.decode("utf-8", "backslashreplace"),
                result.stderr.decode("utf-8", "backslashreplace")))
    program = out_dir / "program"
    check(program.is_file(), "build did not publish %s" % program)
    return program


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", required=True, help="isolated staged woozyc")
    parser.add_argument("--out", default="/tmp/http-async-handler-results")
    parser.add_argument("--only-opt", choices=("0", "2"))
    parser.add_argument("--libxml", type=Path)
    arguments = parser.parse_args()
    if arguments.libxml and arguments.libxml.is_dir():
        os.environ["LD_LIBRARY_PATH"] = os.pathsep.join(filter(None, (
            str(arguments.libxml.resolve()), os.environ.get("LD_LIBRARY_PATH"),
        )))
    cli = Path(arguments.cli).resolve()
    check(cli.is_file(), "compiler is not a file: %s" % cli)
    out = Path(arguments.out)
    out.mkdir(parents=True, exist_ok=True)
    options = [arguments.only_opt] if arguments.only_opt else ["0", "2"]
    report = {"status": "passed", "compiler": str(cli), "builds": [], "runs": []}
    try:
        for opt in options:
            program = build(cli, out / ("opt" + opt), opt)
            report["builds"].append({"opt": int(opt), "program": str(program)})
            report["runs"].append(scenario_basic(program, int(opt)))
            report["runs"].append(scenario_stop_graceful(program, int(opt)))
            report["runs"].append(scenario_stop(program, int(opt)))
            report["runs"].append(scenario_abort(program, int(opt)))
            report["runs"].append(scenario_timeout(program, int(opt)))
            report["runs"].append(scenario_handler_timeout(program, int(opt)))
            for mode in ("async-default", "sync-default", "sync-with"):
                report["runs"].append(scenario_registration(program, int(opt), mode))
    except Failure as error:
        report["status"] = "failed"
        report["failure"] = str(error)
        (out / "http-async-handler-results.json").write_text(
            json.dumps(report, indent=2) + "\n")
        print("FAILED: %s" % error, flush=True)
        return 1
    (out / "http-async-handler-results.json").write_text(
        json.dumps(report, indent=2) + "\n")
    print(json.dumps({
        "status": "passed",
        "runs": len(report["runs"]),
        "report": str(out / "http-async-handler-results.json"),
    }), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
