#!/usr/bin/env python3
"""Focused regression for HTTPS request error cleanup.

Two defects are covered with real Wz source, real Python ssl peers and
instrumented native link boundaries:

1. A valid 204 response followed by TCP truncation must close the owned socket
   exactly once.  A wrapper opens an unrelated /dev/null descriptor after the
   first close, so a second close is observable as an fd-reuse failure.
2. An HTTP parser failure with the peer still holding the connection open must
   return the original HTTPS error promptly and must not call TLS shutdown.
   A fatal TLS record must also skip SSL_shutdown.

The script expects an already-built addon and runner-generated certificates:
pass --addon and --certs (normally tests/native/tls_source/.build/addon and
.build/certs).  All generated projects, wrappers, logs and JSON stay under
--out and are ignored by git.
"""

import argparse
import json
import os
from pathlib import Path
import shutil
import socket
import ssl
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
PROJECT_SOURCE = HERE / "projects/request_probe/src"
CLOSE_WRAPPER = HERE / "focused_close_wrapper.c"
TLS_WRAPPER = HERE / "focused_tls_wrapper.c"
TOOLCHAINS = Path(
    os.environ.get("LANG_TOOLCHAINS")
    or os.environ.get("WOOZY_TOOLCHAIN_ROOT")
    or ROOT.parent / "toolchains")
LEGACY_XML = TOOLCHAINS / "libxml2-legacy-2.13.9/usr/lib"
DEFAULT_CXX = TOOLCHAINS / "llvm-22.1.8/bin/clang++"


def environment():
    result = dict(os.environ)
    result["PYTHONDONTWRITEBYTECODE"] = "1"
    result["PYTHONNOUSERSITE"] = "1"
    if LEGACY_XML.joinpath("libxml2.so.2").exists():
        previous = result.get("LD_LIBRARY_PATH")
        result["LD_LIBRARY_PATH"] = str(LEGACY_XML) + (
            ":" + previous if previous else "")
    return result


def run(argv, timeout):
    process = subprocess.run(
        [str(item) for item in argv], stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=environment(),
        timeout=timeout, text=True, errors="backslashreplace")
    if process.returncode != 0:
        raise AssertionError(
            "command failed (%s): %s\nstdout:\n%s\nstderr:\n%s"
            % (process.returncode, " ".join(map(str, argv)),
               process.stdout, process.stderr))
    return process


def build_project(cli, cc, opt, out, manifest):
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    captured = out / "c_adapter.c"
    stop = threading.Event()

    def watch():
        while not stop.is_set():
            for candidate in out.glob(".woozyc-native-*/c_adapter.c"):
                try:
                    data = candidate.read_bytes()
                    if data:
                        captured.write_bytes(data)
                except FileNotFoundError:
                    pass
            stop.wait(0.002)

    worker = threading.Thread(target=watch)
    worker.start()
    try:
        run([cli, "build", "--project", manifest, "--backend", "native",
             "--opt", str(opt), "--out-dir", out, "--format", "json"], 180)
    finally:
        stop.set()
        worker.join()
    if not captured.exists() or not captured.stat().st_size:
        raise AssertionError("could not capture generated C adapter")
    adapter = out / "c_adapter.o"
    run([cc, "--target=x86_64-unknown-linux-gnu", "-std=c11", "-O2",
         "-fPIC", "-c", captured, "-o", adapter], 60)
    return out / "program.o", adapter, out / "program"


def compile_wrappers(cc, out):
    close_object = out / "focused_close_wrapper.o"
    tls_object = out / "focused_tls_wrapper.o"
    run([cc, "-O0", "-c", CLOSE_WRAPPER, "-o", close_object], 30)
    run([cc, "-std=gnu11", "-O0", "-I", ROOT / "runtime/hosted", "-c",
         TLS_WRAPPER, "-o", tls_object], 30)
    return close_object, tls_object


def link_instrumented(cli, cc, out, opt, adapter, close_object, tls_object):
    runtime = cli.parent / "native/liblang_hosted_runtime.a"
    if not runtime.exists():
        raise AssertionError("hosted runtime archive not found: %s" % runtime)
    binary = out / ("focused-O%d" % opt)
    run([cc, "--target=x86_64-unknown-linux-gnu",
         "-Wl,-u,lang_hosted_startup_v1", "-pthread",
         out / "program.o", adapter, close_object, tls_object, runtime,
         "-L" + str(ARGS.addon), "-lwoozy_tls", "-lssl", "-lcrypto",
         "-lstdc++", "-Wl,--wrap=lang_os_close_v1",
         "-Wl,--wrap=wts_shutdown", "-Wl,--wrap=wts_transport_read",
         "-o", binary], 60)
    return binary


