#!/usr/bin/env python3
"""Build and exercise the real Woozy std/http codec at Native O0 and O2.

The Python side only drives subprocesses, inspects raw bytes, and acts as an
independent loopback TCP peer. It never evaluates Woozy or reimplements the
codec to decide expected values.
"""
import argparse
import json
import os
from pathlib import Path
import select
import socket
import subprocess
import time


ROOT = Path(__file__).resolve().parents[3]
MANIFEST = ROOT / "tests/native/http/lang.toml"
BINARY = bytes([0, 1, 2, 255, 254, 10, 13])
LOOPBACK = "127.0.0.1"


class PipeReader:
    def __init__(self, process):
        self.process = process
        self.fd = process.stdout.fileno()
        self.buffer = bytearray()
        self.eof = False

    def _read(self, timeout):
        ready, _, _ = select.select([self.fd], [], [], max(0.0, timeout))
        if not ready:
            raise AssertionError("process pipe timeout")
        chunk = os.read(self.fd, 65536)
        if not chunk:
            self.eof = True
        else:
            self.buffer.extend(chunk)

    def line(self, timeout=8):
        deadline = time.monotonic() + timeout
        while True:
            index = self.buffer.find(b"\n")
            if index >= 0:
                line = bytes(self.buffer[:index])
                del self.buffer[:index + 1]
                return line
            if self.eof:
                raise AssertionError("process closed before a line")
            self._read(deadline - time.monotonic())

    def rest(self, timeout=8):
        deadline = time.monotonic() + timeout
        while not self.eof:
            self._read(deadline - time.monotonic())
        data = bytes(self.buffer)
        self.buffer.clear()
        return data


def command(argv, environment, timeout=120):
    process = subprocess.run([str(value) for value in argv],
                             stdin=subprocess.DEVNULL,
                             stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE,
                             env=environment,
                             timeout=timeout)
    return dict(argv=[str(value) for value in argv], exit=process.returncode,
                stdout=process.stdout.decode("utf-8", "backslashreplace"),
                stderr=process.stderr.decode("utf-8", "backslashreplace"))


def succeeded(row):
    assert row["exit"] == 0, row


def environment_for(cli):
    environment = dict(os.environ, PYTHONDONTWRITEBYTECODE="1", PYTHONNOUSERSITE="1")
    for parent in cli.parents:
        library = parent / "toolchains/libxml2-legacy-2.13.9/usr/lib"
        if (library / "libxml2.so.2").exists():
            previous = environment.get("LD_LIBRARY_PATH")
            environment["LD_LIBRARY_PATH"] = str(library) + (":" + previous if previous else "")
            break
    return environment


def start_server(program, mode, extra=()):
    process = subprocess.Popen([str(program), mode, *map(str, extra)],
                               stdin=subprocess.DEVNULL,
                               stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE,
                               bufsize=0)
    reader = PipeReader(process)
    try:
        line = reader.line()
        assert line.startswith(b"LISTENING "), (mode, line, reader.buffer)
        port = int(line.split()[1])
        assert 0 < port <= 65535, (mode, line)
    except BaseException:
        stop_process(process)
        raise
    return process, reader, port


def connect(port, timeout=5):
    connection = socket.create_connection((LOOPBACK, port), timeout=timeout)
    connection.settimeout(timeout)
    return connection


def recv_all(connection, timeout=8):
    connection.settimeout(timeout)
    chunks = []
    while True:
        chunk = connection.recv(65536)
        if not chunk:
            break
        chunks.append(chunk)
    return b"".join(chunks)


def send_fragmented(connection, data, step=1, delay=0.001, allow_reset=False):
    for offset in range(0, len(data), step):
        try:
            connection.sendall(data[offset:offset + step])
        except (BrokenPipeError, ConnectionResetError):
            if allow_reset:
                return offset
            raise
        if delay:
            time.sleep(delay)
    return len(data)


def finish_process(process, reader, timeout=8):
    try:
        process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=2)
        raise
    rest = reader.rest(timeout=2)
    stderr = process.stderr.read()
    process.stdout.close()
    process.stderr.close()
    return process.returncode, rest, stderr


def stop_process(process):
    if process.poll() is None:
        process.kill()
        process.wait(timeout=2)


def parse_headers(head):
    lines = head.split(b"\r\n")
    start = lines[0]
    headers = []
    for line in lines[1:]:
        name, value = line.split(b":", 1)
        headers.append((name.decode("ascii"), value.strip(b" \t")))
    return start, headers


def header_values(headers, name):
    return [value for key, value in headers if key.lower() == name.lower()]


