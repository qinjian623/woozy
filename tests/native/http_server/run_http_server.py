#!/usr/bin/env python3
"""Build and drive the real std/http_server async server.

Python only starts processes, writes raw HTTP bytes, and checks the bytes that
come back. It does not evaluate Woozy or reimplement server behavior to derive
expected values.

Run from the repository root:

    python3 -B -s tests/native/http_server/run_http_server.py \
      --cli build/woozyc-cpp/woozyc --out /tmp/http-server-results

The runner builds the fixture at Native O0 and O2, then runs the same wire
checks against each binary. A successful run prints a compact JSON summary.
"""

import argparse
import json
import os
from pathlib import Path
import select
import socket
import subprocess
import sys
import threading
import time
import traceback


ROOT = Path(__file__).resolve().parents[3]
MANIFEST = ROOT / "tests/native/http_server/lang.toml"
CONTROL_MANIFEST = ROOT / "tests/native/http_server/control.toml"
LOOPBACK = "127.0.0.1"
BINARY = bytes(range(256)) * 4


class Failure(AssertionError):
    pass


def check(condition, message):
    if not condition:
        raise Failure(message)


def environment_for(cli):
    environment = dict(os.environ, PYTHONDONTWRITEBYTECODE="1", PYTHONNOUSERSITE="1")
    for parent in cli.parents:
        library = parent / "toolchains/libxml2-legacy-2.13.9/usr/lib"
        if (library / "libxml2.so.2").exists():
            previous = environment.get("LD_LIBRARY_PATH")
            environment["LD_LIBRARY_PATH"] = str(library) + (":" + previous if previous else "")
            break
    return environment


def build(cli, out_dir, opt, environment, manifest=MANIFEST):
    result = subprocess.run(
        [str(cli), "build", "--project", str(manifest), "--backend", "native",
         "--opt", str(opt), "--out-dir", str(out_dir)],
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        env=environment, timeout=600)
    if result.returncode != 0:
        raise Failure(
            "build --opt %d failed (%d):\n%s\n%s" % (
                opt, result.returncode,
                result.stdout.decode("utf-8", "backslashreplace"),
                result.stderr.decode("utf-8", "backslashreplace")))
    program = out_dir / "program"
    check(program.is_file(), "build did not publish %s" % program)
    return program


DEFAULT_SERVER_OPTIONS = dict(
    max_connections=0,
    max_requests=0,
    header_timeout_ms=10000,
    body_timeout_ms=30000,
    idle_timeout_ms=60000,
    write_timeout_ms=30000,
    drain_timeout_ms=10000,
    timer_tick_ms=100,
    large_body_threshold=16384,
)


class ServerProcess:
    def __init__(self, program, **options):
        settings = dict(DEFAULT_SERVER_OPTIONS)
        settings.update(options)
        self.options = settings
        argv = [str(program), "0"] + [
            str(settings[key]) for key in (
                "max_connections", "max_requests", "header_timeout_ms",
                "body_timeout_ms", "idle_timeout_ms", "write_timeout_ms",
                "drain_timeout_ms", "timer_tick_ms", "large_body_threshold")]
        self.process = subprocess.Popen(
            argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, bufsize=0)
        self.port = self._read_listening()

    def line(self, timeout=8.0):
        deadline = time.monotonic() + timeout
        line = bytearray()
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise Failure("server stdout line timeout")
            ready, _, _ = select.select([self.process.stdout.fileno()], [], [], remaining)
            if not ready:
                continue
            chunk = os.read(self.process.stdout.fileno(), 1)
            if not chunk:
                raise Failure("server closed stdout before a line")
            line.extend(chunk)
            if chunk == b"\n":
                return bytes(line).strip()

    def _read_listening(self):
        text = self.line()
        check(text.startswith(b"LISTENING "), "bad listening line: %r" % text)
        port = int(text.split()[1])
        check(0 < port <= 65535, "bad port: %r" % text)
        return port

    def connect(self, timeout=8.0):
        connection = socket.create_connection((LOOPBACK, self.port), timeout=timeout)
        connection.settimeout(timeout)
        return connection

    def stderr_text(self):
        if self.process.poll() is None:
            return b""
        return self.process.stderr.read()

    def stop(self):
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
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=5)
        stderr = self.process.stderr.read()
        self.stop()
        if stderr and exc_type is None:
            raise Failure("server wrote stderr: %r" % stderr)
        if stderr:
            print("server stderr while failing: %r" % stderr, flush=True)