def listener():
    value = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    value.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    value.bind(("127.0.0.1", 0))
    value.listen(1)
    value.settimeout(8)
    return value


def server_context():
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.load_cert_chain(ARGS.certs / "server-chain.pem",
                            ARGS.certs / "server.key")
    return context


def read_request_head(stream):
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = stream.recv(4096)
        if not chunk:
            raise EOFError("client closed before request head")
        data += chunk
    return data


def close_case(binary, opt, out):
    listen = listener()
    port = listen.getsockname()[1]
    errors = []

    def serve():
        connection = None
        wrapped = None
        try:
            connection, _ = listen.accept()
            connection.settimeout(5)
            wrapped = server_context().wrap_socket(connection, server_side=True)
            connection = None
            read_request_head(wrapped)
            wrapped.sendall(b"HTTP/1.1 204 No Content\r\n\r\n")
            raw = wrapped.detach()
            wrapped = None
            os.close(raw)  # TCP close without TLS close_notify
        except BaseException as error:
            errors.append("%s: %s" % (type(error).__name__, error))
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
            listen.close()

    worker = threading.Thread(target=serve)
    worker.start()
    try:
        process = run([binary, port, ARGS.certs / "ca.pem"], 10)
    finally:
        worker.join(8)
    record = {
        "opt": opt,
        "case": "valid-204-then-tcp-truncation",
        "exit": process.returncode,
        "stdout": process.stdout,
        "stderr": process.stderr,
        "server_errors": errors,
    }
    (out / ("close-O%d.json" % opt)).write_text(
        json.dumps(record, indent=2) + "\n", encoding="utf-8")
    if errors:
        raise AssertionError("close-case server failed: %s" % errors)
    if "HTTPS_ERROR" not in process.stdout:
        raise AssertionError("close-case did not return HTTPS_ERROR: %s" % record)
    if process.stderr.count("CLOSE:") != 1:
        raise AssertionError("owned socket was not closed exactly once: %s"
                             % record)
    if "REPLACEMENT_CLOSED" in process.stderr:
        raise AssertionError("second close hit a reused descriptor: %s" % record)
    if "FINAL replacement_alive=0" not in process.stderr:
        raise AssertionError("fd-reuse replacement did not survive: %s" % record)
    return record


def held_peer_case(binary, opt, mode, out):
    listen = listener()
    port = listen.getsockname()[1]
    release = threading.Event()
    sent = threading.Event()
    errors = []

    def serve():
        connection = None
        wrapped = None
        raw = -1
        try:
            connection, _ = listen.accept()
            connection.settimeout(5)
            wrapped = server_context().wrap_socket(connection, server_side=True)
            connection = None
            read_request_head(wrapped)
            if mode == "http-error-idle-peer":
                wrapped.sendall(
                    b"HTTP/1.1 200 OK\r\nContent-Length: x\r\n\r\n")
            else:
                raw = wrapped.detach()
                wrapped = None
                os.write(raw, b"\x17\x03\x03\x00\x01\x00")
            sent.set()
            release.wait(5)
        except BaseException as error:
            errors.append("%s: %s" % (type(error).__name__, error))
        finally:
            if raw >= 0:
                os.close(raw)
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
            listen.close()

    worker = threading.Thread(target=serve)
    worker.start()
    started = time.monotonic()
    timed_out = False
    try:
        process = subprocess.run(
            [str(binary), str(port), str(ARGS.certs / "ca.pem")],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, env=environment(), timeout=2.0,
            text=True, errors="backslashreplace")
    except subprocess.TimeoutExpired as error:
        timed_out = True
        process = None
        stdout = (error.stdout or b"").decode("utf-8", "backslashreplace")
        stderr = (error.stderr or b"").decode("utf-8", "backslashreplace")
        exit_code = None
    else:
        stdout = process.stdout
        stderr = process.stderr
        exit_code = process.returncode
    finally:
        release.set()
        worker.join(6)
    elapsed = time.monotonic() - started
    record = {
        "opt": opt,
        "case": mode,
        "timed_out": timed_out,
        "exit": exit_code,
        "stdout": stdout,
        "stderr": stderr,
        "elapsed_seconds": elapsed,
        "server_sent": sent.is_set(),
        "server_errors": errors,
    }
    (out / ("%s-O%d.json" % (mode, opt))).write_text(
        json.dumps(record, indent=2) + "\n", encoding="utf-8")
    if not sent.is_set() or errors:
        raise AssertionError("held-peer server failed: %s" % record)
    if timed_out:
        raise AssertionError("error cleanup waited for the peer: %s" % record)
    if exit_code != 0 or "HTTPS_ERROR" not in stdout:
        raise AssertionError("error cleanup did not return the error: %s" % record)
    if "TLS_SHUTDOWN" in stderr:
        raise AssertionError("error cleanup attempted TLS shutdown: %s" % record)
    if process.stderr.count("CLOSE:") != 1:
        raise AssertionError("error cleanup close count is not exactly one: %s"
                             % record)
    if mode == "fatal-tls-error":
        if "TLS_READ_ERROR category=5 ssl_error=1" not in stderr:
            raise AssertionError("fatal TLS category was not observed: %s"
                                 % record)
    return record