def parse_response_bytes(data):
    responses = []
    offset = 0
    while offset < len(data):
        end = data.find(b"\r\n\r\n", offset)
        assert end >= 0, ("unterminated response", data[offset:offset + 120])
        head = data[offset:end]
        start, headers = parse_headers(head)
        lengths = header_values(headers, "Content-Length")
        assert len(lengths) == 1, (start, lengths)
        length = int(lengths[0])
        body_start = end + 4
        body_end = body_start + length
        assert body_end <= len(data), (start, length, len(data) - body_start)
        responses.append((start, headers, data[body_start:body_end]))
        offset = body_end
    return responses


def server_roundtrip(program, environment):
    process, reader, port = start_server(program, "serve-roundtrip")
    try:
        with connect(port) as connection:
            request = (
                b"POST /upload?x=1 HTTP/1.1\r\n"
                b"Host: fixture\r\n"
                + "X-Unicode: 中😀 / ok\r\n".encode()
                + b"X-Multi: one\r\n"
                b"X-Multi: two\r\n"
                b"X-Obs: \x80\xff\r\n"
                b"Content-Length: 7\r\n"
                b"Content-Length: 7\r\n"
                b"Content-Type: application/octet-stream\r\n\r\n"
                + BINARY
            )
            send_fragmented(connection, request, step=2, delay=0.001)
            connection.shutdown(socket.SHUT_WR)
            response = recv_all(connection)
        responses = parse_response_bytes(response)
        assert len(responses) == 1, responses
        start, headers, body = responses[0]
        assert start == b"HTTP/1.1 201 Created " + "😀".encode(), start
        assert header_values(headers, "Content-Length") == [b"4"], headers
        assert header_values(headers, "X-Reply") == ["中😀".encode(), b"second"], headers
        assert header_values(headers, "X-Obs") == [b"\x80\xff"], headers
        assert body == bytes([9, 8, 0, 255]), body
        result = reader.line()
        return_code, rest, stderr = finish_process(process, reader)
        assert result == b"RESULT OK", (result, rest, stderr)
        assert not rest, rest
        assert return_code == 0 and not stderr, (return_code, stderr)
        return dict(request_bytes=len(request), request_fragments=(len(request) + 1) // 2,
                    response_bytes=len(response), duplicate_content_length=True,
                    unicode_header=True, binary_request_body=True,
                    binary_response_body=True)
    except BaseException:
        stop_process(process)
        raise


def server_pipeline(program, environment):
    process, reader, port = start_server(program, "serve-pipeline")
    try:
        with connect(port) as connection:
            first = (
                b"POST /first HTTP/1.1\r\n"
                b"Host: fixture\r\n"
                b"Transfer-Encoding: chunked\r\n\r\n"
                b"4;foo=bar\r\nWiki\r\n"
                b"5;quoted=\"a b\"\r\npedia\r\n"
                b"0;end=yes\r\n"
                b"X-Trailer: done\r\n"
                b"Authorization: secret\r\n"
                b"X-Dup: t1\r\n"
                b"X-Dup: t2\r\n\r\n"
            )
            second = b"GET /second HTTP/1.1\r\nHost: fixture\r\nX-Second: yes\r\n\r\n"
            stream = first + second
            send_fragmented(connection, stream, step=1, delay=0.001)
            connection.shutdown(socket.SHUT_WR)
            response = recv_all(connection)
        responses = parse_response_bytes(response)
        assert len(responses) == 2, responses
        assert responses[0][0] == b"HTTP/1.1 200 First", responses[0][0]
        assert responses[0][2] == b"first-ok", responses[0]
        assert responses[1][0] == b"HTTP/1.1 200 Second", responses[1][0]
        assert responses[1][2] == b"second-ok", responses[1]
        result = reader.line()
        return_code, rest, stderr = finish_process(process, reader)
        assert result == b"RESULT OK", (result, rest, stderr)
        assert not rest, rest
        assert return_code == 0 and not stderr, (return_code, stderr)
        return dict(wire_bytes=len(stream), fragments=len(stream),
                    pipelined_requests=2, chunk_extensions=True,
                    trailers_separate=True, pending_preserved=True)
    except BaseException:
        stop_process(process)
        raise