class Reader:
    def __init__(self, connection):
        self.connection = connection
        self.buffer = bytearray()
        self.eof = False

    def fill(self):
        chunk = self.connection.recv(65536)
        if not chunk:
            self.eof = True
            return False
        self.buffer.extend(chunk)
        return True

    def take(self, count):
        while len(self.buffer) < count:
            if not self.fill():
                raise Failure("EOF with %d/%d bytes buffered" % (len(self.buffer), count))
        data = bytes(self.buffer[:count])
        del self.buffer[:count]
        return data

    def until(self, marker):
        while True:
            index = self.buffer.find(marker)
            if index >= 0:
                end = index + len(marker)
                data = bytes(self.buffer[:end])
                del self.buffer[:end]
                return data
            if not self.fill():
                raise Failure("EOF before %r (buffer %r)" % (marker, bytes(self.buffer[:160])))

    def to_eof(self):
        while self.fill():
            pass
        data = bytes(self.buffer)
        self.buffer.clear()
        return data

    def at_eof(self, timeout=2.0):
        if self.buffer:
            return False
        self.connection.settimeout(timeout)
        try:
            chunk = self.connection.recv(1)
        except (socket.timeout, TimeoutError):
            return False
        except ConnectionResetError:
            self.eof = True
            return True
        if not chunk:
            self.eof = True
            return True
        self.buffer.extend(chunk)
        return False


def read_response(reader, method="GET"):
    head = reader.until(b"\r\n\r\n")
    lines = head[:-4].split(b"\r\n")
    start = lines[0]
    parts = start.split(b" ", 2)
    check(len(parts) == 3, "bad status line: %r" % start)
    version = parts[0]
    status = int(parts[1])
    reason = parts[2]
    headers = []
    for line in lines[1:]:
        check(b":" in line, "bad header line: %r" % line)
        name, value = line.split(b":", 1)
        headers.append((name, value.strip(b" \t")))
    lengths = header_values(headers, "content-length")
    no_body = method == "HEAD" or 100 <= status < 200 or status in (204, 304)
    if no_body:
        body = b""
    elif lengths:
        check(len(lengths) == 1, "duplicate content-length: %r" % lengths)
        body = reader.take(int(lengths[0]))
    else:
        body = reader.to_eof()
    return dict(version=version, status=status, reason=reason, headers=headers, body=body)


def header_values(headers, name):
    wanted = name.lower().encode("ascii")
    return [value for key, value in headers if key.lower() == wanted]


def header_text(headers, name):
    values = header_values(headers, name)
    return [value.decode("latin1") for value in values]


def request_bytes(method, target, headers=(), body=b"", version="HTTP/1.1"):
    lines = ["%s %s %s" % (method, target, version)]
    if version == "HTTP/1.1" and not any(name.lower() == "host" for name, _ in headers):
        lines.append("Host: fixture")
    for name, value in headers:
        lines.append("%s: %s" % (name, value))
    if body:
        lines.append("Content-Length: %d" % len(body))
    return ("\r\n".join(lines) + "\r\n\r\n").encode("latin1") + body


def send_request(connection, method, target, headers=(), body=b"", version="HTTP/1.1"):
    connection.sendall(request_bytes(method, target, headers, body, version))


