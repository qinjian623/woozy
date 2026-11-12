#!/usr/bin/env python3
"""Independent Python ssl peer for the hosted TLS runtime tests.

The server binds only on a caller-supplied numeric loopback address, writes the
ephemeral port to --port-file, performs one TLS connection, and writes a JSON
result.  It never contacts an external service.  Payload bytes are deterministic
and contain NULs and arbitrary binary values; no real secret is used.
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
    descriptor, temporary = tempfile.mkstemp(prefix=".tls-result-", dir=directory)
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


def server(args):
    result = {
        "mode": args.mode,
        "handshake": "not-started",
        "sni": None,
        "bytes_sent": 0,
    }
    listener = None
    connection = None
    wrapped = None
    try:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(args.cert, args.key)

        def record_sni(ssl_socket, server_name, initial_context):
            del ssl_socket, initial_context
            result["sni"] = server_name

        context.set_servername_callback(record_sni)

        family = socket.AF_INET6 if ":" in args.host else socket.AF_INET
        listener = socket.socket(family, socket.SOCK_STREAM)
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        if family == socket.AF_INET6:
            listener.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
        listener.bind((args.host, 0))
        listener.listen(1)
        listener.settimeout(args.timeout)
        port = listener.getsockname()[1]
        with open(args.port_file, "w", encoding="ascii") as output:
            output.write(f"{port}\n")

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
                raise RuntimeError(
                    f"SNI mismatch: expected {expected!r}, got {result['sni']!r}"
                )

        data = payload(args.payload_size)
        if args.mode in ("clean", "truncate"):
            wrapped.sendall(data)
            result["bytes_sent"] = len(data)

        if args.mode == "clean":
            wrapped.settimeout(5.0)
            try:
                raw = wrapped.unwrap()
                result["unwrap"] = "ok"
                raw.close()
                wrapped = None
            except (ssl.SSLError, socket.timeout, OSError) as error:
                result["unwrap"] = f"{type(error).__name__}: {error}"
        elif args.mode == "truncate":
            # SSL shutdown is deliberately bypassed: FIN without close_notify.
            wrapped.shutdown(socket.SHUT_RDWR)
            wrapped.close()
            wrapped = None
            result["unwrap"] = "bypassed"
        elif args.mode == "close-after-handshake":
            wrapped.shutdown(socket.SHUT_RDWR)
            wrapped.close()
            wrapped = None
            result["unwrap"] = "bypassed"
        else:
            raise RuntimeError(f"unknown mode {args.mode}")

        result["ok"] = True
    except BaseException as error:  # Keep expected handshake rejections benign.
        result["ok"] = False
        result["error"] = f"{type(error).__name__}: {error}"
        if not isinstance(error, (ssl.SSLError, ConnectionError, OSError,
                                  RuntimeError, socket.timeout)):
            result["traceback"] = traceback.format_exc()
    finally:
        if wrapped is not None:
            try:
                wrapped.close()
            except OSError:
                pass
        if connection is not None:
            try:
                connection.close()
            except OSError:
                pass
        if listener is not None:
            try:
                listener.close()
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
    server_parser.add_argument(
        "--mode",
        choices=("clean", "truncate", "close-after-handshake"),
        default="clean",
    )
    server_parser.add_argument("--payload-size", type=int, default=4096)
    server_parser.add_argument("--expect-sni")
    server_parser.add_argument("--timeout", type=float, default=20.0)
    args = parser.parse_args()
    if args.command == "server":
        return server(args)
    return 2


if __name__ == "__main__":
    sys.exit(main())