def main():
    global ARGS
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", type=Path,
                        default=ROOT / "build/woozy-stdlib-native/woozyc")
    parser.add_argument("--cxx", type=Path, default=DEFAULT_CXX)
    parser.add_argument("--cc", type=Path, default=None)
    parser.add_argument("--addon", type=Path,
                        default=HERE / ".build/addon")
    parser.add_argument("--certs", type=Path,
                        default=HERE / ".build/certs")
    parser.add_argument("--out", type=Path,
                        default=HERE / ".build/focused")
    ARGS = parser.parse_args()
    ARGS.cli = ARGS.cli.resolve()
    if ARGS.cc is None:
        ARGS.cc = ARGS.cxx.with_name("clang")
    for path, label in ((ARGS.cli, "compiler"), (ARGS.cc, "C compiler"),
                        (ARGS.addon / "libwoozy_tls.a", "addon"),
                        (ARGS.certs / "server-chain.pem", "certificate")):
        if not path.exists():
            raise SystemExit("missing %s: %s" % (label, path))
    ARGS.out.mkdir(parents=True, exist_ok=True)
    project = ARGS.out / "project"
    project.mkdir(parents=True, exist_ok=True)
    manifest = project / "lang.toml"
    manifest.write_text(
        'schema_version=1\n'
        '[package]\n'
        'name="focused_request_probe"\n'
        'source_root="' + str(PROJECT_SOURCE) + '"\n'
        'entry="app"\n'
        '[build]\n'
        'backend="native"\n'
        'word_bits=64\n'
        '[build.native]\n'
        'runtime="hosted"\n'
        '[dependencies.woozy_tls]\n'
        'kind="native"\n'
        'binding="' + str(ROOT / "stdlib/tls/bindings/tls.json") + '"\n'
        'libraries=["ssl","crypto","stdc++"]\n'
        'library_dirs=["' + str(ARGS.addon) + '"]\n',
        encoding="utf-8")
    close_object, tls_object = compile_wrappers(ARGS.cc, ARGS.out)
    records = []
    for opt in (0, 2):
        out = ARGS.out / ("O%d" % opt)
        out.mkdir(parents=True, exist_ok=True)
        _, adapter, normal = build_project(ARGS.cli, ARGS.cc, opt, out, manifest)
        binary = link_instrumented(ARGS.cli, ARGS.cc, out, opt, adapter,
                                   close_object, tls_object)
        records.append(close_case(binary, opt, out))
        records.append(held_peer_case(binary, opt, "http-error-idle-peer", out))
        records.append(held_peer_case(binary, opt, "fatal-tls-error", out))
    report = {"status": "passed", "records": records,
              "addon": str(ARGS.addon), "certs": str(ARGS.certs)}
    (ARGS.out / "focused-cleanup.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8")
    for record in records:
        print("PASS focused %s O%d: exit=%s stdout=%s"
              % (record["case"], record["opt"], record["exit"],
                 record["stdout"].strip()))
    print("focused report: %s" % (ARGS.out / "focused-cleanup.json"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