def expect_response(reader, status, body, method="GET", version=b"HTTP/1.1",
                    connection_header=None):
    response = read_response(reader, method)
    check(response["status"] == status,
          "status %d != %d (%r)" % (response["status"], status, response))
    check(response["version"] == version,
          "version %r != %r" % (response["version"], version))
    check(response["body"] == body,
          "body %r != %r" % (response["body"][:80], body[:80]))
    if connection_header is not None:
        values = header_text(response["headers"], "Connection")
        check(values == [connection_header],
              "Connection %r != %r" % (values, [connection_header]))
    return response


def run_case(factory):
    def wrapper(program):
        with ServerProcess(program) as server:
            factory(server)
    wrapper.__name__ = factory.__name__
    return wrapper


@run_case
def closure_handler_state(server):
    connection = server.connect()
    reader = Reader(connection)
    send_request(connection, "GET", "/count")
    expect_response(reader, 200, b"1")
    send_request(connection, "GET", "/hello")
    expect_response(reader, 200, b"hello world")
    send_request(connection, "GET", "/count")
    expect_response(reader, 200, b"3")
    connection.close()


@run_case
def basic_keep_alive(server):
    connection = server.connect()
    reader = Reader(connection)
    send_request(connection, "GET", "/hello")
    response = expect_response(reader, 200, b"hello world", connection_header="keep-alive")
    check(header_text(response["headers"], "Content-Length") == ["11"], response)
    send_request(connection, "GET", "/head")
    expect_response(reader, 200, b"head-body-12345", connection_header="keep-alive")
    connection.close()


@run_case
def binary_and_headers(server):
    connection = server.connect()
    reader = Reader(connection)
    send_request(connection, "POST", "/echo", [("X-Test", "value")], BINARY)
    response = expect_response(reader, 200, BINARY)
    check(header_text(response["headers"], "X-Method") == ["POST"], response)
    send_request(connection, "GET", "/header", [("X-Test", "hello-header")])
    expect_response(reader, 200, b"hello-header")
    connection.close()


@run_case
def pipeline(server):
    connection = server.connect()
    reader = Reader(connection)
    stream = (request_bytes("GET", "/hello") +
              request_bytes("POST", "/echo", (), b"abc") +
              request_bytes("GET", "/close") +
              request_bytes("GET", "/hello"))
    connection.sendall(stream)
    expect_response(reader, 200, b"hello world")
    expect_response(reader, 200, b"abc")
    expect_response(reader, 200, b"closing", connection_header="close")
    check(reader.at_eof(), "server kept the connection after close response")
    connection.close()


@run_case
def fragmented(server):
    connection = server.connect()
    reader = Reader(connection)
    payload = request_bytes("POST", "/echo", [("X-Test", "frag")], b"0123456789")
    for index in range(0, len(payload), 3):
        connection.sendall(payload[index:index + 3])
        time.sleep(0.0005)
    response = expect_response(reader, 200, b"0123456789")
    check(header_text(response["headers"], "X-Method") == ["POST"], response)
    connection.close()


@run_case
def chunked_and_trailers(server):
    connection = server.connect()
    reader = Reader(connection)
    connection.sendall(
        b"POST /chunked HTTP/1.1\r\n"
        b"Host: fixture\r\n"
        b"Transfer-Encoding: chunked\r\n\r\n"
        b"4;foo=bar\r\nWiki\r\n"
        b"5;quoted=\"a b\"\r\npedia\r\n"
        b"0;end=yes\r\n"
        b"X-Trailer: done\r\n\r\n"
        b"GET /hello HTTP/1.1\r\nHost: fixture\r\n\r\n")
    response = expect_response(reader, 200, b"Wikipedia")
    check(header_text(response["headers"], "X-Trailers") == ["1"], response)
    expect_response(reader, 200, b"hello world")
    connection.close()


