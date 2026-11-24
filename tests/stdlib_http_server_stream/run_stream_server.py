#!/usr/bin/env python3
"""Build and drive std/http_server stream handlers with raw HTTP/1.1 clients.

Python only starts the isolated compiler and the resulting server, sends raw
bytes, reads HTTP frames, and checks fixture lifecycle lines and stats. It does
not reimplement Woozy codec behavior.
"""

import argparse
import array
import fcntl
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import termios
import threading
import time


ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / "tests/stdlib_http_server_stream/lang.toml"
LOOPBACK = "127.0.0.1"
BACKPRESSURE_TOTAL = 262144 * 64
HOLD_WRITE_BYTES = 33554432
LARGE_FIXED_BYTES = 32768


class Failure(AssertionError):
    pass


def check(condition, message):
    if not condition:
        raise Failure(message)


class LineSource:
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
                    raise Failure("timed out waiting for server output; lines=%r" %
                                  self.lines)
                if self.eof:
                    raise Failure("server stdout ended while waiting; lines=%r" %
                                  self.lines)
                self.condition.wait(remaining)

    def wait_text(self, text, timeout=8.0, after=None):
        return self.wait_for(lambda line: line == text, timeout=timeout, after=after)

    def join(self, timeout=5.0):
        self.thread.join(timeout)
        with self.condition:
            return list(self.lines)