ERROR_CASES = [
    ("te-cl", "TransferEncodingWithContentLength", "default",
     b"POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n"
     b"Content-Length: 4\r\n\r\n"),
    ("duplicate-cl", "ConflictingContentLength", "default",
     b"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\n"
     b"Content-Length: 5\r\n\r\n"),
    ("bad-header-name", "InvalidHeaderName", "default",
     b"GET / HTTP/1.1\r\nHost: x\r\nBad Name: y\r\n\r\n"),
    ("obs-fold", "ObsoleteLineFolding", "default",
     b"GET / HTTP/1.1\r\nHost: x\r\n folded-value\r\n\r\n"),
    ("header-control", "InvalidHeaderValue", "default",
     b"GET / HTTP/1.1\r\nHost: x\r\nX-Test: a\x01b\r\n\r\n"),
    ("head-oversize", "HeadTooLarge", "tiny",
     b"GET /" + b"a" * 80 + b" HTTP/1.1\r\nHost: x\r\n\r\n"),
    ("header-count", "TooManyHeaders", "headers",
     b"GET / HTTP/1.1\r\nHost: x\r\nX-One: 1\r\nX-Two: 2\r\nX-Three: 3\r\n\r\n"),
    ("body-oversize", "BodyTooLarge", "tiny",
     b"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 17\r\n\r\n"),
    ("cl-overflow", "LengthOverflow", "default",
     b"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 18446744073709551616\r\n\r\n"),
    ("chunk-overflow", "LengthOverflow", "default",
     b"POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"
     b"10000000000000000\r\nabc"),
    ("chunk-truncated", "UnexpectedEof", "default",
     b"POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nabc"),
    ("chunk-forbidden-trailer", "ForbiddenTrailer", "default",
     b"POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"
     b"0\r\nContent-Length: 1\r\n\r\n"),
    ("chunk-line-too-long", "ChunkLineTooLong", "default",
     b"POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n" + b"a" * 5000),
    ("chunk-bad-terminator", "InvalidChunkTerminator", "default",
     b"POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n1\r\nxZZ"),
    ("eof-in-header", "UnexpectedEof", "default",
     b"GET / HTTP/1.1\r\nHost: x"),
    ("eof-in-fixed-body", "UnexpectedEof", "default",
     b"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nabc"),
    ("bad-version", "UnsupportedVersion", "default",
     b"GET / HTTP/1.2\r\nHost: x\r\n\r\n"),
    ("missing-host", "MissingHost", "default",
     b"GET / HTTP/1.1\r\n\r\n"),
    ("duplicate-host", "DuplicateHost", "default",
     b"GET / HTTP/1.1\r\nHost: one.example\r\nHost: two.example\r\n\r\n"),
    ("invalid-host", "InvalidHost", "default",
     b"GET / HTTP/1.1\r\nHost: user@example.com\r\n\r\n"),
    ("invalid-host-ipv6", "InvalidHost", "default",
     b"GET / HTTP/1.1\r\nHost: [::1]extra\r\n\r\n"),
]


def server_error(program, environment, name, expected, profile, fixture):
    process, reader, port = start_server(program, "serve-error", (expected, profile))
    try:
        with connect(port) as connection:
            send_fragmented(connection, fixture, step=7, delay=0, allow_reset=True)
            try:
                connection.shutdown(socket.SHUT_WR)
            except OSError:
                pass
            try:
                connection.recv(1)
            except OSError:
                pass
        result = reader.line()
        return_code, rest, stderr = finish_process(process, reader)
        assert result == b"RESULT " + expected.encode(), (name, result, rest, stderr)
        assert not rest, (name, rest)
        assert return_code == 0 and not stderr, (name, return_code, stderr)
        return dict(case=name, fixture_bytes=len(fixture), error=expected, profile=profile)
    except BaseException:
        stop_process(process)
        raise


class RequestReader:
    def __init__(self, connection):
        self.connection = connection
        self.buffer = bytearray()

    def _fill(self):
        chunk = self.connection.recv(65536)
        assert chunk, ("client closed before request", bytes(self.buffer[:120]))
        self.buffer.extend(chunk)

    def request(self):
        while b"\r\n\r\n" not in self.buffer:
            self._fill()
        end = self.buffer.index(b"\r\n\r\n")
        head = bytes(self.buffer[:end])
        del self.buffer[:end + 4]
        start, headers = parse_headers(head)
        lengths = header_values(headers, "Content-Length")
        assert len(lengths) == 1, (start, lengths)
        length = int(lengths[0])
        while len(self.buffer) < length:
            self._fill()
        body = bytes(self.buffer[:length])
        del self.buffer[:length]
        return start, headers, body