@run_case
def no_body_rules(server):
    connection = server.connect()
    reader = Reader(connection)
    send_request(connection, "HEAD", "/head")
    response = read_response(reader, "HEAD")
    check(response["status"] == 200, response)
    check(response["body"] == b"", response)
    check(header_text(response["headers"], "Content-Length") == ["15"], response)
    send_request(connection, "HEAD", "/hello")
    response = read_response(reader, "HEAD")
    check(response["body"] == b"", response)
    check(header_text(response["headers"], "Content-Length") == ["11"], response)
    send_request(connection, "GET", "/nocontent")
    response = expect_response(reader, 204, b"")
    check(not header_values(response["headers"], "Content-Length"), response)
    send_request(connection, "GET", "/notmodified")
    response = expect_response(reader, 304, b"")
    check(header_text(response["headers"], "Content-Length") == ["11"], response)
    send_request(connection, "HEAD", "/nocontent")
    response = read_response(reader, "HEAD")
    check(response["status"] == 204, response)
    check(response["body"] == b"", response)
    check(not header_values(response["headers"], "Content-Length"), response)
    send_request(connection, "HEAD", "/notmodified")
    response = read_response(reader, "HEAD")
    check(response["status"] == 304, response)
    check(response["body"] == b"", response)
    check(header_text(response["headers"], "Content-Length") == ["11"], response)
    send_request(connection, "GET", "/hello")
    expect_response(reader, 200, b"hello world")
    connection.close()


@run_case
def connection_policy(server):
    connection = server.connect()
    reader = Reader(connection)
    send_request(connection, "GET", "/hello", version="HTTP/1.0")
    expect_response(reader, 200, b"hello world", version=b"HTTP/1.0",
                    connection_header="close")
    check(reader.at_eof(), "HTTP/1.0 default did not close")
    connection.close()

    connection = server.connect()
    reader = Reader(connection)
    send_request(connection, "GET", "/hello", [("Connection", "Keep-Alive")],
                 version="HTTP/1.0")
    expect_response(reader, 200, b"hello world", version=b"HTTP/1.0",
                    connection_header="keep-alive")
    send_request(connection, "GET", "/hello", [("Connection", "close")],
                 version="HTTP/1.0")
    expect_response(reader, 200, b"hello world", version=b"HTTP/1.0",
                    connection_header="close")
    check(reader.at_eof(), "explicit close token was ignored")
    connection.close()

    connection = server.connect()
    reader = Reader(connection)
    send_request(connection, "GET", "/hello",
                 [("Connection", "keep-alive, Close")])
    expect_response(reader, 200, b"hello world", connection_header="close")
    check(reader.at_eof(), "mixed-case close token was ignored")
    connection.close()


@run_case
def response_framing(server):
    connection = server.connect()
    reader = Reader(connection)
    send_request(connection, "GET", "/bad-cl")
    response = expect_response(reader, 200, b"abc")
    check(header_text(response["headers"], "Content-Length") == ["3"], response)
    send_request(connection, "GET", "/hello")
    expect_response(reader, 200, b"hello world")
    connection.close()

    for target in ("/bad-te", "/bad-status", "/trailers", "/handler-error"):
        connection = server.connect()
        reader = Reader(connection)
        send_request(connection, "GET", target)
        expect_response(reader, 500, b"", connection_header="close")
        check(reader.at_eof(), "%s did not close after 500" % target)
        connection.close()

    connection = server.connect()
    reader = Reader(connection)
    send_request(connection, "GET", "/switch")
    response = read_response(reader)
    check(response["status"] == 101, response)
    check(response["body"] == b"", response)
    check(header_text(response["headers"], "Connection") == ["close"], response)
    check(reader.at_eof(), "101 did not close")
    connection.close()


