#!/usr/bin/env python3
"""Independent Python peer for the Woozy std/tls and std/https source tests.

Only the standard library is used.  The script drives real loopback TLS/HTTP
connections, records what it saw, and returns a small JSON result.  It never
imports or evaluates Woozy code and never contacts an external network.

Certificates are supplied by the caller; the runner generates throwaway test
certificates.  No private key or credential is stored in this repository.
"""

import argparse
import json
import os
import socket
import ssl
import sys
import tempfile
import traceback


def payload(size):
    return bytes(((index * 37 + 11) & 0xFF) for index in range(size))


def write_result(path, result):
    data = json.dumps(result, sort_keys=True).encode("utf-8") + b"\n"
    if path is None:
        sys.stdout.buffer.write(data)
        sys.stdout.buffer.flush()
        return
    directory = os.path.dirname(path) or "."
    descriptor, temporary = tempfile.mkstemp(prefix=".source-result-", dir=directory)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(data)
        os.replace(temporary, path)
    except BaseException:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise


def tls_context(args, server):
    if server:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(args.cert, args.key)
        return context
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.check_hostname = True
    context.verify_mode = ssl.CERT_REQUIRED
    context.load_verify_locations(cafile=args.ca)
    return context


def make_listener(host, timeout):
    family = socket.AF_INET6 if ":" in host else socket.AF_INET
    listener = socket.socket(family, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if family == socket.AF_INET6:
        listener.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
    listener.bind((host, 0))
    listener.listen(1)
    listener.settimeout(timeout)
    return listener


def recv_into_buffer(connection, buffer, timeout):
    connection.settimeout(timeout)
    chunk = connection.recv(65536)
    if not chunk:
        raise EOFError("peer closed")
    buffer.extend(chunk)
    return len(chunk)


def read_until(connection, buffer, marker, timeout):
    while True:
        index = buffer.find(marker)
        if index >= 0:
            end = index + len(marker)
            value = bytes(buffer[:end])
            del buffer[:end]
            return value
        recv_into_buffer(connection, buffer, timeout)


def read_exact(connection, buffer, length, timeout):
    while len(buffer) < length:
        recv_into_buffer(connection, buffer, timeout)
    value = bytes(buffer[:length])
    del buffer[:length]
    return value


def parse_head(head):
    lines = head[:-4].split(b"\r\n")
    start = lines[0]
    headers = []
    for line in lines[1:]:
        name, value = line.split(b":", 1)
        headers.append((name.decode("ascii"), value.strip(b" \t")))
    return start, headers


def header_values(headers, name):
    return [value for key, value in headers if key.lower() == name.lower()]


def read_chunked(connection, buffer, timeout):
    body = bytearray()
    while True:
        line = read_until(connection, buffer, b"\r\n", timeout)
        token = line[:-2].split(b";", 1)[0].strip()
        size = int(token, 16)
        if size == 0:
            break
        body.extend(read_exact(connection, buffer, size, timeout))
        if read_exact(connection, buffer, 2, timeout) != b"\r\n":
            raise AssertionError("missing chunk terminator")
    trailers = []
    while True:
        line = read_until(connection, buffer, b"\r\n", timeout)
        if line == b"\r\n":
            break
        name, value = line[:-2].split(b":", 1)
        trailers.append((name.decode("ascii"), value.strip(b" \t")))
    return bytes(body), trailers


def read_message(connection, buffer, timeout, expect_response=False):
    head = read_until(connection, buffer, b"\r\n\r\n", timeout)
    start, headers = parse_head(head)
    body = b""
    trailers = []
    if header_values(headers, "Transfer-Encoding"):
        body, trailers = read_chunked(connection, buffer, timeout)
    else:
        lengths = header_values(headers, "Content-Length")
        if lengths:
            body = read_exact(connection, buffer, int(lengths[0]), timeout)
        elif expect_response:
            status = int(start.split()[1])
            if status >= 200 and status != 204 and status != 304:
                # The test responses always frame their bodies; refusing an
                # unframed response keeps the peer checks deterministic.
                raise AssertionError("unframed test response")
    return start, headers, body, trailers


def chunked_bytes(data, sizes):
    out = bytearray()
    offset = 0
    for size in sizes:
        piece = data[offset:offset + size]
        offset += len(piece)
        if not piece:
            break
        out.extend(("%x" % len(piece)).encode("ascii") + b"\r\n")
        out.extend(piece)
        out.extend(b"\r\n")
    if offset < len(data):
        piece = data[offset:]
        out.extend(("%x" % len(piece)).encode("ascii") + b"\r\n")
        out.extend(piece)
        out.extend(b"\r\n")
    out.extend(b"0\r\nX-Trailer: done\r\n\r\n")
    return bytes(out)


def response(status, reason, body, extra_headers=(), chunked=False):
    lines = [b"HTTP/1.1 %d %s" % (status, reason)]
    for name, value in extra_headers:
        lines.append(name + b": " + value)
    if chunked:
        lines.append(b"Transfer-Encoding: chunked")
        head = b"\r\n".join(lines) + b"\r\n\r\n"
        return head + chunked_bytes(body, (1, 2, 7, 64))
    lines.append(b"Content-Length: " + str(len(body)).encode("ascii"))
    head = b"\r\n".join(lines) + b"\r\n\r\n"
    return head + body


def server(args):
    result = {
        "mode": args.mode,
        "handshake": "not-started",
        "sni": None,
        "requests": [],
        "ok": False,
    }
    listener = None
    connection = None
    wrapped = None
    buffer = bytearray()
    try:
        context = tls_context(args, server=True)

        def record_sni(ssl_socket, server_name, initial_context):
            del ssl_socket, initial_context
            result["sni"] = server_name

        context.set_servername_callback(record_sni)
        listener = make_listener(args.host, args.timeout)
        port = listener.getsockname()[1]
        with open(args.port_file, "w", encoding="ascii") as output:
            output.write("%d\n" % port)
        connection, peer = listener.accept()
        del peer
        connection.settimeout(args.timeout)
        wrapped = context.wrap_socket(connection, server_side=True)
        connection = None
        result["handshake"] = "ok"
        result["version"] = wrapped.version()
        result["cipher"] = wrapped.cipher()[0]
        if args.expect_sni is not None:
            expected = None if args.expect_sni == "none" else args.expect_sni
            if result["sni"] != expected:
                raise AssertionError(
                    "SNI mismatch: expected %r, got %r"
                    % (expected, result["sni"])
                )

        if args.mode == "tls-clean":
            data = payload(args.payload_size)
            wrapped.sendall(data)
            result["bytes_sent"] = len(data)
            wrapped.settimeout(5.0)
            raw = wrapped.unwrap()
            result["unwrap"] = "ok"
            raw.close()
            wrapped = None
        elif args.mode == "tls-truncate":
            data = payload(args.payload_size)
            wrapped.sendall(data)
            result["bytes_sent"] = len(data)
            wrapped.shutdown(socket.SHUT_RDWR)
            wrapped.close()
            wrapped = None
            result["unwrap"] = "bypassed"
        elif args.mode == "tls-echo":
            body = read_exact(wrapped, buffer, args.payload_size, args.timeout)
            result["echo_bytes"] = len(body)
            wrapped.sendall(body)
            wrapped.settimeout(5.0)
            raw = wrapped.unwrap()
            result["unwrap"] = "ok"
            raw.close()
            wrapped = None
        elif args.mode == "https-single":
            start, headers, body, trailers = read_message(
                wrapped, buffer, args.timeout)
            result["requests"].append({
                "start": start.decode("latin-1"),
                "body": len(body),
                "headers": len(headers),
                "trailers": len(trailers),
            })
            reply_body = payload(4096)
            wrapped.sendall(response(200, b"OK", reply_body,
                                     [(b"X-Reply", b"yes")], chunked=True))
            wrapped.settimeout(5.0)
            raw = wrapped.unwrap()
            result["unwrap"] = "ok"
            raw.close()
            wrapped = None
        elif args.mode == "https-reuse":
            start, headers, body, trailers = read_message(
                wrapped, buffer, args.timeout)
            result["requests"].append({
                "start": start.decode("latin-1"),
                "body": len(body),
                "headers": len(headers),
            })
            wrapped.sendall(response(200, b"First", payload(8), chunked=True))
            start, headers, body, trailers = read_message(
                wrapped, buffer, args.timeout)
            result["requests"].append({
                "start": start.decode("latin-1"),
                "body": len(body),
                "headers": len(headers),
            })
            wrapped.sendall(b"HTTP/1.1 204 No Content\r\n\r\n")
            wrapped.settimeout(5.0)
            raw = wrapped.unwrap()
            result["unwrap"] = "ok"
            raw.close()
            wrapped = None
        else:
            raise AssertionError("unknown server mode %s" % args.mode)
        result["ok"] = True
    except BaseException as error:
        result["error"] = "%s: %s" % (type(error).__name__, error)
        if not isinstance(error, (ssl.SSLError, ConnectionError, OSError,
                                  EOFError, AssertionError, socket.timeout)):
            result["traceback"] = traceback.format_exc()
    finally:
        for item in (wrapped, connection, listener):
            if item is not None:
                try:
                    item.close()
                except OSError:
                    pass
        write_result(args.result_file, result)
    return 0


def connect(args):
    context = tls_context(args, server=False)
    connection = socket.create_connection((args.host, args.port),
                                          timeout=args.timeout)
    wrapped = context.wrap_socket(connection, server_hostname=args.name)
    wrapped.settimeout(args.timeout)
    return wrapped


def client(args):
    result = {"mode": args.mode, "ok": False}
    wrapped = None
    try:
        wrapped = connect(args)
        result["version"] = wrapped.version()
        result["name"] = wrapped.server_hostname
        if args.mode == "tls-echo":
            data = payload(args.payload_size)
            wrapped.sendall(data)
            received = bytearray()
            while len(received) < len(data):
                chunk = wrapped.recv(65536)
                if not chunk:
                    raise EOFError("server closed before echo completed")
                received.extend(chunk)
            if bytes(received) != data:
                raise AssertionError("echo mismatch")
            result["echo_bytes"] = len(received)
            raw = wrapped.unwrap()
            raw.close()
            wrapped = None
        elif args.mode == "https-single":
            wrapped.sendall(
                b"GET /single HTTP/1.1\r\n"
                b"Host: localhost\r\n"
                b"X-Test: python\r\n\r\n")
            buffer = bytearray()
            start, headers, body, trailers = read_message(
                wrapped, buffer, args.timeout, expect_response=True)
            if start != b"HTTP/1.1 200 Single":
                raise AssertionError("single status line %r" % start)
            if body != payload(4096):
                raise AssertionError("single body mismatch")
            result["body"] = len(body)
            raw = wrapped.unwrap()
            raw.close()
            wrapped = None
        elif args.mode == "https-chunked":
            body = bytes([0, 1, 2, 255, 254, 10, 13]) + payload(61)
            request = (
                b"POST /chunk HTTP/1.1\r\n"
                b"Host: localhost\r\n"
                b"Transfer-Encoding: chunked\r\n\r\n"
                + chunked_bytes(body, (1, 2, 5, 3))
            )
            wrapped.sendall(request)
            buffer = bytearray()
            start, headers, received, trailers = read_message(
                wrapped, buffer, args.timeout, expect_response=True)
            if start != b"HTTP/1.1 200 ChunkReply":
                raise AssertionError("chunk status line %r" % start)
            if received != body:
                raise AssertionError("chunk response body mismatch")
            if header_values(headers, "X-Reply") != [b"chunk-ok"]:
                raise AssertionError("missing X-Reply")
            result["chunk_request"] = len(body)
            result["chunk_response"] = len(received)
            wrapped.sendall(
                b"GET /second HTTP/1.1\r\nHost: localhost\r\n\r\n")
            start, headers, received, trailers = read_message(
                wrapped, buffer, args.timeout, expect_response=True)
            if start != b"HTTP/1.1 204 No Content":
                raise AssertionError("second status line %r" % start)
            if received:
                raise AssertionError("204 response carried a body")
            result["reuse"] = True
            raw = wrapped.unwrap()
            raw.close()
            wrapped = None
        else:
            raise AssertionError("unknown client mode %s" % args.mode)
        result["ok"] = True
    except BaseException as error:
        result["error"] = "%s: %s" % (type(error).__name__, error)
        if not isinstance(error, (ssl.SSLError, ConnectionError, OSError,
                                  EOFError, AssertionError, socket.timeout)):
            result["traceback"] = traceback.format_exc()
    finally:
        if wrapped is not None:
            try:
                wrapped.close()
            except OSError:
                pass
        write_result(args.result_file, result)
    return 0


def main():
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    server_parser = subparsers.add_parser("server")
    server_parser.add_argument("--cert", required=True)
    server_parser.add_argument("--key", required=True)
    server_parser.add_argument("--host", default="127.0.0.1")
    server_parser.add_argument("--port-file", required=True)
    server_parser.add_argument("--result-file")
    server_parser.add_argument("--mode", required=True)
    server_parser.add_argument("--payload-size", type=int, default=4096)
    server_parser.add_argument("--expect-sni")
    server_parser.add_argument("--timeout", type=float, default=20.0)
    client_parser = subparsers.add_parser("client")
    client_parser.add_argument("--host", default="127.0.0.1")
    client_parser.add_argument("--port", type=int, required=True)
    client_parser.add_argument("--ca", required=True)
    client_parser.add_argument("--name", default="localhost")
    client_parser.add_argument("--result-file")
    client_parser.add_argument("--mode", required=True)
    client_parser.add_argument("--payload-size", type=int, default=4096)
    client_parser.add_argument("--timeout", type=float, default=20.0)
    args = parser.parse_args()
    if args.command == "server":
        return server(args)
    return client(args)


if __name__ == "__main__":
    sys.exit(main())