def client_roundtrip(program, environment):
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.bind((LOOPBACK, 0))
    listener.listen(1)
    listener.settimeout(8)
    port = listener.getsockname()[1]
    process = subprocess.Popen([str(program), "client", str(port)],
                               stdin=subprocess.DEVNULL,
                               stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE,
                               bufsize=0)
    reader = PipeReader(process)
    try:
        connection, _ = listener.accept()
        connection.settimeout(8)
        with connection:
            requests = RequestReader(connection)
            start, headers, body = requests.request()
            assert start == b"POST /client-upload HTTP/1.1", start
            assert body == bytes([0, 255, 1, 254]), body
            assert header_values(headers, "Content-Length") == [b"4"], headers
            assert header_values(headers, "Transfer-Encoding") == [], headers
            assert header_values(headers, "X-Multi") == [b"one", b"two"], headers
            assert header_values(headers, "X-Unicode") == ["中😀".encode()], headers
            assert header_values(headers, "X-Obs") == [b"\x80\xff"], headers
            chunked = (
                b"HTTP/1.1 200 \x80\xff\r\n"
                b"Transfer-Encoding: chunked\r\n"
                + "X-Reply: 中😀\r\n".encode()
                + b"X-Obs: \x80\xff\r\n"
                + b"\r\n"
                b"5;ext=1\r\nchunk\r\n"
                b"3\r\n-ok\r\n"
                b"0\r\n"
                b"X-Trailer: yes\r\n\r\n"
            )
            send_fragmented(connection, chunked, step=3, delay=0.002)

            start, headers, body = requests.request()
            assert start == b"HEAD /head HTTP/1.1", start
            assert body == b"", body
            assert header_values(headers, "Content-Length") == [b"0"], headers
            head_sequence = (
                b"HTTP/1.1 100 Continue\r\n\r\n"
                b"HTTP/1.1 200 OK\r\nContent-Length: 123\r\nX-Repr: yes\r\n\r\n"
                b"HTTP/1.1 204 No Content\r\n\r\n"
                b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"
            )
            send_fragmented(connection, head_sequence, step=5, delay=0.001)

            start, headers, body = requests.request()
            assert start == b"GET /close HTTP/1.1", start
            assert body == b"", body
            connection.sendall(
                b"HTTP/1.0 200 OK\r\nConnection: close\r\n\r\nclosed")
            connection.shutdown(socket.SHUT_WR)
        result = reader.line()
        return_code, rest, stderr = finish_process(process, reader)
        assert result == b"RESULT OK", (result, rest, stderr)
        assert not rest, rest
        assert return_code == 0 and not stderr, (return_code, stderr)
        return dict(request_roundtrip=3, chunked_response=True,
                    response_trailer=True, interim_1xx=True, head_no_body=True,
                    no_content_204=True, close_delimited=True)
    except BaseException:
        stop_process(process)
        raise
    finally:
        listener.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.cli = args.cli.resolve()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    environment = environment_for(args.cli)
    report = dict(status="failed", contract="stdlib/http/README.contract.md",
                  compiler=str(args.cli), runs=[])
    try:
        for opt in (0, 2):
            row = dict(opt=opt, status="failed", build=None, selftest=None,
                       cases=[])
            report["runs"].append(row)
            output = args.out / f"O{opt}"
            row["build"] = build = command([
                args.cli, "build", "--project", MANIFEST, "--backend", "native",
                "--opt", str(opt), "--out-dir", output, "--format", "json",
            ], environment)
            succeeded(build)
            program = output / "program"
            row["selftest"] = selftest = command([program, "selftest"], environment, 30)
            succeeded(selftest)
            assert selftest["stdout"] == "http-selftest: ok\n", selftest
            assert not selftest["stderr"], selftest
            row["cases"].append(dict(name="codec-selftest", status="passed"))
            row["nonblocking"] = nonblocking = command(
                [program, "nonblocking-retry"], environment, 30)
            succeeded(nonblocking)
            assert nonblocking["stdout"] == "nonblocking-retry: ok\n", nonblocking
            assert not nonblocking["stderr"], nonblocking
            row["cases"].append(dict(name="nonblocking-poll-retry", status="passed"))
            row["cases"].append(dict(name="server-roundtrip", status="passed",
                                     details=server_roundtrip(program, environment)))
            row["cases"].append(dict(name="server-pipeline-fragmented", status="passed",
                                     details=server_pipeline(program, environment)))
            for name, expected, profile, fixture in ERROR_CASES:
                row["cases"].append(dict(name="server-error-" + name, status="passed",
                                         details=server_error(
                                             program, environment, name,
                                             expected, profile, fixture)))
            row["cases"].append(dict(name="client-fixture-roundtrip", status="passed",
                                     details=client_roundtrip(program, environment)))
            row["status"] = "passed"
        report["status"] = "passed"
    except BaseException as error:
        report["error"] = repr(error)
        raise
    finally:
        path = args.out / "http-results.json"
        path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
        print(json.dumps(dict(status=report["status"], runs=len(report["runs"]),
                              report=str(path))))


if __name__ == "__main__":
    main()