class ServerProcess:
    def __init__(self, program, mode="stream", body_timeout=30000,
                 drain_timeout=1000, tick=20, write_timeout=30000,
                 body_limit=1048576, processing_timeout=30000):
        self.argv = [
            str(program), "0", mode, str(body_timeout), str(drain_timeout),
            str(tick), str(write_timeout), str(body_limit),
            str(processing_timeout),
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

    def finish(self, timeout=10.0):
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
        if stderr:
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
        self.received = 0

    def fill(self):
        chunk = self.connection.recv(65536)
        if not chunk:
            return False
        self.received += len(chunk)
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
                raise Failure("EOF before %r; buffer=%r" %
                              (marker, bytes(self.buffer[:160])))

    def take(self, count):
        while len(self.buffer) < count:
            if not self.fill():
                raise Failure("EOF with %d/%d bytes buffered" %
                              (len(self.buffer), count))
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
        self.received += len(chunk)
        self.buffer.extend(chunk)
        return False


def connect(port, timeout=8.0):
    connection = socket.create_connection((LOOPBACK, port), timeout=timeout)
    connection.settimeout(timeout)
    return connection


def read_until_eof(connection, timeout=5.0):
    """Consume bytes until the peer closes; fail if it stays open."""
    deadline = time.monotonic() + timeout
    total = 0
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise Failure("connection stayed open after %d bytes" % total)
        connection.settimeout(remaining)
        try:
            chunk = connection.recv(65536)
        except (socket.timeout, TimeoutError):
            raise Failure("connection stayed open after %d bytes" % total)
        if not chunk:
            return total
        total += len(chunk)


def read_bytes_until_eof(connection, timeout=5.0, limit=64 * 1024 * 1024):
    """Return every byte up to peer close, with a bounded buffer."""
    deadline = time.monotonic() + timeout
    data = bytearray()
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise Failure("connection stayed open after %d bytes" % len(data))
        connection.settimeout(remaining)
        try:
            chunk = connection.recv(65536)
        except (socket.timeout, TimeoutError):
            raise Failure("connection stayed open after %d bytes" % len(data))
        if not chunk:
            return bytes(data)
        data.extend(chunk)
        if len(data) > limit:
            raise Failure("response exceeded %d bytes" % limit)


def _tcp_inode_for(pid, server_port, client_port, timeout=3.0):
    """Find the accepted socket inode from the public /proc/net tables."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for name in ("tcp", "tcp6"):
            try:
                with open("/proc/%d/net/%s" % (pid, name), "r") as handle:
                    rows = handle.read().splitlines()
            except OSError:
                rows = []
            for row in rows[1:]:
                fields = row.split()
                if len(fields) < 10:
                    continue
                try:
                    local_port = int(fields[1].rsplit(":", 1)[1], 16)
                    remote_port = int(fields[2].rsplit(":", 1)[1], 16)
                except (IndexError, ValueError):
                    continue
                if (local_port == server_port and remote_port == client_port and
                        fields[9] != "0"):
                    return fields[9]
        time.sleep(0.01)
    return None


def server_fd_for(server, connection, timeout=3.0):
    """Map a client connection to the server process's accepted fd via /proc.

    This uses only public kernel tables and the server's own descriptor table;
    no fixture or stdlib test hook exposes the socket fd.
    """
    client_port = connection.getsockname()[1]
    inode = _tcp_inode_for(server.process.pid, server.port, client_port, timeout)
    check(inode is not None,
          "could not find TCP inode for client port %d" % client_port)
    deadline = time.monotonic() + timeout
    fd_dir = "/proc/%d/fd" % server.process.pid
    while time.monotonic() < deadline:
        try:
            entries = os.listdir(fd_dir)
        except OSError:
            entries = []
        for entry in entries:
            try:
                target = os.readlink(os.path.join(fd_dir, entry))
            except OSError:
                continue
            if target == "socket:[%s]" % inode:
                return int(entry), inode
        time.sleep(0.005)
    raise Failure("could not find server fd for socket inode %s" % inode)


def wait_fd_released(server, fd, inode, timeout=3.0):
    """Wait until the process either closed fd or gave it to another socket."""
    deadline = time.monotonic() + timeout
    path = "/proc/%d/fd/%d" % (server.process.pid, fd)
    expected = "socket:[%s]" % inode
    while time.monotonic() < deadline:
        try:
            target = os.readlink(path)
        except OSError:
            return
        if target != expected:
            return
        time.sleep(0.005)
    raise Failure("server fd %d was still the old socket %s" % (fd, inode))


def acquire_reused_connection(server, old_fd, max_attempts=16):
    """Open idle connections until the accepted fd number equals old_fd.

    Earlier non-matching connections stay open so the kernel keeps advancing to
    the next free descriptor instead of reusing the same number.
    """
    held = []
    last = None
    for _ in range(max_attempts):
        connection = connect(server.port)
        fd, inode = server_fd_for(server, connection)
        last = (connection, fd, inode)
        if fd == old_fd:
            return connection, True, fd, inode, held
        held.append(connection)
    return last[0], False, last[1], last[2], held[:-1]


def require_fd_reuse(old_fd, old_inode, matched, reused_fd, reused_inode):
    """Linux must prove the exact fd number and a distinct socket inode."""
    check(matched and reused_fd == old_fd,
          "server fd %r was not reused (expected %d, got %r)" %
          (reused_fd, old_fd, reused_fd))
    check(reused_fd is not None and reused_inode is not None,
          "reused fd/inode was not measured")
    check(reused_inode != old_inode,
          "socket inode %r was reused for fd %d" %
          (reused_inode, old_fd))


def pending_socket_bytes(connection):
    """Return bytes already queued in the client receive buffer."""
    value = array.array("i", [0])
    fcntl.ioctl(connection.fileno(), termios.FIONREAD, value, True)
    return value[0]


def wait_pending_bytes(connection, minimum, timeout=5.0):
    """Wait for a positive buffered-byte observation without consuming it."""
    deadline = time.monotonic() + timeout
    pending = 0
    while time.monotonic() < deadline:
        pending = pending_socket_bytes(connection)
        if pending >= minimum:
            return pending
        time.sleep(0.01)
    raise Failure("only %d bytes were buffered after %.1fs" % (pending, timeout))


def close_connections(connections):
    for connection in connections:
        try:
            connection.close()
        except OSError:
            pass


def send_request(connection, target, method="GET", body=b"", headers=(),
                 version="HTTP/1.1"):
    connection.sendall(request_bytes(target, method, body, headers, version))


def request_bytes(target, method="GET", body=b"", headers=(),
                  version="HTTP/1.1"):
    head = [
        "%s %s %s" % (method, target, version),
        "Host: 127.0.0.1",
        "Content-Length: %d" % len(body),
    ]
    for name, value in headers:
        head.append("%s: %s" % (name, value))
    return ("\r\n".join(head) + "\r\n\r\n").encode("ascii") + body


def send_head(connection, target, method="GET", content_length=0,
              headers=(), version="HTTP/1.1"):
    head = [
        "%s %s %s" % (method, target, version),
        "Host: 127.0.0.1",
        "Content-Length: %d" % content_length,
    ]
    for name, value in headers:
        head.append("%s: %s" % (name, value))
    connection.sendall(("\r\n".join(head) + "\r\n\r\n").encode("ascii"))


def send_chunked_request(connection, target, chunks, trailers=()):
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


def parse_head(reader):
    head = reader.until(b"\r\n\r\n")
    lines = head[:-4].split(b"\r\n")
    parts = lines[0].split(b" ", 2)
    check(len(parts) == 3, "bad status line: %r" % lines[0])
    headers = {}
    for line in lines[1:]:
        check(b":" in line, "bad header line: %r" % line)
        name, value = line.split(b":", 1)
        headers.setdefault(name.strip().lower().decode(), []).append(value.strip())
    return parts[0].decode("ascii"), int(parts[1]), headers


def read_chunked_body(reader):
    body = bytearray()
    trailers = {}
    while True:
        line = reader.until(b"\r\n")
        size_text = line[:-2].split(b";", 1)[0]
        size = int(size_text, 16)
        if size == 0:
            while True:
                trailer_line = reader.until(b"\r\n")
                if trailer_line == b"\r\n":
                    return bytes(body), trailers
                name, _, value = trailer_line[:-2].partition(b":")
                trailers[name.strip().lower().decode()] = value.strip()
        body.extend(reader.take(size))
        check(reader.take(2) == b"\r\n", "bad chunk terminator")
        if len(body) > 128 * 1024 * 1024:
            raise Failure("chunked response exceeded test bound")


def read_response(reader, method="GET"):
    version, status, headers = parse_head(reader)
    no_body = method == "HEAD" or 100 <= status < 200 or status in (204, 304)
    trailers = {}
    if no_body:
        body = b""
    elif any(b"chunked" in value.lower()
             for value in headers.get("transfer-encoding", [])):
        body, trailers = read_chunked_body(reader)
    else:
        lengths = headers.get("content-length", [])
        check(lengths, "response lacks Content-Length: %r" % headers)
        body = reader.take(int(lengths[0]))
    return {
        "version": version,
        "status": status,
        "headers": headers,
        "body": body,
        "trailers": trailers,
    }


def read_chunk_frame(reader):
    line = reader.until(b"\r\n")
    size = int(line[:-2].split(b";", 1)[0], 16)
    if size == 0:
        trailers = {}
        while True:
            trailer_line = reader.until(b"\r\n")
            if trailer_line == b"\r\n":
                return b"", trailers, True
            name, _, value = trailer_line[:-2].partition(b":")
            trailers[name.strip().lower().decode()] = value.strip()
    data = reader.take(size)
    check(reader.take(2) == b"\r\n", "bad chunk terminator")
    return data, {}, False


def parse_partial_chunked_response(data):
    """Parse a chunked response that ended before its first chunk completed."""
    split = data.find(b"\r\n\r\n")
    check(split >= 0, "response head is incomplete: %r" % data[:160])
    head = data[:split + 4]
    lines = head[:-4].split(b"\r\n")
    parts = lines[0].split(b" ", 2)
    check(len(parts) == 3 and parts[1] == b"200",
          "partial response status: %r" % lines[0])
    headers = {}
    for line in lines[1:]:
        check(b":" in line, "bad partial header line: %r" % line)
        name, value = line.split(b":", 1)
        headers.setdefault(name.strip().lower().decode(), []).append(value.strip())
    check(any(b"chunked" in value.lower()
              for value in headers.get("transfer-encoding", [])),
          "partial response is not chunked: %r" % headers)
    body = data[split + 4:]
    line_end = body.find(b"\r\n")
    check(line_end > 0, "missing chunk size line: %r" % body[:160])
    size_text = body[:line_end].split(b";", 1)[0]
    try:
        size = int(size_text, 16)
    except ValueError:
        raise Failure("bad chunk size %r" % size_text)
    payload = body[line_end + 2:]
    return {"head": head, "headers": headers, "chunk_size": size,
            "payload": payload}


def parse_stats(lines):
    for line in lines:
        if line.startswith("STATS "):
            fields = {}
            for item in line[6:].split():
                name, value = item.split("=", 1)
                fields[name] = value
            return fields
    raise Failure("STATS line missing; lines=%r" % lines)


def check_drained(lines):
    check("DRAINED true" in lines, "graceful drain did not finish: %r" % lines)
    stats = parse_stats(lines)
    check(stats.get("active") == "0", "active connections remain: %r" % stats)
    check(stats.get("stopping") == "true", "stopping flag not set: %r" % stats)
    return stats


def require_order(lines, first, second):
    check(first in lines and second in lines,
          "missing lifecycle line: %r in %r" % ((first, second), lines))
    check(lines.index(first) < lines.index(second),
          "wrong lifecycle order: %r before %r" % (first, second))


def scenario_fixed_and_framing(program, opt):
    server = ServerProcess(program)
    try:
        connection = connect(server.port)
        reader = Reader(connection)
        send_request(connection, "/fixed")
        response = read_response(reader)
        check((response["status"], response["body"]) == (200, b"fixed"),
              "fixed response: %r" % response)
        check(response["headers"].get("connection") == [b"keep-alive"],
              "fixed keep-alive: %r" % response)
        send_request(connection, "/fast")
        response = read_response(reader)
        check((response["status"], response["body"]) == (200, b"fast"),
              "keep-alive response: %r" % response)
        connection.close()

        connection = connect(server.port)
        reader = Reader(connection)
        send_request(connection, "/head", method="HEAD")
        response = read_response(reader, method="HEAD")
        check(response["status"] == 200 and response["body"] == b"",
              "HEAD response: %r" % response)
        check(response["headers"].get("content-length") == [b"9"],
              "HEAD representation length: %r" % response)
        connection.close()

        connection = connect(server.port)
        reader = Reader(connection)
        send_request(connection, "/no-content")
        response = read_response(reader)
        check(response["status"] == 204 and response["body"] == b"",
              "204 response: %r" % response)
        connection.close()

        connection = connect(server.port)
        reader = Reader(connection)
        send_request(connection, "/fixed", version="HTTP/1.0")
        response = read_response(reader)
        check(response["version"] == "HTTP/1.0" and response["status"] == 200,
              "HTTP/1.0 response: %r" % response)
        check(response["body"] == b"fixed", "HTTP/1.0 body: %r" % response)
        check(response["headers"].get("connection") == [b"close"],
              "HTTP/1.0 connection policy: %r" % response)
        check(reader.at_eof(), "HTTP/1.0 connection stayed open")
        connection.close()

        connection = connect(server.port)
        reader = Reader(connection)
        send_request(connection, "/echo-chunked", version="HTTP/1.0")
        response = read_response(reader)
        check(response["status"] == 505, "HTTP/1.0 chunked rejection: %r" % response)
        check(reader.at_eof(), "HTTP/1.0 chunked rejection stayed open")
        connection.close()

        connection = connect(server.port)
        reader = Reader(connection)
        send_request(connection, "/fail")
        response = read_response(reader)
        check(response["status"] == 500 and response["body"] == b"",
              "handler error response: %r" % response)
        check(reader.at_eof(), "handler error connection stayed open")
        connection.close()

        connection = connect(server.port)
        reader = Reader(connection)
        send_request(connection, "/commit-error")
        response = read_response(reader)
        check((response["status"], response["body"]) == (200, b"partial"),
              "committed error response: %r" % response)
        check(reader.at_eof(), "committed error appended a second response")
        connection.close()

        stop = connect(server.port)
        stop_reader = Reader(stop)
        send_request(stop, "/stop", headers=[("Connection", "close")])
        response = read_response(stop_reader)
        check(response["status"] == 200, "stop response: %r" % response)
        stop.close()

        status, lines, stderr = server.finish()
        check(status == 0, "server exit %d; lines=%r stderr=%r" %
              (status, lines, stderr))
        stats = check_drained(lines)
        # Seven completed handler responses. The generated 500 and 505 below
        # are failures, not completed handler requests.
        check(int(stats["requests"]) == 7,
              "completed response counter: %r" % stats)
        check(int(stats["errors"]) >= 3, "error counter too small: %r" % stats)
        check(int(stats["timeouts"]) == 0, "unexpected timeout: %r" % stats)
        return {"scenario": "fixed-framing-errors", "opt": opt, "stats": stats}
    finally:
        server.close()


def read_closed_response(reader, method="GET"):
    response = read_response(reader, method)
    check(reader.at_eof(timeout=2.0), "connection stayed open after response")
    check(not reader.buffer, "unexpected bytes after response: %r" %
          bytes(reader.buffer[:160]))
    return response, reader.received


def scenario_large_fixed(program, opt):
    """A fixed response over the large-body threshold sends head and body."""
    server = ServerProcess(program, tick=20)
    try:
        connection = connect(server.port)
        reader = Reader(connection)
        send_request(connection, "/large-fixed")
        response = read_response(reader)
        check((response["status"], len(response["body"])) ==
              (200, LARGE_FIXED_BYTES),
              "large fixed response: status=%r bytes=%d" %
              (response["status"], len(response["body"])))
        check(response["body"] == b"A" * LARGE_FIXED_BYTES,
              "large fixed body bytes differ")
        send_request(connection, "/fast")
        response = read_response(reader)
        check(response["body"] == b"fast",
              "large fixed keep-alive response: %r" % response)
        send_request(connection, "/stop", headers=[("Connection", "close")])
        response = read_response(reader)
        check(response["status"] == 200, "large fixed stop: %r" % response)
        connection.close()
        status, lines, stderr = server.finish(timeout=8)
        check(status == 0, "large-fixed server exit %d; lines=%r stderr=%r" %
              (status, lines, stderr))
        stats = check_drained(lines)
        check(int(stats["requests"]) == 3,
              "large fixed request count: %r" % stats)
        check(int(stats["errors"]) == 0,
              "large fixed counted an error: %r" % stats)
        check(int(stats["timeouts"]) == 0,
              "large fixed counted a timeout: %r" % stats)
        return {"scenario": "large-fixed-single-write", "opt": opt,
                "stats": stats, "body_bytes": LARGE_FIXED_BYTES}
    finally:
        server.close()


def scenario_chunked_connection_close(program, opt):
    """An explicit response Connection: close closes a chunked response."""
    server = ServerProcess(program, tick=20)
    try:
        connection = connect(server.port)
        reader = Reader(connection)
        send_request(connection, "/chunked-close")
        # The followup is already on the wire when the first response closes.
        send_request(connection, "/fast")
        response, chunked_bytes = read_closed_response(reader)
        check((response["status"], response["body"]) == (200, b"closing"),
              "chunked close response: %r" % response)
        check(response["headers"].get("connection") == [b"close"],
              "chunked close header: %r" % response["headers"])
        connection.close()

        stop = connect(server.port)
        stop_reader = Reader(stop)
        send_request(stop, "/stop", headers=[("Connection", "close")])
        stop_response, stop_bytes = read_closed_response(stop_reader)
        check(stop_response["status"] == 200, "stop response: %r" % stop_response)
        stop.close()

        status, lines, stderr = server.finish(timeout=8)
        check(status == 0, "chunked-close server exit %d; lines=%r stderr=%r" %
              (status, lines, stderr))
        stats = check_drained(lines)
        check(int(stats["requests"]) == 2,
              "chunked close request count: %r" % stats)
        check(int(stats["errors"]) == 0,
              "chunked close counted an error: %r" % stats)
        check(int(stats["timeouts"]) == 0,
              "chunked close counted a timeout: %r" % stats)
        check(int(stats["bytes"]) == chunked_bytes + stop_bytes,
              "chunked close byte count: %r != %d" %
              (stats, chunked_bytes + stop_bytes))
        return {"scenario": "chunked-connection-close", "opt": opt,
                "stats": stats, "chunked_response_bytes": chunked_bytes,
                "pipelined_second_response": False}
    finally:
        server.close()


def scenario_failure_stats_exact(program, opt):
    """One success plus handler/protocol failures keep exact stats."""
    server = ServerProcess(program, tick=20)
    try:
        total = 0

        connection = connect(server.port)
        reader = Reader(connection)
        send_request(connection, "/fixed",
                     headers=[("Connection", "close")])
        response, sent = read_closed_response(reader)
        check((response["status"], response["body"]) == (200, b"fixed"),
              "exact-stats success response: %r" % response)
        total += sent
        connection.close()

        connection = connect(server.port)
        reader = Reader(connection)
        send_request(connection, "/fail")
        response, sent = read_closed_response(reader)
        check((response["status"], response["body"]) == (500, b""),
              "exact-stats handler failure response: %r" % response)
        total += sent
        connection.close()

        connection = connect(server.port)
        reader = Reader(connection)
        send_request(connection, "/echo-chunked", version="HTTP/1.0")
        response, sent = read_closed_response(reader)
        check((response["status"], response["body"]) == (505, b""),
              "exact-stats protocol failure response: %r" % response)
        total += sent
        connection.close()

        stop = connect(server.port)
        stop_reader = Reader(stop)
        send_request(stop, "/stop", headers=[("Connection", "close")])
        response, sent = read_closed_response(stop_reader)
        check(response["status"] == 200, "exact-stats stop response: %r" % response)
        total += sent
        stop.close()

        status, lines, stderr = server.finish(timeout=8)
        check(status == 0, "failure-stats server exit %d; lines=%r stderr=%r" %
              (status, lines, stderr))
        stats = check_drained(lines)
        check(int(stats["requests"]) == 2,
              "generated failures counted as requests: %r" % stats)
        check(int(stats["errors"]) == 2,
              "generated failure error count: %r" % stats)
        check(int(stats["timeouts"]) == 0,
              "generated failure counted a timeout: %r" % stats)
        check(int(stats["rejected"]) == 0,
              "generated failure counted a capacity rejection: %r" % stats)
        check(int(stats["bytes"]) == total,
              "generated failure byte count: %r != %d" % (stats, total))
        return {"scenario": "failure-stats-exact", "opt": opt,
                "stats": stats, "success_and_failure_bytes": total,
                "generated_rejections": 2}
    finally:
        server.close()


def scenario_alias_complete_no_error(program, opt):
    """A pending alias after a complete response alone is not an error."""
    server = ServerProcess(program, tick=20)
    try:
        connection = connect(server.port)
        reader = Reader(connection)
        send_head(connection, "/alias-read-after-complete", method="POST",
                  content_length=10, headers=[("Connection", "close")])
        server.lines.wait_text("ALIAS-COMPLETE-READ-START", timeout=3)
        server.lines.wait_text("ALIAS-COMPLETE-HANDLER-DONE", timeout=3)
        response, alias_bytes = read_closed_response(reader)
        check((response["status"], response["body"]) == (200, b"complete"),
              "alias-complete response: %r" % response)
        check(response["headers"].get("connection") == [b"close"],
              "alias-complete close header: %r" % response["headers"])
        server.lines.wait_text(
            "ALIAS-COMPLETE-READ-REJECTED ReadOrder", timeout=3)
        check("ALIAS-COMPLETE-READ-AFTER" not in server.lines.lines,
              "alias read resumed after completion: %r" %
              server.lines.lines)
        check("ALIAS-COMPLETE-READ-REJECTED Other" not in server.lines.lines,
              "alias read left with a non-ReadOrder error: %r" %
              server.lines.lines)
        connection.close()

        stop = connect(server.port)
        stop_reader = Reader(stop)
        send_request(stop, "/stop", headers=[("Connection", "close")])
        response, stop_bytes = read_closed_response(stop_reader)
        check(response["status"] == 200, "alias-complete stop: %r" % response)
        stop.close()

        status, lines, stderr = server.finish(timeout=8)
        check(status == 0, "alias-complete server exit %d; lines=%r stderr=%r" %
              (status, lines, stderr))
        stats = check_drained(lines)
        check(int(stats["requests"]) == 2,
              "alias-complete request count: %r" % stats)
        check(int(stats["errors"]) == 0,
              "pending alias after complete response counted an error: %r" %
              stats)
        check(int(stats["timeouts"]) == 0,
              "alias-complete counted a timeout: %r" % stats)
        check(int(stats["bytes"]) == alias_bytes + stop_bytes,
              "alias-complete byte count: %r != %d" %
              (stats, alias_bytes + stop_bytes))
        return {"scenario": "stream-alias-complete-no-error", "opt": opt,
                "stats": stats, "response_bytes": alias_bytes}
    finally:
        server.close()


def scenario_body_early(program, opt):
    server = ServerProcess(program)
    try:
        connection = connect(server.port)
        reader = Reader(connection)
        send_head(connection, "/read-early", method="POST", content_length=10)
        connection.sendall(b"abcd")
        server.lines.wait_text("BODY-PART /read-early 4 4", timeout=3)
        connection.sendall(b"efghij")
        response = read_response(reader)
        check((response["status"], response["body"]) == (200, b"done"),
              "early body response: %r" % response)
        send_request(connection, "/fixed")
        response = read_response(reader)
        check(response["body"] == b"fixed", "early-body keep-alive response: %r" % response)
        connection.close()

        connection = connect(server.port)
        reader = Reader(connection)
        started = time.monotonic()
        send_request(connection, "/stream-early")
        version, status, headers = parse_head(reader)
        check(status == 200, "streamed head: %r" % status)
        first, trailers, done = read_chunk_frame(reader)
        first_at = time.monotonic() - started
        check(first == b"first" and not done, "first streamed chunk: %r/%r" % (first, done))
        check(first_at < 0.35, "first response chunk was delayed: %.3fs" % first_at)
        server.lines.wait_text("STREAM-FIRST /stream-early", timeout=3)

        fast = connect(server.port)
        fast_reader = Reader(fast)
        fast_started = time.monotonic()
        send_request(fast, "/fast")
        response = read_response(fast_reader)
        fast_latency = time.monotonic() - fast_started
        check(response["body"] == b"fast", "unrelated fast response: %r" % response)
        check(fast_latency < 0.35,
              "unrelated connection blocked by stream handler: %.3fs" % fast_latency)
        fast.close()

        body = bytearray(first)
        final_trailers = {}
        while True:
            chunk, chunk_trailers, done = read_chunk_frame(reader)
            body.extend(chunk)
            if done:
                final_trailers = chunk_trailers
                break
        check(bytes(body) == b"firstsecond", "streamed body: %r" % bytes(body))
        check(final_trailers.get("x-trailer") == b"done",
              "streamed trailers: %r" % final_trailers)
        connection.close()

        pipelined = connect(server.port)
        pipeline_reader = Reader(pipelined)
        started = time.monotonic()
        send_request(pipelined, "/stream-early")
        send_request(pipelined, "/fast")
        first = read_response(pipeline_reader)
        pipeline_latency = time.monotonic() - started
        check(first["body"] == b"firstsecond", "pipeline first response: %r" % first)
        check(pipeline_latency >= 0.35,
              "pipelined request ran before first stream finished: %.3fs" % pipeline_latency)
        second = read_response(pipeline_reader)
        check(second["body"] == b"fast", "pipeline second response: %r" % second)
        pipelined.close()

        stop = connect(server.port)
        stop_reader = Reader(stop)
        send_request(stop, "/stop", headers=[("Connection", "close")])
        read_response(stop_reader)
        stop.close()
        status, lines, stderr = server.finish()
        check(status == 0, "server exit %d; lines=%r stderr=%r" %
              (status, lines, stderr))
        stats = check_drained(lines)
        check(int(stats["timeouts"]) == 0, "unexpected timeout: %r" % stats)
        return {"scenario": "early-body-early-response", "opt": opt, "stats": stats}
    finally:
        server.close()


def scenario_stop_graceful(program, opt):
    server = ServerProcess(program, drain_timeout=1000, tick=20)
    try:
        slow = connect(server.port)
        slow_reader = Reader(slow)
        send_request(slow, "/stream-early")
        server.lines.wait_text("STREAM-FIRST /stream-early", timeout=3)
        stop = connect(server.port)
        stop_reader = Reader(stop)
        send_request(stop, "/stop", headers=[("Connection", "close")])
        read_response(stop_reader)
        response = read_response(slow_reader)
        check(response["body"] == b"firstsecond",
              "in-flight stream response after stop: %r" % response)
        check(response["trailers"].get("x-trailer") == b"done",
              "in-flight stream trailer after stop: %r" % response)
        slow.close()
        stop.close()
        status, lines, stderr = server.finish(timeout=8)
        check(status == 0, "graceful stop server exit %d; lines=%r stderr=%r" %
              (status, lines, stderr))
        stats = check_drained(lines)
        check(int(stats["requests"]) >= 2, "in-flight stream response not counted: %r" % stats)
        check(int(stats["timeouts"]) == 0, "graceful stream stop timed out: %r" % stats)
        return {"scenario": "stop-allows-stream-finish", "opt": opt, "stats": stats}
    finally:
        server.close()


def scenario_chunked_echo(program, opt):
    server = ServerProcess(program)
    try:
        connection = connect(server.port)
        reader = Reader(connection)
        send_request(connection, "/echo-chunked")
        response = read_response(reader)
        check(response["body"] == b"", "empty chunked echo: %r" % response)
        check(response["trailers"].get("x-request-trailers") == b"0",
              "empty chunked trailers: %r" % response)
        send_request(connection, "/fixed")
        response = read_response(reader)
        check(response["body"] == b"fixed", "chunked keep-alive response: %r" % response)
        connection.close()

        connection = connect(server.port)
        reader = Reader(connection)
        send_chunked_request(connection, "/echo-chunked",
                             [b"hello ", b"world"], [("X-T", "v")])
        response = read_response(reader)
        check(response["body"] == b"hello world", "chunked echo body: %r" % response)
        check(response["trailers"].get("x-request-trailers") == b"1",
              "chunked echo trailers: %r" % response)
        check(reader.at_eof(), "early chunked response should close the connection")
        connection.close()

        connection = connect(server.port)
        reader = Reader(connection)
        send_chunked_request(connection, "/buffered-echo",
                             [b"abc", b"def"], [("X-T", "v")])
        response = read_response(reader)
        check(response["body"] == b"abcdef", "buffered echo body: %r" % response)
        check(response["trailers"].get("x-request-trailers") == b"1",
              "buffered echo trailers: %r" % response)
        send_request(connection, "/fixed")
        response = read_response(reader)
        check(response["body"] == b"fixed", "buffered echo keep-alive: %r" % response)
        connection.close()

        connection = connect(server.port)
        reader = Reader(connection)
        send_head(connection, "/buffered-echo", method="POST", content_length=5,
                  headers=[("Expect", "100-continue")])
        response = read_response(reader)
        check(response["status"] == 100, "100 Continue response: %r" % response)
        connection.sendall(b"hello")
        response = read_response(reader)
        check(response["body"] == b"hello", "Expect echo body: %r" % response)
        send_request(connection, "/fixed")
        response = read_response(reader)
        check(response["body"] == b"fixed", "Expect keep-alive: %r" % response)
        connection.close()

        stop = connect(server.port)
        stop_reader = Reader(stop)
        send_request(stop, "/stop", headers=[("Connection", "close")])
        read_response(stop_reader)
        stop.close()
        status, lines, stderr = server.finish()
        check(status == 0, "server exit %d; lines=%r stderr=%r" %
              (status, lines, stderr))
        stats = check_drained(lines)
        check(int(stats["timeouts"]) == 0, "unexpected timeout: %r" % stats)
        return {"scenario": "chunked-echo-expect", "opt": opt, "stats": stats}
    finally:
        server.close()


def scenario_limits(program, opt):
    server = ServerProcess(program, body_limit=1024)
    try:
        connection = connect(server.port)
        reader = Reader(connection)
        send_chunked_request(connection, "/too-large", [b"z" * 2048])
        response = read_response(reader)
        check(response["status"] == 413, "chunked body limit response: %r" % response)
        check(reader.at_eof(), "body-limit connection stayed open")
        connection.close()

        connection = connect(server.port)
        reader = Reader(connection)
        send_request(connection, "/too-large", method="POST", body=b"y" * 2048)
        response = read_response(reader)
        check(response["status"] == 413, "fixed body limit response: %r" % response)
        check(reader.at_eof(), "fixed body-limit connection stayed open")
        connection.close()

        stop = connect(server.port)
        stop_reader = Reader(stop)
        send_request(stop, "/stop", headers=[("Connection", "close")])
        read_response(stop_reader)
        stop.close()
        status, lines, stderr = server.finish()
        check(status == 0, "server exit %d; lines=%r stderr=%r" %
              (status, lines, stderr))
        stats = check_drained(lines)
        check(int(stats["errors"]) >= 2, "body limit errors not counted: %r" % stats)
        return {"scenario": "body-limit", "opt": opt, "stats": stats}
    finally:
        server.close()


def scenario_stale_duplicate(program, opt):
    server = ServerProcess(program)
    try:
        connection = connect(server.port)
        reader = Reader(connection)
        send_request(connection, "/stale")
        response = read_response(reader)
        check(response["body"] == b"stale-base", "stale base response: %r" % response)
        server.lines.wait_text("STALE-REJECTED", timeout=3)
        send_request(connection, "/fixed")
        response = read_response(reader)
        check(response["body"] == b"fixed", "connection after stale rejection: %r" % response)
        connection.close()

        connection = connect(server.port)
        reader = Reader(connection)
        send_request(connection, "/duplicate")
        response = read_response(reader)
        check(response["body"] == b"a", "duplicate route body: %r" % response)
        server.lines.wait_text("DUP-REJECTED 3", timeout=3)
        send_request(connection, "/fixed")
        response = read_response(reader)
        check(response["body"] == b"fixed", "connection after duplicate rejection: %r" % response)
        connection.close()

        stop = connect(server.port)
        stop_reader = Reader(stop)
        send_request(stop, "/stop", headers=[("Connection", "close")])
        read_response(stop_reader)
        stop.close()
        status, lines, stderr = server.finish()
        check(status == 0, "server exit %d; lines=%r stderr=%r" %
              (status, lines, stderr))
        stats = check_drained(lines)
        return {"scenario": "stale-duplicate-rejection", "opt": opt, "stats": stats}
    finally:
        server.close()


def scenario_backpressure(program, opt):
    server = ServerProcess(program)
    try:
        slow = connect(server.port)
        slow_reader = Reader(slow)
        send_request(slow, "/backpressure")
        server.lines.wait_text("BP-START /backpressure", timeout=3)
        time.sleep(0.1)

        fast = connect(server.port)
        fast_reader = Reader(fast)
        started = time.monotonic()
        send_request(fast, "/fast")
        response = read_response(fast_reader)
        latency = time.monotonic() - started
        check(response["body"] == b"fast", "backpressure fast response: %r" % response)
        check(latency < 0.5,
              "unrelated connection blocked by backpressure: %.3fs" % latency)
        fast.close()

        response = read_response(slow_reader)
        check(len(response["body"]) == BACKPRESSURE_TOTAL,
              "backpressure body length: %d" % len(response["body"]))
        check(response["body"] == b"x" * BACKPRESSURE_TOTAL,
              "backpressure body bytes differ")
        check(response["trailers"].get("x-backpressure") == b"done",
              "backpressure trailers: %r" % response["trailers"])
        slow.close()

        stop = connect(server.port)
        stop_reader = Reader(stop)
        send_request(stop, "/stop", headers=[("Connection", "close")])
        read_response(stop_reader)
        stop.close()
        status, lines, stderr = server.finish()
        check(status == 0, "server exit %d; lines=%r stderr=%r" %
              (status, lines, stderr))
        stats = check_drained(lines)
        check("BP-DONE /backpressure" in lines, "backpressure completion missing")
        return {"scenario": "backpressure", "opt": opt, "stats": stats,
                "latency_ms": round(latency * 1000)}
    finally:
        server.close()


def scenario_stop_abort(program, opt):
    server = ServerProcess(program, drain_timeout=250, tick=20)
    try:
        hold = connect(server.port)
        send_request(hold, "/hold")
        server.lines.wait_text("HOLD-START /hold", timeout=3)
        stop = connect(server.port)
        send_request(stop, "/stop", headers=[("Connection", "close")])
        try:
            read_response(Reader(stop))
        except Failure:
            pass
        status, lines, stderr = server.finish(timeout=8)
        check(status == 0, "stop server exit %d; lines=%r stderr=%r" %
              (status, lines, stderr))
        check("HOLD-CLEANUP /hold" in lines, "stop cleanup missing: %r" % lines)
        check("HOLD-AFTER /hold" not in lines, "stopped handler kept running: %r" % lines)
        require_order(lines, "HOLD-START /hold", "HOLD-CLEANUP /hold")
        stats = check_drained(lines)
        check(int(stats["timeouts"]) >= 1, "drain timeout not counted: %r" % stats)
        hold.close()
        stop.close()
        stop_stats = stats
    finally:
        server.close()

    server = ServerProcess(program, tick=20)
    try:
        hold = connect(server.port)
        send_request(hold, "/hold")
        server.lines.wait_text("HOLD-START /hold", timeout=3)
        abort = connect(server.port)
        send_request(abort, "/abort", headers=[("Connection", "close")])
        try:
            read_response(Reader(abort))
        except Failure:
            pass
        status, lines, stderr = server.finish(timeout=8)
        check(status == 0, "abort server exit %d; lines=%r stderr=%r" %
              (status, lines, stderr))
        check("HOLD-CLEANUP /hold" in lines, "abort cleanup missing: %r" % lines)
        check("HOLD-AFTER /hold" not in lines, "aborted handler kept running: %r" % lines)
        stats = check_drained(lines)
        check(int(stats["timeouts"]) == 0, "abort counted a timeout: %r" % stats)
        hold.close()
        abort.close()
        return {"scenario": "stop-abort-cancellation", "opt": opt,
                "stop_stats": stop_stats, "abort_stats": stats}
    finally:
        server.close()


def scenario_body_timeout(program, opt):
    server = ServerProcess(program, body_timeout=150, tick=20)
    try:
        hold = connect(server.port)
        reader = Reader(hold)
        send_head(hold, "/hold-read", method="POST", content_length=10)
        hold.sendall(b"abc")
        server.lines.wait_text("HOLD-READ-START /hold-read", timeout=3)
        server.lines.wait_text("HOLD-READ-CLEANUP /hold-read", timeout=4)
        check("HOLD-READ-AFTER /hold-read" not in server.lines.lines,
              "timed-out reader kept running: %r" % server.lines.lines)
        check(reader.at_eof(timeout=3), "body-timeout connection stayed open")
        hold.close()

        stop = connect(server.port)
        stop_reader = Reader(stop)
        send_request(stop, "/stop", headers=[("Connection", "close")])
        read_response(stop_reader)
        stop.close()
        status, lines, stderr = server.finish(timeout=8)
        check(status == 0, "body-timeout server exit %d; lines=%r stderr=%r" %
              (status, lines, stderr))
        stats = check_drained(lines)
        check(int(stats["timeouts"]) >= 1, "body timeout not counted: %r" % stats)
        return {"scenario": "body-read-timeout", "opt": opt, "stats": stats}
    finally:
        server.close()


def scenario_processing_timeout(program, opt):
    server = ServerProcess(
        program, body_timeout=2000, drain_timeout=1000, tick=20,
        write_timeout=30000, processing_timeout=150)
    try:
        connection = connect(server.port)
        reader = Reader(connection)
        send_head(connection, "/body-then-hold-proc", method="POST",
                  content_length=10)
        server.lines.wait_text("HOLD-PROC-START /body-then-hold-proc", timeout=3)
        for index in range(5):
            connection.sendall(bytes([97 + index, 98 + index]))
            time.sleep(0.08)
        server.lines.wait_text(
            "HOLD-PROC-BODY-DONE /body-then-hold-proc", timeout=3)
        server.lines.wait_text(
            "HOLD-PROC-CLEANUP /body-then-hold-proc", timeout=3)
        check("HOLD-PROC-AFTER /body-then-hold-proc" not in server.lines.lines,
              "processing timeout did not stop handler: %r" % server.lines.lines)
        check(reader.at_eof(timeout=3), "processing-timeout connection stayed open")
        connection.close()

        stop = connect(server.port)
        stop_reader = Reader(stop)
        send_request(stop, "/stop", headers=[("Connection", "close")])
        read_response(stop_reader)
        stop.close()
        status, lines, stderr = server.finish(timeout=8)
        check(status == 0, "processing-timeout server exit %d; lines=%r stderr=%r" %
              (status, lines, stderr))
        stats = check_drained(lines)
        check(int(stats["timeouts"]) >= 1,
              "processing timeout not counted: %r" % stats)
        require_order(lines, "HOLD-PROC-START /body-then-hold-proc",
                      "HOLD-PROC-BODY-DONE /body-then-hold-proc")
        require_order(lines, "HOLD-PROC-BODY-DONE /body-then-hold-proc",
                      "HOLD-PROC-CLEANUP /body-then-hold-proc")
        return {"scenario": "stream-read-processing-clocks", "opt": opt,
                "stats": stats}
    finally:
        server.close()


def scenario_processing_gaps(program, opt):
    """Each gap is under the processing clock; their sum is over it."""
    server = ServerProcess(program, body_timeout=2000, tick=20,
                           processing_timeout=250)
    try:
        connection = connect(server.port)
        reader = Reader(connection)
        send_request(connection, "/processing-gaps", method="POST",
                     body=b"abc")
        server.lines.wait_text("PROC-GAPS-DONE /processing-gaps", timeout=3)
        response = read_response(reader)
        check((response["status"], response["body"]) == (200, b"gaps"),
              "multi-gap response: %r" % response)
        send_request(connection, "/stop", headers=[("Connection", "close")])
        response = read_response(reader)
        check(response["status"] == 200, "multi-gap stop: %r" % response)
        connection.close()
        status, lines, stderr = server.finish(timeout=8)
        check(status == 0, "processing-gaps server exit %d; lines=%r stderr=%r" %
              (status, lines, stderr))
        stats = check_drained(lines)
        check(int(stats["timeouts"]) == 0,
              "per-gap processing clock expired: %r" % stats)
        check(int(stats["requests"]) == 2,
              "processing-gaps request count: %r" % stats)
        return {"scenario": "stream-processing-gaps", "opt": opt,
                "stats": stats}
    finally:
        server.close()


def scenario_processing_single_gap(program, opt):
    """One gap over the processing clock still ends the handler."""
    server = ServerProcess(program, body_timeout=2000, tick=20,
                           processing_timeout=250)
    try:
        hold = connect(server.port)
        send_request(hold, "/processing-single-gap", method="POST",
                     body=b"ab")
        server.lines.wait_text("PROC-SINGLE-START /processing-single-gap",
                               timeout=3)
        server.lines.wait_text("PROC-SINGLE-CLEANUP /processing-single-gap",
                               timeout=4)
        check("PROC-SINGLE-AFTER /processing-single-gap" not in
              server.lines.lines,
              "overlong processing gap kept running: %r" % server.lines.lines)
        check(read_bytes_until_eof(hold, timeout=3) == b"",
              "overlong processing gap wrote a response")
        hold.close()

        stop = connect(server.port)
        stop_reader = Reader(stop)
        send_request(stop, "/stop", headers=[("Connection", "close")])
        read_response(stop_reader)
        stop.close()
        status, lines, stderr = server.finish(timeout=8)
        check(status == 0, "single-gap server exit %d; lines=%r stderr=%r" %
              (status, lines, stderr))
        stats = check_drained(lines)
        check(int(stats["timeouts"]) >= 1,
              "overlong processing gap was not timed out: %r" % stats)
        return {"scenario": "stream-processing-single-gap", "opt": opt,
                "stats": stats}
    finally:
        server.close()


def scenario_read_per_call(program, opt):
    """Each read gets its own deadline through one split chunk header."""
    server = ServerProcess(program, body_timeout=300, tick=20,
                           processing_timeout=2000)
    try:
        connection = connect(server.port)
        reader = Reader(connection)
        connection.sendall(
            b"POST /read-early HTTP/1.1\r\nHost: 127.0.0.1\r\n"
            b"Transfer-Encoding: chunked\r\n\r\n")
        connection.sendall(b"5\r")
        time.sleep(0.1)
        connection.sendall(b"\nhello\r\n")
        server.lines.wait_text("BODY-PART /read-early 5 5", timeout=3)
        time.sleep(0.1)
        connection.sendall(b"6\r")
        time.sleep(0.1)
        connection.sendall(b"\nworld!\r\n")
        server.lines.wait_text("BODY-PART /read-early 6 11", timeout=3)
        time.sleep(0.1)
        connection.sendall(b"0\r\n\r\n")
        response = read_response(reader)
        check((response["status"], response["body"]) == (200, b"done"),
              "per-call read response: %r" % response)
        send_request(connection, "/stop", headers=[("Connection", "close")])
        response = read_response(reader)
        check(response["status"] == 200, "per-call read stop: %r" % response)
        connection.close()
        status, lines, stderr = server.finish(timeout=8)
        check(status == 0, "per-call read server exit %d; lines=%r stderr=%r" %
              (status, lines, stderr))
        stats = check_drained(lines)
        check(int(stats["timeouts"]) == 0,
              "per-call read timing charged a timeout: %r" % stats)
        return {"scenario": "stream-read-per-call", "opt": opt,
                "stats": stats}
    finally:
        server.close()


def scenario_stream_pause(program, opt):
    server = ServerProcess(
        program, body_timeout=2000, drain_timeout=1000, tick=20,
        write_timeout=100, processing_timeout=1000)
    try:
        connection = connect(server.port)
        reader = Reader(connection)
        send_request(connection, "/stream-pause")
        response = read_response(reader)
        check(response["body"] == b"onetwo", "paused stream body: %r" % response)
        check(response["trailers"].get("x-pause") == b"done",
              "paused stream trailer: %r" % response)
        server.lines.wait_text("STREAM-PAUSE-DONE /stream-pause", timeout=3)
        connection.close()

        stop = connect(server.port)
        stop_reader = Reader(stop)
        send_request(stop, "/stop", headers=[("Connection", "close")])
        read_response(stop_reader)
        stop.close()
        status, lines, stderr = server.finish(timeout=8)
        check(status == 0, "stream-pause server exit %d; lines=%r stderr=%r" %
              (status, lines, stderr))
        stats = check_drained(lines)
        check(int(stats["timeouts"]) == 0,
              "processing pause was charged to the write clock: %r" % stats)
        return {"scenario": "stream-write-processing-clocks", "opt": opt,
                "stats": stats}
    finally:
        server.close()


def scenario_write_timeout(program, opt):
    server = ServerProcess(program, write_timeout=150, tick=20)
    try:
        hold = connect(server.port)
        send_request(hold, "/hold-write")
        server.lines.wait_text("HOLD-WRITE-START /hold-write", timeout=3)
        server.lines.wait_text("HOLD-WRITE-CLEANUP /hold-write", timeout=5)
        check("HOLD-WRITE-AFTER /hold-write" not in server.lines.lines,
              "timed-out writer kept running: %r" % server.lines.lines)
        hold.close()

        stop = connect(server.port)
        stop_reader = Reader(stop)
        send_request(stop, "/stop", headers=[("Connection", "close")])
        read_response(stop_reader)
        stop.close()
        status, lines, stderr = server.finish(timeout=8)
        check(status == 0, "write-timeout server exit %d; lines=%r stderr=%r" %
              (status, lines, stderr))
        stats = check_drained(lines)
        check(int(stats["timeouts"]) >= 1, "write timeout not counted: %r" % stats)
        return {"scenario": "write-stall-timeout", "opt": opt, "stats": stats}
    finally:
        server.close()


def scenario_stop_continue_drain_cap(program, opt):
    """A pending stop clamps the 100 Continue write and still drains it."""
    server = ServerProcess(program, drain_timeout=1000, tick=20,
                           write_timeout=30000)
    try:
        slow = connect(server.port)
        slow_reader = Reader(slow)
        slow.sendall(
            b"POST /buffered-echo HTTP/1.1\r\nHost: 127.0.0.1\r\n"
            b"Content-Length: 5\r\nExpect: 100-continue\r\n")
        time.sleep(0.1)
        stop = connect(server.port)
        stop_reader = Reader(stop)
        send_request(stop, "/stop", headers=[("Connection", "close")])
        try:
            read_response(stop_reader)
        except Failure:
            pass
        # Let the stop controller publish SERVER_STOPPING before the head is
        # completed, so the 100 Continue transition sees a pending drain.
        time.sleep(0.1)
        slow.sendall(b"\r\n")
        response = read_response(slow_reader)
        check(response["status"] == 100,
              "pending-stop 100 Continue: %r" % response)
        slow.sendall(b"hello")
        response = read_response(slow_reader)
        check(response["body"] == b"hello",
              "pending-stop echoed body: %r" % response)
        check(response["headers"].get("connection") == [b"close"],
              "pending-stop connection policy: %r" % response["headers"])
        slow.close()
        stop.close()
        status, lines, stderr = server.finish(timeout=8)
        check(status == 0, "continue-drain server exit %d; lines=%r stderr=%r" %
              (status, lines, stderr))
        stats = check_drained(lines)
        check(int(stats["requests"]) == 2,
              "continue-drain request count: %r" % stats)
        check(int(stats["errors"]) == 0,
              "continue-drain counted an error: %r" % stats)
        check(int(stats["timeouts"]) == 0,
              "continue-drain counted a timeout: %r" % stats)
        return {"scenario": "stop-continue-drain-cap", "opt": opt,
                "stats": stats}
    finally:
        server.close()


def scenario_graceful_drain_cap(program, opt):
    """A stalled write uses the drain deadline, not the longer write clock."""
    server = ServerProcess(program, drain_timeout=250, tick=20,
                           write_timeout=30000)
    try:
        hold = connect(server.port)
        send_request(hold, "/hold-write")
        server.lines.wait_text("HOLD-WRITE-START /hold-write", timeout=3)
        time.sleep(0.1)
        stop = connect(server.port)
        stop_reader = Reader(stop)
        send_request(stop, "/stop", headers=[("Connection", "close")])
        try:
            read_response(stop_reader)
        except Failure:
            pass
        server.lines.wait_text("HOLD-WRITE-CLEANUP /hold-write", timeout=5)
        check("HOLD-WRITE-AFTER /hold-write" not in server.lines.lines,
              "drain cap did not stop the writer: %r" % server.lines.lines)
        hold.close()
        stop.close()
        status, lines, stderr = server.finish(timeout=8)
        check(status == 0, "drain-cap server exit %d; lines=%r stderr=%r" %
              (status, lines, stderr))
        stats = check_drained(lines)
        check(int(stats["timeouts"]) >= 1,
              "drain cap did not time out the write: %r" % stats)
        return {"scenario": "graceful-drain-cap", "opt": opt,
                "stats": stats}
    finally:
        server.close()


def scenario_default_registration(program, opt):
    server = ServerProcess(program, mode="stream-default")
    try:
        connection = connect(server.port)
        reader = Reader(connection)
        send_request(connection, "/fixed")
        response = read_response(reader)
        check(response["body"] == b"fixed", "default stream fixed: %r" % response)
        send_request(connection, "/echo-chunked")
        response = read_response(reader)
        check(response["trailers"].get("x-request-trailers") == b"0",
              "default stream chunked: %r" % response)
        send_request(connection, "/stop", headers=[("Connection", "close")])
        response = read_response(reader)
        check(response["status"] == 200, "default stream stop: %r" % response)
        connection.close()
        status, lines, stderr = server.finish()
        check(status == 0, "default server exit %d; lines=%r stderr=%r" %
              (status, lines, stderr))
        stats = check_drained(lines)
        check(int(stats["requests"]) >= 3, "default request counter: %r" % stats)
        return {"scenario": "listen-stream-default", "opt": opt, "stats": stats}
    finally:
        server.close()


def scenario_stream_return_inflight(program, opt):
    server = ServerProcess(program, tick=20)
    try:
        connection = connect(server.port)
        send_head(connection, "/return-read", method="POST", content_length=10)
        server.lines.wait_text("RETURN-READ-START /return-read", timeout=3)
        server.lines.wait_text(
            "RETURN-READ-HANDLER-DONE /return-read", timeout=3)
        # The request body stays incomplete (10 declared, zero sent). After the
        # handler returns, only the spawned read's typed rejection and a
        # zero-byte close may appear. Sending another request here would let
        # /fixed be parsed as the missing body instead of testing the return.
        server.lines.wait_text(
            "RETURN-READ-OP-REJECTED ReadOrder", timeout=3)
        check("RETURN-READ-OP-AFTER" not in server.lines.lines,
              "pending read resumed after handler return: %r" %
              server.lines.lines)
        check("RETURN-READ-OP-REJECTED Other" not in server.lines.lines,
              "pending read got a non-ReadOrder error: %r" %
              server.lines.lines)
        late = read_bytes_until_eof(connection, timeout=3)
        check(late == b"",
              "return-read produced response bytes: %r" % late[:160])
        check("FIXED-HANDLER-ENTER /fixed" not in server.lines.lines,
              "return-read entered /fixed: %r" % server.lines.lines)
        connection.close()

        connection = connect(server.port)
        # Both complete requests are on the wire before the first handler
        # returns. The pipelined /fixed must never reach its handler.
        connection.sendall(request_bytes("/return-write") +
                           request_bytes("/fixed"))
        server.lines.wait_text("RETURN-WRITE-START /return-write", timeout=3)
        server.lines.wait_text(
            "RETURN-WRITE-HANDLER-DONE /return-write", timeout=3)
        server.lines.wait_text(
            "RETURN-WRITE-OP-REJECTED ReadOrder", timeout=5)
        check("RETURN-WRITE-OP-AFTER" not in server.lines.lines,
              "pending write resumed after handler return: %r" %
              server.lines.lines)
        check("RETURN-WRITE-OP-REJECTED Other" not in server.lines.lines,
              "pending write got a non-ReadOrder error: %r" %
              server.lines.lines)
        data = read_bytes_until_eof(connection, timeout=8,
                                    limit=HOLD_WRITE_BYTES + 4096)
        check(data.count(b"HTTP/1.1 ") == 1,
              "return-write served a second response: %r" % data[:160])
        partial = parse_partial_chunked_response(data)
        check(partial["chunk_size"] == HOLD_WRITE_BYTES,
              "return-write chunk size: %d" % partial["chunk_size"])
        check(0 < len(partial["payload"]) < HOLD_WRITE_BYTES,
              "return-write had %d/%d partial chunk bytes" %
              (len(partial["payload"]), HOLD_WRITE_BYTES))
        check(partial["payload"] == b'z' * len(partial["payload"]),
              "return-write partial chunk bytes differ")
        check("FIXED-HANDLER-ENTER /fixed" not in server.lines.lines,
              "pipelined /fixed handler entered: %r" % server.lines.lines)
        check(b"fixed" not in data,
              "pipelined /fixed was served after return-write: %r" %
              data[:160])
        connection.close()

        fast = connect(server.port)
        fast_reader = Reader(fast)
        send_request(fast, "/fast")
        response = read_response(fast_reader)
        check(response["body"] == b"fast",
              "other connection after in-flight return: %r" % response)
        fast.close()

        stop = connect(server.port)
        stop_reader = Reader(stop)
        send_request(stop, "/stop", headers=[("Connection", "close")])
        read_response(stop_reader)
        stop.close()
        status, lines, stderr = server.finish(timeout=8)
        check(status == 0, "in-flight-return server exit %r; lines=%r stderr=%r" %
              (status, lines, stderr))
        stats = check_drained(lines)
        check(int(stats["requests"]) == 2,
              "in-flight stream return requests: %r" % stats)
        check(int(stats["errors"]) == 2,
              "in-flight stream returns were not counted exactly: %r" % stats)
        check(int(stats["timeouts"]) == 0,
              "in-flight stream return charged a timeout: %r" % stats)
        return {"scenario": "stream-return-inflight", "opt": opt,
                "stats": stats,
                "read_second_response_bytes": len(late),
                "return_write_response_bytes": len(data),
                "return_write_partial_chunk_bytes": len(partial["payload"]),
                "fixed_handler_entered":
                    "FIXED-HANDLER-ENTER /fixed" in lines,
                "pipelined_second_response": data.count(b"HTTP/1.1 ") > 1}
    finally:
        server.close()


def scenario_overlap_pending_read(program, opt):
    server = ServerProcess(program, tick=20)
    try:
        connection = connect(server.port)
        reader = Reader(connection)
        send_head(connection, "/overlap-read-start", method="POST",
                  content_length=5)
        server.lines.wait_text("OVERLAP-READ-OP-START", timeout=3)
        server.lines.wait_text("OVERLAP-READ-COMMIT true", timeout=3)
        check("OVERLAP-READ-COMMIT false" not in server.lines.lines,
              "read overlap was not rejected: %r" % server.lines.lines)
        connection.sendall(b"hello")
        server.lines.wait_text("OVERLAP-READ-OP-DONE 5", timeout=3)
        response = read_response(reader)
        check((response["status"], response["body"]) == (200, b"after-read"),
              "overlap read poisoned the response: %r" % response)
        send_request(connection, "/fast")
        response = read_response(reader)
        check(response["body"] == b"fast",
              "overlap read poisoned keep-alive: %r" % response)
        connection.close()

        stop = connect(server.port)
        stop_reader = Reader(stop)
        send_request(stop, "/stop", headers=[("Connection", "close")])
        read_response(stop_reader)
        stop.close()
        status, lines, stderr = server.finish(timeout=8)
        check(status == 0, "overlap-read server exit %r; lines=%r stderr=%r" %
              (status, lines, stderr))
        stats = check_drained(lines)
        check(int(stats["errors"]) == 0,
              "rejected read overlap counted an error: %r" % stats)
        return {"scenario": "stream-overlap-pending-read", "opt": opt,
                "stats": stats}
    finally:
        server.close()


def scenario_overlap_pending_write(program, opt):
    server = ServerProcess(program, tick=20)
    try:
        connection = connect(server.port)
        reader = Reader(connection)
        send_head(connection, "/overlap-write-read", method="POST",
                  content_length=10)
        server.lines.wait_text("OVERLAP-WRITE-OP-START", timeout=3)
        server.lines.wait_text("OVERLAP-WRITE-READ true", timeout=5)
        check("OVERLAP-WRITE-READ false" not in server.lines.lines,
              "write overlap was not rejected: %r" % server.lines.lines)
        response = read_response(reader)
        server.lines.wait_text("OVERLAP-WRITE-OP-DONE", timeout=8)
        check(len(response["body"]) == HOLD_WRITE_BYTES,
              "write overlap body length: %d" % len(response["body"]))
        check(response["body"] == b"{" * HOLD_WRITE_BYTES,
              "write overlap body bytes differ")
        check(response["trailers"].get("x-overlap") == b"done",
              "write overlap trailers: %r" % response["trailers"])
        connection.close()

        fast = connect(server.port)
        fast_reader = Reader(fast)
        send_request(fast, "/fast")
        response = read_response(fast_reader)
        check(response["body"] == b"fast",
              "write overlap blocked another connection: %r" % response)
        fast.close()

        stop = connect(server.port)
        stop_reader = Reader(stop)
        send_request(stop, "/stop", headers=[("Connection", "close")])
        read_response(stop_reader)
        stop.close()
        status, lines, stderr = server.finish(timeout=8)
        check(status == 0, "overlap-write server exit %r; lines=%r stderr=%r" %
              (status, lines, stderr))
        stats = check_drained(lines)
        check(int(stats["errors"]) == 0,
              "rejected write overlap counted an error: %r" % stats)
        return {"scenario": "stream-overlap-pending-write", "opt": opt,
                "stats": stats}
    finally:
        server.close()


def _before(lines, first, second):
    return (first in lines and second in lines and
            lines.index(first) < lines.index(second))


def scenario_timeout_alias_read_cleanup(program, opt):
    """A read deadline must wake an aliased read while the fd is still owned."""
    server = ServerProcess(program, body_timeout=150, tick=20)
    try:
        hold = connect(server.port)
        reader = Reader(hold)
        send_head(hold, "/timeout-aliased-read", method="POST",
                  content_length=10)
        # Withhold the body so the spawned read is still pending at the deadline.
        server.lines.wait_text("ALIAS-READER-START", timeout=3)
        server.lines.wait_text("ALIAS-HANDLER-CLEANUP", timeout=4)
        server.lines.wait_text("ALIAS-READER-CLEANUP", timeout=4)
        check("DRAINED true" not in server.lines.lines,
              "read-deadline alias cleanup waited for drain: %r" %
              server.lines.lines)
        check(reader.at_eof(timeout=3), "timed-out alias connection stayed open")
        hold.close()

        fast = connect(server.port)
        fast_reader = Reader(fast)
        send_request(fast, "/fast")
        response = read_response(fast_reader)
        check(response["body"] == b"fast",
              "healthy request after alias timeout: %r" % response)
        fast.close()

        stop = connect(server.port)
        stop_reader = Reader(stop)
        send_request(stop, "/stop", headers=[("Connection", "close")])
        read_response(stop_reader)
        stop.close()
        status, lines, stderr = server.finish(timeout=8)
        check(status == 0, "alias-timeout server exit %r; lines=%r stderr=%r" %
              (status, lines, stderr))
        stats = check_drained(lines)
        require_order(lines, "ALIAS-READER-START", "ALIAS-HANDLER-CLEANUP")
        require_order(lines, "ALIAS-HANDLER-CLEANUP", "ALIAS-READER-CLEANUP")
        # We only sent /stop after cleanup, so cleanup predates the explicit
        # DRAIN-ENTER marker rather than being inferred from drain behavior.
        require_order(lines, "ALIAS-READER-CLEANUP", "DRAIN-ENTER")
        require_order(lines, "DRAIN-ENTER", "DRAINED true")
        check("ALIAS-READER-EXIT Read" not in lines,
              "aliased read succeeded without a full body: %r" % lines)
        check(int(stats["timeouts"]) == 1,
              "alias read deadline not counted exactly once: %r" % stats)
        exit_line = next((line for line in lines
                          if line.startswith("ALIAS-READER-EXIT")), None)
        return {"scenario": "timeout-alias-read-cleanup", "opt": opt,
                "stats": stats, "read_exit": exit_line}
    finally:
        server.close()


def scenario_abort_alias_read_cleanup(program, opt):
    """Abort is an independent read-direction cancellation exit."""
    server = ServerProcess(program, tick=20)
    try:
        hold = connect(server.port)
        send_head(hold, "/timeout-aliased-read", method="POST",
                  content_length=10)
        server.lines.wait_text("ALIAS-READER-START", timeout=3)

        # A legal same-stream start must be rejected with typed ReadOrder while
        # the aliased read owns the direction. This proves the read is pending
        # before the abort, rather than only proving the coroutine started.
        overlap = connect(server.port)
        overlap_reader = Reader(overlap)
        send_request(overlap, "/alias-read-overlap",
                     headers=[("Connection", "close")])
        gate_response = read_response(overlap_reader)
        check(gate_response["body"] == b"alias-overlap-gate",
              "alias-read gate response: %r" % gate_response)
        overlap.close()
        server.lines.wait_text("ALIAS-READ-OVERLAP ReadOrder", timeout=3)
        check("ALIAS-READ-OVERLAP USED" not in server.lines.lines,
              "same-stream start succeeded during aliased read: %r" %
              server.lines.lines)
        check("ALIAS-READ-OVERLAP Other" not in server.lines.lines,
              "read overlap got a non-ReadOrder error: %r" %
              server.lines.lines)

        abort = connect(server.port)
        send_request(abort, "/abort", headers=[("Connection", "close")])
        try:
            read_response(Reader(abort))
        except Failure:
            pass
        server.lines.wait_text("ALIAS-HANDLER-CLEANUP", timeout=5)
        server.lines.wait_text("ALIAS-READER-CLEANUP", timeout=5)
        # The canceled read owns no response. The old connection must reach
        # EOF without delivering a late body or a failure response.
        late = read_bytes_until_eof(hold, timeout=3)
        check(late == b"",
              "aborted alias read connection carried %d bytes: %r" %
              (len(late), late[:160]))
        status, lines, stderr = server.finish(timeout=8)
        check(status == 0, "alias-abort server exit %r; lines=%r stderr=%r" %
              (status, lines, stderr))
        check("ALIAS-READER-CLEANUP" in lines,
              "aborted alias read never cleaned up: %r" % lines)
        check("ALIAS-HANDLER-CLEANUP" in lines,
              "aborted alias handler never cleaned up: %r" % lines)
        check("ALIAS-READER-EXIT Read" not in lines,
              "aliased read completed instead of being aborted: %r" % lines)
        require_order(lines, "ALIAS-READER-START", "ALIAS-READ-OVERLAP ReadOrder")
        require_order(lines, "ALIAS-READ-OVERLAP ReadOrder",
                      "ALIAS-READER-CLEANUP")
        require_order(lines, "ALIAS-READER-START", "ALIAS-HANDLER-CLEANUP")
        require_order(lines, "ALIAS-HANDLER-CLEANUP", "ALIAS-READER-CLEANUP")
        # DRAIN-ENTER is printed immediately before server.drain. The fixture
        # waits 1 s after abort, so cleanup must precede that explicit point,
        # not merely occur during drain before DRAINED true.
        require_order(lines, "ALIAS-READER-CLEANUP", "DRAIN-ENTER")
        require_order(lines, "DRAIN-ENTER", "DRAINED true")
        cleanup_before_drain = _before(lines, "ALIAS-READER-CLEANUP",
                                       "DRAIN-ENTER")
        stats = check_drained(lines)
        check(int(stats["requests"]) == 2,
              "alias-read gate/abort request count: %r" % stats)
        check(int(stats["timeouts"]) == 0,
              "abort charged the alias read a timeout: %r" % stats)
        exit_line = next((line for line in lines
                          if line.startswith("ALIAS-READER-EXIT")), None)
        hold.close()
        abort.close()
        return {"scenario": "abort-alias-read-cleanup", "opt": opt,
                "stats": stats,
                "cleanup_before_drain": cleanup_before_drain,
                "read_exit": exit_line,
                "read_eof_bytes": len(late)}
    finally:
        server.close()


def scenario_timeout_alias_write_cleanup(program, opt):
    """A write deadline must wake an aliased writer blocked on the fd.

    The peer never reads the 32 MiB chunk, so the spawned write is suspended
    on writable when the stream write deadline cancels the handler.
    """
    server = ServerProcess(program, write_timeout=600, tick=20)
    try:
        hold = connect(server.port)
        send_request(hold, "/timeout-aliased-write")
        server.lines.wait_text("ALIAS-WRITER-START", timeout=3)
        server.lines.wait_text("ALIAS-WRITER-HEAD", timeout=5)
        server.lines.wait_text("ALIAS-WRITER-HANDLER-CLEANUP", timeout=8)
        server.lines.wait_text("ALIAS-WRITER-CLEANUP", timeout=8)
        check("ALIAS-WRITER-AFTER" not in server.lines.lines,
              "timed-out aliased write continued: %r" % server.lines.lines)
        check("ALIAS-WRITER-EXIT Other" not in server.lines.lines,
              "aliased write left with a non-ReadOrder error: %r" %
              server.lines.lines)
        check("DRAINED true" not in server.lines.lines,
              "write-deadline alias cleanup waited for drain: %r" %
              server.lines.lines)
        data = read_bytes_until_eof(hold, timeout=5,
                                    limit=HOLD_WRITE_BYTES + 4096)
        partial = parse_partial_chunked_response(data)
        check(partial["chunk_size"] == HOLD_WRITE_BYTES,
              "write-deadline chunk size: %d" % partial["chunk_size"])
        check(0 < len(partial["payload"]) < HOLD_WRITE_BYTES,
              "write-deadline had %d/%d partial chunk bytes" %
              (len(partial["payload"]), HOLD_WRITE_BYTES))
        check(partial["payload"] == b'~' * len(partial["payload"]),
              "write-deadline partial chunk bytes differ")
        sent = len(data)
        hold.close()

        fast = connect(server.port)
        fast_reader = Reader(fast)
        send_request(fast, "/fast")
        response = read_response(fast_reader)
        check(response["body"] == b"fast",
              "healthy request after alias write timeout: %r" % response)
        fast.close()

        stop = connect(server.port)
        stop_reader = Reader(stop)
        send_request(stop, "/stop", headers=[("Connection", "close")])
        read_response(stop_reader)
        stop.close()
        status, lines, stderr = server.finish(timeout=8)
        check(status == 0, "alias-write-timeout server exit %r; lines=%r stderr=%r" %
              (status, lines, stderr))
        stats = check_drained(lines)
        require_order(lines, "ALIAS-WRITER-START", "ALIAS-WRITER-HEAD")
        require_order(lines, "ALIAS-WRITER-HEAD", "ALIAS-WRITER-HANDLER-CLEANUP")
        require_order(lines, "ALIAS-WRITER-HANDLER-CLEANUP",
                      "ALIAS-WRITER-CLEANUP")
        require_order(lines, "ALIAS-WRITER-CLEANUP", "DRAIN-ENTER")
        require_order(lines, "DRAIN-ENTER", "DRAINED true")
        check(int(stats["timeouts"]) == 1,
              "aliased write deadline not counted exactly once: %r" % stats)
        check(int(stats["requests"]) == 2,
              "aliased write deadline request count: %r" % stats)
        return {"scenario": "timeout-alias-write-cleanup", "opt": opt,
                "stats": stats, "partial_response_bytes": sent,
                "partial_chunk_bytes": len(partial["payload"])}
    finally:
        server.close()


def scenario_abort_alias_write_cleanup(program, opt):
    """Abort must wake an aliased writer blocked on the fd."""
    server = ServerProcess(program, tick=20)
    try:
        hold = connect(server.port)
        send_request(hold, "/timeout-aliased-write")
        server.lines.wait_text("ALIAS-WRITER-START", timeout=3)
        server.lines.wait_text("ALIAS-WRITER-HEAD", timeout=5)
        # Do not read the client socket: wait until the writer has actually put
        # a substantial chunk prefix in the receive buffer. This is a measured
        # readiness condition, not a fixed sleep, and enables the abort only
        # after the write is in flight.
        receive_capacity = hold.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF)
        buffered = wait_pending_bytes(hold, min(65536, receive_capacity // 4))
        check(buffered < HOLD_WRITE_BYTES,
              "writer completed the full chunk before abort: %d bytes" %
              buffered)

        abort = connect(server.port)
        send_request(abort, "/abort", headers=[("Connection", "close")])
        try:
            read_response(Reader(abort))
        except Failure:
            pass
        # Wait for the cancellation cleanup before draining the client-side
        # buffers, so reading cannot unblock the writer ahead of abort.
        server.lines.wait_text("ALIAS-WRITER-HANDLER-CLEANUP", timeout=5)
        server.lines.wait_text("ALIAS-WRITER-CLEANUP", timeout=5)
        data = read_bytes_until_eof(hold, timeout=5,
                                    limit=HOLD_WRITE_BYTES + 4096)
        partial = parse_partial_chunked_response(data)
        check(partial["chunk_size"] == HOLD_WRITE_BYTES,
              "aborted writer chunk size: %d" % partial["chunk_size"])
        check(0 < len(partial["payload"]) < HOLD_WRITE_BYTES,
              "aborted writer had %d/%d partial chunk bytes" %
              (len(partial["payload"]), HOLD_WRITE_BYTES))
        check(partial["payload"] == b'~' * len(partial["payload"]),
              "aborted writer partial chunk bytes differ")
        sent = len(data)
        status, lines, stderr = server.finish(timeout=8)
        check(status == 0, "alias-write-abort server exit %r; lines=%r stderr=%r" %
              (status, lines, stderr))
        check("ALIAS-WRITER-CLEANUP" in lines,
              "aborted aliased write never cleaned up: %r" % lines)
        check("ALIAS-WRITER-HANDLER-CLEANUP" in lines,
              "aborted writer handler never cleaned up: %r" % lines)
        check("ALIAS-WRITER-AFTER" not in lines,
              "aborted aliased write continued: %r" % lines)
        check("ALIAS-WRITER-EXIT Other" not in lines,
              "aborted aliased write left with a non-ReadOrder error: %r" %
              lines)
        require_order(lines, "ALIAS-WRITER-START", "ALIAS-WRITER-HANDLER-CLEANUP")
        require_order(lines, "ALIAS-WRITER-HANDLER-CLEANUP",
                      "ALIAS-WRITER-CLEANUP")
        # DRAIN-ENTER is printed immediately before server.drain.
        require_order(lines, "ALIAS-WRITER-CLEANUP", "DRAIN-ENTER")
        require_order(lines, "DRAIN-ENTER", "DRAINED true")
        cleanup_before_drain = _before(lines, "ALIAS-WRITER-CLEANUP",
                                       "DRAIN-ENTER")
        stats = check_drained(lines)
        check(int(stats["requests"]) == 1,
              "aborted aliased write request count: %r" % stats)
        check(int(stats["timeouts"]) == 0,
              "abort charged the aliased write a timeout: %r" % stats)
        hold.close()
        abort.close()
        return {"scenario": "abort-alias-write-cleanup", "opt": opt,
                "stats": stats, "in_flight_bytes": sent,
                "buffered_at_abort": buffered,
                "partial_chunk_bytes": len(partial["payload"]),
                "cleanup_before_drain": cleanup_before_drain}
    finally:
        server.close()


def _exercise_reused_read(server, connection, old_fd, reused_fd,
                          reused_inode, held):
    """Read and write through the exact reused server fd, then stop."""
    reader = Reader(connection)
    send_head(connection, "/read-early", method="POST", content_length=6)
    connection.sendall(b"abcd")
    server.lines.wait_text("BODY-PART /read-early 4 4", timeout=3)
    connection.sendall(b"ef")
    response = read_response(reader)
    check((response["status"], response["body"]) == (200, b"done"),
          "reused-fd read response: %r" % response)
    send_request(connection, "/fast")
    response = read_response(reader)
    check(response["body"] == b"fast",
          "reused-fd fast response: %r" % response)
    fd_now, inode_now = server_fd_for(server, connection)
    check(fd_now == old_fd,
          "reused connection changed fd: %d -> %d" % (old_fd, fd_now))
    check(fd_now == reused_fd,
          "reused fd differs from acquisition: %d -> %d" %
          (reused_fd, fd_now))
    check(inode_now == reused_inode,
          "reused connection changed socket inode: %s -> %s" %
          (reused_inode, inode_now))
    close_connections(held)

    stop = connect(server.port)
    stop_reader = Reader(stop)
    send_request(stop, "/stop", headers=[("Connection", "close")])
    read_response(stop_reader)
    stop.close()
    status, lines, stderr = server.finish(timeout=8)
    check(status == 0, "reused-fd server exit %r; lines=%r stderr=%r" %
          (status, lines, stderr))
    stats = check_drained(lines)
    return stats


def scenario_direct_cancel_alias_write(program, opt):
    """Cancel the spawned writer task directly while its fd wait is pending.

    The handler keeps the connection owned, waits for the writer's cancel exit
    plan, then returns. The old fd is then measured through /proc, released,
    and a new connection is checked for exact fd-number reuse.
    """
    server = ServerProcess(program, tick=20, write_timeout=30000)
    try:
        hold = connect(server.port)
        old_fd, old_inode = server_fd_for(server, hold)
        send_request(hold, "/direct-cancel-aliased-write")
        server.lines.wait_text("DCW-WRITER-START", timeout=3)
        server.lines.wait_text("DCW-WRITER-HEAD", timeout=5)
        server.lines.wait_text("DCW-WRITER-CLEANUP", timeout=10)
        server.lines.wait_text("DCW-HANDLER-DONE", timeout=3)
        lines = server.lines.lines
        check("DCW-WRITER-AFTER" not in lines,
              "directly cancelled writer continued: %r" % lines)
        check("DCW-WRITER-EXIT Other" not in lines,
              "directly cancelled writer left with a non-ReadOrder error: %r" %
              lines)
        require_order(lines, "DCW-WRITER-HEAD", "DCW-WRITER-CLEANUP")
        require_order(lines, "DCW-WRITER-CLEANUP", "DCW-HANDLER-DONE")
        data = read_bytes_until_eof(hold, timeout=5,
                                    limit=HOLD_WRITE_BYTES + 4096)
        partial = parse_partial_chunked_response(data)
        check(partial["chunk_size"] == HOLD_WRITE_BYTES,
              "direct-cancel chunk size: %d" % partial["chunk_size"])
        check(0 < len(partial["payload"]) < HOLD_WRITE_BYTES,
              "direct-cancel had %d/%d partial chunk bytes" %
              (len(partial["payload"]), HOLD_WRITE_BYTES))
        check(partial["payload"] == b'\x7f' * len(partial["payload"]),
              "direct-cancel partial chunk bytes differ")
        sent = len(data)
        hold.close()
        wait_fd_released(server, old_fd, old_inode, timeout=3)

        reused, matched, reused_fd, reused_inode, held = \
            acquire_reused_connection(server, old_fd)
        require_fd_reuse(old_fd, old_inode, matched,
                         reused_fd, reused_inode)
        stats = _exercise_reused_read(
            server, reused, old_fd, reused_fd, reused_inode, held)
        check(int(stats["timeouts"]) == 0,
              "direct cancel charged a timeout: %r" % stats)
        check(int(stats["errors"]) == 1,
              "incomplete committed response was not counted exactly: %r" %
              stats)
        check(int(stats["requests"]) == 3,
              "direct-cancel request count: %r" % stats)
        return {"scenario": "direct-cancel-alias-write", "opt": opt,
                "stats": stats, "partial_response_bytes": sent,
                "partial_chunk_bytes": len(partial["payload"]),
                "old_fd": old_fd, "old_inode": old_inode,
                "reused_fd": reused_fd,
                "reused_inode": reused_inode,
                "fd_reuse_observed": True}
    finally:
        try:
            close_connections(held)
        except NameError:
            pass
        server.close()


def scenario_direct_cancel_stream_timer(program, opt):
    """Direct cancellation replaces the abandoned write clock and poisons frames."""
    server = ServerProcess(program, tick=20, write_timeout=500,
                           processing_timeout=3000)
    try:
        hold = connect(server.port)
        send_request(hold, "/direct-cancel-op-timer")
        server.lines.wait_text("DCO-TIMER-HANDLER-START", timeout=3)
        server.lines.wait_text("DCO-TIMER-FINISH true", timeout=8)
        server.lines.wait_text("DCO-TIMER-HANDLER-DONE", timeout=5)
        data = read_bytes_until_eof(hold, timeout=5)
        check(data.startswith(b"HTTP/1.1 200 "),
              "direct-cancel timer response head: %r" % data[:160])
        check(data.count(b"HTTP/1.1 ") == 1,
              "direct-cancel timer appended a frame: %r" % data[:160])
        hold.close()

        stop = connect(server.port)
        stop_reader = Reader(stop)
        send_request(stop, "/stop", headers=[("Connection", "close")])
        read_response(stop_reader)
        stop.close()
        status, lines, stderr = server.finish(timeout=8)
        check(status == 0,
              "direct-cancel timer server exit %d; lines=%r stderr=%r" %
              (status, lines, stderr))
        stats = check_drained(lines)
        check(int(stats["timeouts"]) == 0,
              "cancelled write left a stale timer: %r" % stats)
        check(int(stats["errors"]) >= 1,
              "incomplete cancelled response was not counted: %r" % stats)
        check(int(stats["requests"]) == 1,
              "cancelled response was counted as complete: %r" % stats)
        require_order(lines, "DCO-TIMER-HANDLER-START",
                      "DCO-TIMER-FINISH true")
        require_order(lines, "DCO-TIMER-FINISH true",
                      "DCO-TIMER-HANDLER-DONE")
        return {"scenario": "direct-cancel-stream-timer", "opt": opt,
                "stats": stats, "partial_response_bytes": len(data)}
    finally:
        server.close()


def scenario_stale_stream_fd_reuse(program, opt):
    """Use a retained, uncommitted stream after its fd number was reused.

    The handler returns without committing a response, so the server produces
    the initial 500 and a stale start could not be rejected by the committed
    flag. A separate control request triggers the stale start only after the
    replacement connection has delivered its first body part. The remaining
    body is withheld until the typed rejection is observed, and the same
    replacement fd must still serve it afterwards.
    """
    server = ServerProcess(program, tick=20)
    try:
        old = connect(server.port)
        old_fd, old_inode = server_fd_for(server, old)
        old_reader = Reader(old)
        send_request(old, "/stale-fd-reuse")
        response = read_response(old_reader)
        check((response["status"], response["body"]) == (500, b""),
              "stale-fd uncommitted response: %r" % response)
        late = bytes(old_reader.buffer) + read_bytes_until_eof(old, timeout=3)
        check(late == b"",
              "stale-fd old connection carried %d extra bytes: %r" %
              (len(late), late[:160]))
        server.lines.wait_text("STALE-FD-HANDLER-DONE", timeout=3)
        old.close()
        wait_fd_released(server, old_fd, old_inode, timeout=3)

        reused, matched, reused_fd, reused_inode, held = \
            acquire_reused_connection(server, old_fd)
        require_fd_reuse(old_fd, old_inode, matched,
                         reused_fd, reused_inode)
        fd_acquired, inode_acquired = server_fd_for(server, reused)
        check(fd_acquired == old_fd and inode_acquired == reused_inode,
              "replacement fd changed at acquisition: %d/%s -> %d/%s" %
              (reused_fd, reused_inode, fd_acquired, inode_acquired))
        reader = Reader(reused)
        send_head(reused, "/read-early", method="POST", content_length=6)
        reused.sendall(b"abcd")
        server.lines.wait_text("BODY-PART /read-early 4 4", timeout=3)

        # The gate is a separate request. It runs the stale start after the
        # replacement connection's first body part, while the rest is withheld.
        gate = connect(server.port)
        gate_reader = Reader(gate)
        send_request(gate, "/stale-gate", headers=[("Connection", "close")])
        gate_response = read_response(gate_reader)
        check(gate_response["body"] == b"stale-gate",
              "stale gate response: %r" % gate_response)
        gate.close()
        server.lines.wait_text("STALE-FD-ATTEMPT", timeout=3)
        server.lines.wait_text("STALE-FD-REJECTED ReadOrder", timeout=3)
        check("STALE-FD-USED" not in server.lines.lines,
              "stale stream was used after fd reuse: %r" %
              server.lines.lines)
        check("STALE-FD-REJECTED Other" not in server.lines.lines,
              "stale stream got a non-ReadOrder error: %r" %
              server.lines.lines)
        fd_now, inode_now = server_fd_for(server, reused)
        check(fd_now == old_fd and inode_now == reused_inode,
              "replacement fd changed at stale rejection: %d/%s -> %d/%s" %
              (reused_fd, reused_inode, fd_now, inode_now))

        reused.sendall(b"ef")
        response = read_response(reader)
        check(response["body"] == b"done",
              "fd-reuse read response: %r" % response)
        fd_after, inode_after = server_fd_for(server, reused)
        check(fd_after == old_fd and inode_after == reused_inode,
              "replacement fd changed after body completion: %d/%s -> %d/%s" %
              (reused_fd, reused_inode, fd_after, inode_after))
        send_request(reused, "/fast")
        response = read_response(reader)
        check(response["body"] == b"fast",
              "reused fd after stale rejection: %r" % response)
        close_connections(held)

        stop = connect(server.port)
        stop_reader = Reader(stop)
        send_request(stop, "/stop", headers=[("Connection", "close")])
        read_response(stop_reader)
        stop.close()
        status, lines, stderr = server.finish(timeout=8)
        check(status == 0, "stale-fd server exit %r; lines=%r stderr=%r" %
              (status, lines, stderr))
        stats = check_drained(lines)
        check(int(stats["requests"]) == 4,
              "stale fd reuse request count: %r" % stats)
        check(int(stats["errors"]) == 1,
              "stale fd reuse error count: %r" % stats)
        check(int(stats["timeouts"]) == 0,
              "stale fd reuse charged a timeout: %r" % stats)
        return {"scenario": "stale-stream-fd-reuse", "opt": opt,
                "stats": stats, "old_fd": old_fd,
                "old_inode": old_inode,
                "reused_fd": reused_fd,
                "reused_inode": reused_inode,
                "fd_reuse_observed": True,
                "stale_rejected_before_rest": True}
    finally:
        try:
            close_connections(held)
        except NameError:
            pass
        server.close()


def scenario_timeout_abort_alias_cleanup(program, opt):
    """Compatibility selector that runs the split read timeout and abort cases."""
    timeout = scenario_timeout_alias_read_cleanup(program, opt)
    abort = scenario_abort_alias_read_cleanup(program, opt)
    return {"scenario": "timeout-abort-alias-cleanup", "opt": opt,
            "timeout": timeout, "abort": abort}


def scenario_direct_cancel_read_after_complete(program, opt):
    """Cancel a pending read after an early complete response, then read again.

    The first cancelled read installed a descriptor wait while complete=true.
    Its cleanup must unregister that wait and restore the processing deadline. The handler waits past the abandoned
    deadline and confirms that the second read owns the stream.
    """
    server = ServerProcess(program, tick=20, body_timeout=300,
                           processing_timeout=3000)
    try:
        connection = connect(server.port)
        reader = Reader(connection)
        send_head(connection, "/direct-cancel-read-after-complete",
                  method="POST", content_length=10,
                  headers=[("Connection", "close")])
        server.lines.wait_text("DCR-READ-HEAD-SENT", timeout=3)
        response = read_response(reader)
        check((response["status"], response["body"]) ==
              (200, b"early-complete"),
              "early complete response: %r" % response)
        server.lines.wait_text("DCR-READ-CANCELED 1", timeout=5)
        server.lines.wait_text("DCR-READ-SECOND-START true", timeout=5)
        connection.sendall(b"0123456789")
        server.lines.wait_text("DCR-READ-SECOND-DONE 10 true", timeout=5)
        server.lines.wait_text("DCR-READ-HANDLER-DONE 2", timeout=5)
        tail = read_bytes_until_eof(connection, timeout=3)
        check(tail == b"",
              "early complete response appended bytes: %r" % tail[:160])
        connection.close()

        stop = connect(server.port)
        stop_reader = Reader(stop)
        send_request(stop, "/stop", headers=[("Connection", "close")])
        read_response(stop_reader)
        stop.close()
        status, lines, stderr = server.finish(timeout=8)
        check(status == 0,
              "read-cancel-after-complete server exit %d; lines=%r stderr=%r" %
              (status, lines, stderr))
        stats = check_drained(lines)
        check(int(stats["requests"]) == 2,
              "read-cancel-after-complete requests: %r" % stats)
        check(int(stats["errors"]) == 0,
              "read-cancel-after-complete counted an error: %r" % stats)
        check(int(stats["timeouts"]) == 0,
              "read-cancel-after-complete counted a timeout: %r" % stats)
        require_order(lines, "DCR-READ-HEAD-SENT", "DCR-READ-CANCELED 1")
        require_order(lines, "DCR-READ-CANCELED 1",
                      "DCR-READ-SECOND-START true")
        require_order(lines, "DCR-READ-SECOND-DONE 10 true",
                      "DCR-READ-HANDLER-DONE 2")
        return {"scenario": "direct-cancel-read-after-complete", "opt": opt,
                "stats": stats}
    finally:
        server.close()


def scenario_chunked_no_head(program, opt):
    """Cancelling start_chunked before its head is sent must reject chunks."""
    server = ServerProcess(program, tick=20, body_timeout=30000)
    try:
        connection = connect(server.port)
        reader = Reader(connection)
        send_head(connection, "/chunked-no-head", method="POST",
                  content_length=10, headers=[("Connection", "close")])
        index = server.lines.wait_for(
            lambda line: line.startswith("CNH-STRAY-REJECTED"), timeout=5)
        rejected = server.lines.lines[index]
        check(rejected == "CNH-STRAY-REJECTED true true",
              "chunks accepted after a missing head: %r; lines=%r" %
              (rejected, server.lines.lines))
        server.lines.wait_text("CNH-FALLBACK true", timeout=3)
        response, response_bytes = read_closed_response(reader)
        check((response["status"], response["body"]) ==
              (200, b"head-fallback"),
              "missing-head fallback response: %r" % response)
        check("transfer-encoding" not in response["headers"],
              "fallback was chunked: %r" % response["headers"])
        check(b"stray" not in response["body"],
              "missing-head response carried stray chunk data: %r" % response)
        connection.close()

        stop = connect(server.port)
        stop_reader = Reader(stop)
        send_request(stop, "/stop", headers=[("Connection", "close")])
        stop_response, stop_bytes = read_closed_response(stop_reader)
        check(stop_response["status"] == 200,
              "chunked-no-head stop response: %r" % stop_response)
        stop.close()
        status, lines, stderr = server.finish(timeout=8)
        check(status == 0,
              "chunked-no-head server exit %d; lines=%r stderr=%r" %
              (status, lines, stderr))
        stats = check_drained(lines)
        check(int(stats["requests"]) == 2,
              "missing-head request count: %r" % stats)
        check(int(stats["errors"]) == 0,
              "missing-head fallback counted an error: %r" % stats)
        check(int(stats["bytes"]) == response_bytes + stop_bytes,
              "missing-head response bytes: %r != %d" %
              (stats, response_bytes + stop_bytes))
        require_order(lines, "CNH-HANDLER-START", "CNH-OPENING-CANCELED")
        require_order(lines, "CNH-OPENING-CANCELED", "CNH-READER-CANCELED")
        require_order(lines, "CNH-READER-CANCELED",
                      "CNH-STRAY-REJECTED true true")
        require_order(lines, "CNH-STRAY-REJECTED true true",
                      "CNH-FALLBACK true")
        return {"scenario": "chunked-no-head", "opt": opt, "stats": stats,
                "fallback": True,
                "response_bytes": response_bytes}
    finally:
        server.close()


def scenario_direct_cancel_nested_chunk(program, opt):
    """Cancel a large write frame shortly after it enters backpressure.

    The canceller is queued after the public write, so the chunked head is
    already complete and the large chunk frame is in its write loop when
    cancellation arrives. A nested writer would start here and keep sending
    after the frame was abandoned; the frame itself must stop.
    """
    server = ServerProcess(program, tick=20, write_timeout=30000)
    try:
        connection = connect(server.port)
        send_request(connection, "/direct-cancel-nested-chunk",
                     headers=[("Connection", "close")])
        server.lines.wait_text("DCN-HEAD-SENT", timeout=3)
        server.lines.wait_text("DCN-CANCEL-DONE", timeout=8)
        data = read_bytes_until_eof(connection, timeout=8,
                                    limit=64 * 1024 * 1024)
        check(data.startswith(b"HTTP/1.1 200 "),
              "cancelled nested chunk response head: %r" % data[:160])
        check(data.count(b"HTTP/1.1 ") == 1,
              "cancelled nested chunk appended a frame: %r" % data[:160])
        check(b"\r\n2000000\r\n" in data,
              "cancelled chunk frame did not start: %r" % data[:160])
        check(len(data) < 16 * 1024 * 1024,
              "nested writer kept sending: %d bytes after cancel" % len(data))
        connection.close()

        stop = connect(server.port)
        stop_reader = Reader(stop)
        send_request(stop, "/stop", headers=[("Connection", "close")])
        stop_response, stop_bytes = read_closed_response(stop_reader)
        check(stop_response["status"] == 200,
              "nested-chunk stop response: %r" % stop_response)
        stop.close()
        status, lines, stderr = server.finish(timeout=8)
        check(status == 0,
              "direct-cancel-nested server exit %d; lines=%r stderr=%r" %
              (status, lines, stderr))
        stats = check_drained(lines)
        check(int(stats["requests"]) == 1,
              "cancelled nested chunk counted complete: %r" % stats)
        check(int(stats["errors"]) >= 1,
              "cancelled nested chunk was not counted: %r" % stats)
        check(int(stats["timeouts"]) == 0,
              "cancelled nested chunk charged a timeout: %r" % stats)
        require_order(lines, "DCN-HEAD-SENT", "DCN-CANCEL-DONE")
        require_order(lines, "DCN-CANCEL-DONE", "DCN-HANDLER-DONE")
        return {"scenario": "direct-cancel-nested-chunk", "opt": opt,
                "stats": stats, "partial_response_bytes": len(data),
                "stop_bytes": stop_bytes}
    finally:
        server.close()


def scenario_concurrent_finish(program, opt):
    """Two spawned finish calls must serialize to one terminal chunk and count."""
    server = ServerProcess(program, tick=20)
    try:
        connection = connect(server.port)
        reader = Reader(connection)
        send_request(connection, "/finish-race",
                     headers=[("Connection", "close")])
        response = read_response(reader)
        check((response["status"], response["body"]) == (200, b"race"),
              "concurrent-finish response: %r" % response)
        check(response["headers"].get("transfer-encoding") == [b"chunked"],
              "concurrent-finish framing: %r" % response["headers"])
        check(response["headers"].get("connection") == [b"close"],
              "concurrent-finish close header: %r" % response["headers"])
        index = server.lines.wait_for(
            lambda line: line.startswith("FINISH-RACE-HANDLER-DONE"),
            timeout=5)
        handler_done = server.lines.lines[index]
        outcomes = [line for line in server.lines.lines
                    if line.startswith("FINISH-RACE-OP ") and
                    (line.endswith(" OK") or line.endswith(" ReadOrder") or
                     line.endswith(" Other"))]
        check(len(outcomes) == 2,
              "finish outcomes are not two: %r; lines=%r" %
              (outcomes, server.lines.lines))
        ok_count = sum(1 for line in outcomes if line.endswith(" OK"))
        order_count = sum(1 for line in outcomes
                          if line.endswith(" ReadOrder"))
        other_count = sum(1 for line in outcomes if line.endswith(" Other"))
        check(ok_count == 1,
              "concurrent finish did not have exactly one success: %r" %
              outcomes)
        check(order_count == 1,
              "concurrent finish did not have exactly one ReadOrder: %r" %
              outcomes)
        check(other_count == 0,
              "concurrent finish had a non-ReadOrder failure: %r" % outcomes)
        check(handler_done in ("FINISH-RACE-HANDLER-DONE 1 2",
                               "FINISH-RACE-HANDLER-DONE 2 1"),
              "finish result line was not one OK and one ReadOrder: %r" %
              handler_done)
        # The parser above stops at the first terminal chunk. Anything left in
        # the buffer, or arriving before close, is a second response frame.
        pending = bytes(reader.buffer)
        extra = read_bytes_until_eof(connection, timeout=3)
        tail = pending + extra
        check(tail == b"",
              "concurrent finish appended bytes after the terminal chunk: %r; "
              "lines=%r" % (tail[:200], server.lines.lines))
        race_bytes = reader.received + len(extra)
        connection.close()

        stop = connect(server.port)
        stop_reader = Reader(stop)
        send_request(stop, "/stop", headers=[("Connection", "close")])
        stop_response, stop_bytes = read_closed_response(stop_reader)
        check(stop_response["status"] == 200,
              "concurrent-finish stop response: %r" % stop_response)
        stop.close()
        status, lines, stderr = server.finish(timeout=8)
        check(status == 0,
              "concurrent-finish server exit %d; lines=%r stderr=%r" %
              (status, lines, stderr))
        stats = check_drained(lines)
        check(int(stats["requests"]) == 2,
              "concurrent finish counted the race twice: %r" % stats)
        check(int(stats["errors"]) == 0,
              "concurrent finish counted an error: %r" % stats)
        check(int(stats["timeouts"]) == 0,
              "concurrent finish counted a timeout: %r" % stats)
        check(int(stats["bytes"]) == race_bytes + stop_bytes,
              "concurrent finish bytes: %r != %d" %
              (stats, race_bytes + stop_bytes))
        require_order(lines, "FINISH-RACE-HANDLER-START",
                      "FINISH-RACE-OP-START 0")
        require_order(lines, "FINISH-RACE-HANDLER-START",
                      "FINISH-RACE-OP-START 1")
        return {"scenario": "concurrent-finish-serialization", "opt": opt,
                "stats": stats, "outcomes": outcomes,
                "race_bytes": race_bytes, "stop_bytes": stop_bytes}
    finally:
        server.close()


SCENARIOS = (
    ("fixed-framing-errors", scenario_fixed_and_framing),
    ("large-fixed-single-write", scenario_large_fixed),
    ("early-body-early-response", scenario_body_early),
    ("stop-allows-stream-finish", scenario_stop_graceful),
    ("chunked-echo-expect", scenario_chunked_echo),
    ("body-limit", scenario_limits),
    ("stale-duplicate-rejection", scenario_stale_duplicate),
    ("backpressure", scenario_backpressure),
    ("stop-abort-cancellation", scenario_stop_abort),
    ("body-read-timeout", scenario_body_timeout),
    ("write-stall-timeout", scenario_write_timeout),
    ("stream-read-processing-clocks", scenario_processing_timeout),
    ("stream-processing-gaps", scenario_processing_gaps),
    ("stream-processing-single-gap", scenario_processing_single_gap),
    ("stream-read-per-call", scenario_read_per_call),
    ("stream-write-processing-clocks", scenario_stream_pause),
    ("stop-continue-drain-cap", scenario_stop_continue_drain_cap),
    ("graceful-drain-cap", scenario_graceful_drain_cap),
    ("stream-return-inflight", scenario_stream_return_inflight),
    ("stream-overlap-pending-read", scenario_overlap_pending_read),
    ("stream-overlap-pending-write", scenario_overlap_pending_write),
    ("listen-stream-default", scenario_default_registration),
    ("chunked-connection-close", scenario_chunked_connection_close),
    ("failure-stats-exact", scenario_failure_stats_exact),
    ("stream-alias-complete-no-error", scenario_alias_complete_no_error),
    ("timeout-alias-read-cleanup", scenario_timeout_alias_read_cleanup),
    ("abort-alias-read-cleanup", scenario_abort_alias_read_cleanup),
    ("timeout-alias-write-cleanup", scenario_timeout_alias_write_cleanup),
    ("abort-alias-write-cleanup", scenario_abort_alias_write_cleanup),
    ("direct-cancel-alias-write", scenario_direct_cancel_alias_write),
    ("direct-cancel-stream-timer", scenario_direct_cancel_stream_timer),
    ("direct-cancel-read-after-complete",
     scenario_direct_cancel_read_after_complete),
    ("chunked-no-head", scenario_chunked_no_head),
    ("direct-cancel-nested-chunk", scenario_direct_cancel_nested_chunk),
    ("concurrent-finish-serialization", scenario_concurrent_finish),
    ("stale-stream-fd-reuse", scenario_stale_stream_fd_reuse),
)

# Selector-only compatibility aggregate; not part of the default run.
ALIASES = {
    "timeout-abort-alias-cleanup": scenario_timeout_abort_alias_cleanup,
}


def build(cli, out_dir, opt, timeout):
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
        timeout=timeout,
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
    parser.add_argument("--out", default="/tmp/http-stream-server-results")
    parser.add_argument("--only-opt", choices=("0", "2"))
    parser.add_argument("--libxml", type=Path)
    parser.add_argument("--build-timeout", type=int, default=600)
    parser.add_argument("--only-scenario",
                        help="run only the named scenario at each selected opt")
    arguments = parser.parse_args()
    if arguments.build_timeout <= 0:
        parser.error("--build-timeout must be positive")
    if arguments.libxml and arguments.libxml.is_dir():
        os.environ["LD_LIBRARY_PATH"] = os.pathsep.join(filter(None, (
            str(arguments.libxml.resolve()), os.environ.get("LD_LIBRARY_PATH"),
        )))
    names = [name for name, _ in SCENARIOS] + list(ALIASES)
    check(arguments.only_scenario is None or
          arguments.only_scenario in names,
          "unknown scenario: %r" % arguments.only_scenario)
    if arguments.only_scenario in ALIASES:
        selected = [(arguments.only_scenario,
                     ALIASES[arguments.only_scenario])]
    else:
        selected = [(name, run) for name, run in SCENARIOS
                    if arguments.only_scenario in (None, name)]
    cli = Path(arguments.cli).resolve()
    check(cli.is_file(), "compiler is not a file: %s" % cli)
    out = Path(arguments.out)
    out.mkdir(parents=True, exist_ok=True)
    options = [arguments.only_opt] if arguments.only_opt else ["0", "2"]
    report = {"status": "passed", "compiler": str(cli), "builds": [], "runs": []}
    try:
        for opt in options:
            program = build(cli, out / ("opt" + opt), opt, arguments.build_timeout)
            report["builds"].append({"opt": int(opt), "program": str(program)})
            for name, run in selected:
                report["runs"].append(run(program, int(opt)))
    except Failure as error:
        report["status"] = "failed"
        report["failure"] = str(error)
        (out / "http-stream-server-results.json").write_text(
            json.dumps(report, indent=2) + "\n")
        print("FAILED: %s" % error, flush=True)
        return 1
    (out / "http-stream-server-results.json").write_text(
        json.dumps(report, indent=2) + "\n")
    print(json.dumps({
        "status": "passed",
        "runs": len(report["runs"]),
        "report": str(out / "http-stream-server-results.json"),
    }), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