@run_case
def request_errors(server):
    cases = [
        (b"GET /hello HTTP/1.1\r\n\r\n", 400),
        (b"GET /hello HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\n"
         b"Content-Length: 5\r\n\r\n", 400),
        (b"GET /hello HTTP/1.1\r\nHost: x\r\nBad Name: y\r\n\r\n", 400),
        (b"GET /hello HTTP/1.2\r\nHost: x\r\n\r\n", 505),
        (b"POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip\r\n\r\n", 501),
        (b"GET /" + b"a" * 9000 + b" HTTP/1.1\r\nHost: x\r\n\r\n", 431),
        (b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 2000000\r\n\r\n", 413),
        (b"GET /hello HTTP/1.1\r\nHost: x\r\n" +
         b"".join(b"X-%d: v\r\n" % index for index in range(101)) + b"\r\n", 431),
    ]
    for payload, status in cases:
        connection = server.connect()
        reader = Reader(connection)
        connection.sendall(payload)
        try:
            connection.shutdown(socket.SHUT_WR)
        except OSError:
            pass
        expect_response(reader, status, b"", connection_header="close")
        connection.close()


@run_case
def incomplete_body_closes(server):
    connection = server.connect()
    reader = Reader(connection)
    connection.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nabc")
    connection.shutdown(socket.SHUT_WR)
    check(reader.at_eof(4.0), "server did not close after truncated body")
    connection.close()


@run_case
def large_response(server):
    connection = server.connect()
    reader = Reader(connection)
    size = 4 * 1024 * 1024
    send_request(connection, "GET", "/big/%d" % size)
    response = read_response(reader)
    check(response["status"] == 200, response)
    check(len(response["body"]) == size, len(response["body"]))
    check(response["body"][:1] == b"x" and response["body"][-1:] == b"x", "bad fill")
    send_request(connection, "GET", "/hello")
    expect_response(reader, 200, b"hello world")
    connection.close()


@run_case
def slow_reader_does_not_block(server):
    slow = server.connect(timeout=20)
    slow_reader = Reader(slow)
    slow.sendall(request_bytes("GET", "/big/%d" % (16 * 1024 * 1024)))
    time.sleep(0.3)

    fast = server.connect(timeout=5)
    fast_reader = Reader(fast)
    started = time.monotonic()
    send_request(fast, "GET", "/hello")
    expect_response(fast_reader, 200, b"hello world")
    elapsed = time.monotonic() - started
    check(elapsed < 2.0, "fast request waited %.3fs behind a slow writer" % elapsed)

    response = read_response(slow_reader)
    check(response["status"] == 200, response)
    check(len(response["body"]) == 16 * 1024 * 1024, len(response["body"]))
    fast.close()
    slow.close()


@run_case
def pipeline_fairness(server):
    flood = server.connect(timeout=60)
    flood.settimeout(60)
    count = 50000
    request = request_bytes("GET", "/hello")

    errors = []

    def drain():
        try:
            reader = Reader(flood)
            for _ in range(count):
                expect_response(reader, 200, b"hello world")
        except BaseException as error:
            errors.append(error)

    worker = threading.Thread(target=drain)
    worker.start()
    flood.sendall(request * count)
    time.sleep(0.01)

    fast = server.connect(timeout=10)
    fast_reader = Reader(fast)
    started = time.monotonic()
    send_request(fast, "GET", "/hello")
    expect_response(fast_reader, 200, b"hello world")
    elapsed = time.monotonic() - started
    check(elapsed < 0.8,
          "pipeline flood delayed another connection by %.3fs" % elapsed)
    worker.join(timeout=120)
    check(not worker.is_alive(), "pipeline drain did not finish")
    check(not errors, "pipeline drain failed: %r" % errors[:1])
    fast.close()
    flood.close()


def header_timeout_case(program):
    with ServerProcess(program, header_timeout_ms=150, body_timeout_ms=1000,
                       idle_timeout_ms=1000, timer_tick_ms=25) as server:
        connection = server.connect(timeout=5)
        reader = Reader(connection)
        response = read_response(reader)
        check(response["status"] == 408, response)
        check(header_text(response["headers"], "Connection") == ["close"], response)
        check(reader.at_eof(), "408 did not close")
        connection.close()

        connection = server.connect(timeout=5)
        reader = Reader(connection)
        connection.sendall(b"GET /hello HTTP/1.1\r\nHost:")
        response = read_response(reader)
        check(response["status"] == 408, response)
        connection.close()

        connection = server.connect(timeout=5)
        reader = Reader(connection)
        send_request(connection, "GET", "/hello")
        expect_response(reader, 200, b"hello world")
        connection.close()


def body_timeout_case(program):
    with ServerProcess(program, header_timeout_ms=1000, body_timeout_ms=150,
                       idle_timeout_ms=1000, timer_tick_ms=25) as server:
        connection = server.connect(timeout=5)
        reader = Reader(connection)
        connection.sendall(
            b"POST /echo HTTP/1.1\r\nHost: fixture\r\n"
            b"Content-Length: 5\r\n\r\nab")
        response = read_response(reader)
        check(response["status"] == 408, response)
        check(reader.at_eof(), "body 408 did not close")
        connection.close()

        connection = server.connect(timeout=5)
        reader = Reader(connection)
        send_request(connection, "POST", "/echo", (), b"abcde")
        expect_response(reader, 200, b"abcde")
        connection.close()


def idle_timeout_case(program):
    with ServerProcess(program, header_timeout_ms=1000, body_timeout_ms=1000,
                       idle_timeout_ms=150, timer_tick_ms=25) as server:
        connection = server.connect(timeout=5)
        reader = Reader(connection)
        send_request(connection, "GET", "/hello")
        expect_response(reader, 200, b"hello world")
        check(reader.at_eof(2.0), "idle keep-alive connection stayed open")
        connection.close()


def expect_continue_case(program):
    with ServerProcess(program) as server:
        connection = server.connect(timeout=5)
        reader = Reader(connection)
        connection.sendall(
            b"POST /echo HTTP/1.1\r\nHost: fixture\r\n"
            b"Expect: 100-Continue\r\nContent-Length: 5\r\n\r\n")
        response = read_response(reader)
        check(response["status"] == 100, response)
        check(response["body"] == b"", response)
        connection.sendall(b"abcde")
        expect_response(reader, 200, b"abcde")
        connection.close()

        connection = server.connect(timeout=5)
        reader = Reader(connection)
        connection.sendall(
            b"POST /echo HTTP/1.1\r\nHost: fixture\r\n"
            b"Expect: something-else\r\nContent-Length: 5\r\n\r\n")
        response = read_response(reader)
        check(response["status"] == 417, response)
        check(reader.at_eof(), "417 did not close")
        connection.close()

        connection = server.connect(timeout=5)
        reader = Reader(connection)
        send_request(connection, "GET", "/hello", [("Expect", "100-continue")])
        expect_response(reader, 200, b"hello world")
        connection.close()


def zero_copy_body_case(program):
    with ServerProcess(program, large_body_threshold=0) as server:
        connection = server.connect(timeout=20)
        reader = Reader(connection)
        size = 1024 * 1024
        send_request(connection, "GET", "/big/%d" % size)
        response = read_response(reader)
        check(response["status"] == 200, response)
        check(len(response["body"]) == size, len(response["body"]))
        check(response["body"][:1] == b"x" and response["body"][-1:] == b"x",
              "bad zero-copy fill")
        send_request(connection, "GET", "/hello")
        expect_response(reader, 200, b"hello world")
        connection.close()


def graceful_shutdown_case(program):
    with ServerProcess(program) as server:
        idle = server.connect(timeout=5)
        active = server.connect(timeout=5)
        active_reader = Reader(active)
        send_request(active, "GET", "/hello")
        expect_response(active_reader, 200, b"hello")

        failed = server.connect(timeout=5)
        failed_reader = Reader(failed)
        send_request(failed, "GET", "/error")
        response = expect_response(failed_reader, 500, b"",
                                   connection_header="close")
        check(failed_reader.at_eof(), "handler error connection stayed open")

        slow = server.connect(timeout=5)
        slow.sendall(
            b"POST /echo HTTP/1.1\r\nHost: fixture\r\n"
            b"Content-Length: 5\r\n\r\nab")

        stop = server.connect(timeout=5)
        stop_reader = Reader(stop)
        send_request(stop, "GET", "/stop")
        response = expect_response(stop_reader, 200, b"stopping",
                                   connection_header="close")
        check(stop_reader.at_eof(), "stop connection stayed open")
        check(Reader(idle).at_eof(3.0), "idle connection stayed open")

        try:
            server.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            raise Failure("control server did not exit after drain")
        drained = server.line(2.0)
        stats_line = server.line(2.0)
        check(drained == b"DRAINED true", drained)
        check(stats_line.startswith(b"STATS "), stats_line)
        fields = {}
        for item in stats_line.split()[1:]:
            name, _, value = item.partition(b"=")
            fields[name.decode("ascii")] = value.decode("ascii")
        check(fields.get("active") == "0", fields)
        check(int(fields.get("accepted", "0")) >= 5, fields)
        check(int(fields.get("requests", "0")) >= 3, fields)
        check(int(fields.get("bytes", "0")) > 0, fields)
        check(fields.get("errors") == "1", fields)
        check(fields.get("timeouts") == "1", fields)
        check(fields.get("rejected") == "0", fields)
        check(fields.get("stopping") == "true", fields)
        check(server.process.returncode == 0, server.process.returncode)
        active.close()
        failed.close()
        slow.close()
        stop.close()
        idle.close()


def max_connections_case(program):
    with ServerProcess(program, max_connections=2) as server:
        first = server.connect()
        second = server.connect()
        time.sleep(0.1)
        third = server.connect()
        reader = Reader(third)
        send_request(third, "GET", "/hello")
        response = read_response(reader)
        check(response["status"] == 503, response)
        check(reader.at_eof(), "503 did not close")
        first.close()
        time.sleep(0.1)
        fourth = server.connect()
        fourth_reader = Reader(fourth)
        send_request(fourth, "GET", "/hello")
        expect_response(fourth_reader, 200, b"hello world")
        fourth.close()
        second.close()
        third.close()


def max_requests_case(program):
    with ServerProcess(program, max_requests=3) as server:
        connection = server.connect()
        reader = Reader(connection)
        stream = b"".join(request_bytes("GET", "/hello") for _ in range(4))
        connection.sendall(stream)
        for index in range(3):
            expect_response(reader, 200, b"hello world",
                            connection_header="keep-alive" if index < 2 else "close")
        check(reader.at_eof(), "connection stayed open past max_requests")
        connection.close()


def concurrent_clients_case(program):
    with ServerProcess(program) as server:
        errors = []
        lock = threading.Lock()

        def worker(index):
            try:
                connection = server.connect(timeout=25)
                reader = Reader(connection)
                for step in range(40):
                    target = "/hello" if (index + step) % 3 else "/head"
                    send_request(connection, "GET", target)
                    expected = b"hello world" if target == "/hello" else b"head-body-12345"
                    expect_response(reader, 200, expected)
                connection.close()
            except BaseException as error:
                with lock:
                    errors.append("worker %d: %r" % (index, error))

        threads = [threading.Thread(target=worker, args=(index,)) for index in range(32)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(timeout=60)
        check(not errors, "concurrent failures: %r" % errors[:3])


def benchmark(program):
    with ServerProcess(program) as server:
        request = request_bytes("GET", "/hello")

        connection = server.connect(timeout=30)
        reader = Reader(connection)
        started = time.monotonic()
        for _ in range(2000):
            connection.sendall(request)
            expect_response(reader, 200, b"hello world")
        sequential = 2000 / (time.monotonic() - started)
        connection.close()

        connection = server.connect(timeout=30)
        reader = Reader(connection)
        started = time.monotonic()
        total = 5000
        sent = 0
        while sent < total:
            count = min(64, total - sent)
            connection.sendall(request * count)
            for _ in range(count):
                expect_response(reader, 200, b"hello world")
            sent += count
        pipelined = total / (time.monotonic() - started)
        connection.close()

        results = []
        threads = 8
        lock = threading.Lock()

        def worker():
            local = 0
            try:
                connection = server.connect(timeout=30)
                reader = Reader(connection)
                for _ in range(1000):
                    connection.sendall(request)
                    expect_response(reader, 200, b"hello world")
                    local += 1
                connection.close()
            except BaseException:
                pass
            with lock:
                results.append(local)

        started = time.monotonic()
        workers = [threading.Thread(target=worker) for _ in range(threads)]
        for thread in workers:
            thread.start()
        for thread in workers:
            thread.join(timeout=120)
        elapsed = time.monotonic() - started
        concurrent = sum(results) / elapsed
        check(sum(results) == threads * 1000, "benchmark requests lost")
        return dict(
            sequential_requests_per_second=int(sequential),
            pipelined_requests_per_second=int(pipelined),
            concurrent_8_requests_per_second=int(concurrent))


CASES = [
    closure_handler_state,
    basic_keep_alive,
    binary_and_headers,
    pipeline,
    fragmented,
    chunked_and_trailers,
    no_body_rules,
    connection_policy,
    response_framing,
    request_errors,
    incomplete_body_closes,
    large_response,
    zero_copy_body_case,
    header_timeout_case,
    body_timeout_case,
    idle_timeout_case,
    expect_continue_case,
    slow_reader_does_not_block,
    pipeline_fairness,
    max_connections_case,
    max_requests_case,
    concurrent_clients_case,
]


def record_case(entry, name, function, *args):
    started = time.monotonic()
    try:
        function(*args)
        entry["cases"].append(dict(name=name, status="passed",
                                   seconds=round(time.monotonic() - started, 3)))
        print("  [PASS] %s" % name, flush=True)
        return True
    except BaseException:
        traceback.print_exc()
        entry["cases"].append(dict(name=name, status="failed",
                                   seconds=round(time.monotonic() - started, 3),
                                   detail=traceback.format_exc(limit=4)))
        print("  [FAIL] %s" % name, flush=True)
        return False


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--opt", action="append", type=int, choices=(0, 2))
    parser.add_argument("--benchmark", action="store_true")
    arguments = parser.parse_args()
    opt_levels = arguments.opt or [0, 2]
    cli = Path(arguments.cli).resolve()
    out = Path(arguments.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    environment = environment_for(cli)

    report = dict(schema_version=1, status="failed", cli=str(cli),
                  manifest=str(MANIFEST), control_manifest=str(CONTROL_MANIFEST),
                  optimizations=[], cases=[], benchmark=None)
    for opt in opt_levels:
        build_dir = out / ("native-o%d" % opt)
        entry = dict(opt=opt, build=str(build_dir), cases=[])
        print("building Native O%d" % opt, flush=True)
        program = build(cli, build_dir, opt, environment)
        control_program = build(
            cli, out / ("control-o%d" % opt), opt, environment, CONTROL_MANIFEST)
        for case in CASES:
            record_case(entry, case.__name__, case, program)
        record_case(entry, "graceful_shutdown", graceful_shutdown_case,
                    control_program)
        if arguments.benchmark:
            try:
                entry["benchmark"] = benchmark(program)
                print("  [BENCH] %r" % entry["benchmark"], flush=True)
            except BaseException:
                traceback.print_exc()
                entry["cases"].append(dict(name="benchmark", status="failed",
                                           detail=traceback.format_exc(limit=4)))
                print("  [FAIL] benchmark", flush=True)
        report["optimizations"].append(entry)

    passed = all(case["status"] == "passed"
                 for entry in report["optimizations"] for case in entry["cases"])
    report["status"] = "passed" if passed else "failed"
    report_path = out / "http-server-results.json"
    report_path.write_text(json.dumps(report, indent=1) + "\n")
    if not passed:
        print(json.dumps(dict(status="failed", report=str(report_path))))
        return 1
    print(json.dumps(dict(status="passed",
                          runs=sum(len(entry["cases"]) for entry in report["optimizations"]),
                          report=str(report_path))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
