#!/usr/bin/env python3
"""Reproducible Linux x86_64 runner for the Woozy std/tls + std/https sources.

The runner builds an isolated addon archive, generates throwaway test
certificates under .build/certs, builds the real Woozy projects at O0 and O2,
and drives independent Python TLS/HTTP peers.  It never contacts an external
network and never writes credentials into tracked paths.

The fixed LLVM 22 toolchain and a private Woozy build are supplied by options.
Every child process has a finite timeout.  A missing certificate toolchain or
compiler is reported as an environment failure, not as a passing test.
"""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
PROJECTS = HERE / "projects"
PEER = HERE / "python_peer.py"
DEFAULT_CXX = Path(
    "/home/qin/Work/language-dev-20261103/toolchains/llvm-22.1.8/bin/clang++")
LEGACY_XML = Path(
    "/home/qin/Work/language-dev-20261103/toolchains/"
    "libxml2-legacy-2.13.9/usr/lib")
CHILDREN = []


def log(lines, message):
    print(message, flush=True)
    lines.append(message)


def run(argv, *, timeout, env=None, cwd=None):
    merged = dict(os.environ)
    merged["PYTHONDONTWRITEBYTECODE"] = "1"
    merged["PYTHONNOUSERSITE"] = "1"
    if LEGACY_XML.joinpath("libxml2.so.2").exists():
        previous = merged.get("LD_LIBRARY_PATH")
        merged["LD_LIBRARY_PATH"] = str(LEGACY_XML) + (
            ":" + previous if previous else "")
    if env:
        merged.update(env)
    try:
        process = subprocess.run(
            [str(item) for item in argv], cwd=str(cwd) if cwd else None,
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, env=merged, timeout=timeout,
            text=True, errors="backslashreplace")
    except subprocess.TimeoutExpired as error:
        raise AssertionError(
            "timeout after %ss: %s\nstdout:\n%s\nstderr:\n%s"
            % (timeout, " ".join(map(str, argv)),
               error.stdout or "", error.stderr or ""))
    return process


def ok(argv, *, timeout, env=None, cwd=None):
    result = run(argv, timeout=timeout, env=env, cwd=cwd)
    if result.returncode != 0:
        raise AssertionError(
            "command failed (%s): %s\nstdout:\n%s\nstderr:\n%s"
            % (result.returncode, " ".join(map(str, argv)),
               result.stdout, result.stderr))
    return result


def make_certs(build, openssl, lines):
    certs = build / "certs"
    if certs.exists():
        shutil.rmtree(certs)
    certs.mkdir(parents=True)
    san = certs / "san.ext"
    san.write_text(
        "basicConstraints=critical,CA:FALSE\n"
        "keyUsage=critical,digitalSignature,keyEncipherment\n"
        "extendedKeyUsage=serverAuth\n"
        "subjectAltName=DNS:localhost,IP:127.0.0.1,IP:::1\n",
        encoding="ascii")
    quiet = dict(stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    commands = [
        [openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-sha256",
         "-days", "2", "-keyout", certs / "ca.key", "-out", certs / "ca.pem",
         "-subj", "/CN=Woozy-Test-CA"],
        [openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-sha256",
         "-days", "2", "-keyout", certs / "other-ca.key",
         "-out", certs / "other-ca.pem", "-subj", "/CN=Woozy-Other-CA"],
        [openssl, "req", "-newkey", "rsa:2048", "-nodes", "-sha256",
         "-keyout", certs / "server.key", "-out", certs / "server.csr",
         "-subj", "/CN=localhost"],
        [openssl, "x509", "-req", "-in", certs / "server.csr",
         "-CA", certs / "ca.pem", "-CAkey", certs / "ca.key",
         "-CAcreateserial", "-out", certs / "server.pem", "-days", "2",
         "-sha256", "-extfile", san],
    ]
    for command in commands:
        try:
            result = subprocess.run(
                [str(item) for item in command], timeout=60, **quiet)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise AssertionError("certificate generation failed: %s" % error)
        if result.returncode != 0:
            raise AssertionError("certificate generation failed: %s"
                                 % " ".join(map(str, command)))
    (certs / "server-chain.pem").write_bytes(
        (certs / "server.pem").read_bytes() + (certs / "ca.pem").read_bytes())

    database = certs / "ca-db"
    database.mkdir()
    (database / "index.txt").write_text("", encoding="ascii")
    (database / "serial").write_text("1000\n", encoding="ascii")
    configuration = certs / "ca.cnf"
    configuration.write_text(
        "[ ca ]\n"
        "default_ca = CA_default\n"
        "[ CA_default ]\n"
        "dir = %s\n"
        "database = $dir/index.txt\n"
        "new_certs_dir = %s\n"
        "serial = $dir/serial\n"
        "certificate = %s\n"
        "private_key = %s\n"
        "default_md = sha256\n"
        "policy = policy_any\n"
        "[ policy_any ]\n"
        "commonName = supplied\n"
        % (database, certs, certs / "ca.pem", certs / "ca.key"),
        encoding="ascii")
    expired = subprocess.run(
        [openssl, "ca", "-batch", "-config", configuration,
         "-in", certs / "server.csr", "-out", certs / "server-expired.pem",
         "-startdate", "20200101000000Z", "-enddate", "20200102000000Z",
         "-extfile", san, "-notext"], timeout=60, **quiet)
    if expired.returncode != 0:
        raise AssertionError("expired certificate generation failed")
    (certs / "server-expired-chain.pem").write_bytes(
        (certs / "server-expired.pem").read_bytes()
        + (certs / "ca.pem").read_bytes())
    log(lines, "PASS certificates: generated throwaway CA/server/expired certs")


def build_addon(build, cxx, cc, lines):
    addon = build / "addon"
    if addon.exists():
        shutil.rmtree(addon)
    ok(["cmake", "-S", ROOT / "stdlib/tls/addon", "-B", addon, "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_CXX_COMPILER=" + str(cxx),
        "-DCMAKE_C_COMPILER=" + str(cc)], timeout=180)
    ok(["cmake", "--build", addon, "--target", "woozy_tls", "-j", "4"],
       timeout=300)
    ok(["cmake", "--build", addon, "--target", "woozy_tls_header_probe", "-j", "4"],
       timeout=120)
    ok([addon / "woozy_tls_header_probe"], timeout=30)
    log(lines, "PASS addon: libwoozy_tls.a and C11 header probe")


def build_project(cli, project, opt, out, env):
    if out.exists():
        shutil.rmtree(out)
    result = run([cli, "build", "--project", project, "--backend", "native",
                  "--opt", str(opt), "--out-dir", out, "--format", "json"],
                 timeout=180, env=env)
    if result.returncode != 0:
        raise AssertionError("build failed for %s O%d\n%s\n%s"
                             % (project, opt, result.stdout, result.stderr))
    return out / "program"


def stage_project(project, directory, addon):
    """Keep fixture sources in place and point its manifest at this run's addon."""
    text = project.read_text(encoding="utf-8")
    values = {
        "source_root": str(project.parent / "src"),
        "binding": str(ROOT / "stdlib/tls/bindings/tls.json"),
        "library_dirs": [str(addon)],
    }
    for key, value in values.items():
        text, count = re.subn(
            r"(?m)^" + key + r"\s*=.*$",
            lambda _match: key + " = " + json.dumps(value), text)
        if count != 1:
            raise AssertionError("fixture must declare exactly one " + key)
    directory.mkdir(parents=True, exist_ok=True)
    manifest = directory / "lang.toml"
    manifest.write_text(text, encoding="utf-8")
    return manifest


def start_python_server(build, python, mode, certs, payload_size, expect_sni,
                        tag):
    directory = build / "run"
    directory.mkdir(parents=True, exist_ok=True)
    port_file = directory / (tag + ".port")
    result_file = directory / (tag + ".json")
    log_file = directory / (tag + ".log")
    for path in (port_file, result_file, log_file):
        if path.exists():
            path.unlink()
    handle = log_file.open("w", encoding="utf-8")
    process = subprocess.Popen(
        [str(python), str(PEER), "server",
         "--cert", str(certs / "server-chain.pem"),
         "--key", str(certs / "server.key"),
         "--host", "127.0.0.1", "--port-file", str(port_file),
         "--result-file", str(result_file), "--mode", mode,
         "--payload-size", str(payload_size), "--expect-sni", expect_sni],
        stdin=subprocess.DEVNULL, stdout=handle, stderr=subprocess.STDOUT,
        env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1"))
    CHILDREN.append(process)
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        if port_file.exists() and port_file.stat().st_size:
            break
        if process.poll() is not None:
            handle.close()
            raise AssertionError("Python server %s exited early\n%s"
                                 % (tag, log_file.read_text(errors="replace")))
        time.sleep(0.02)
    else:
        process.kill()
        handle.close()
        raise AssertionError("Python server %s did not publish a port" % tag)
    port = int(port_file.read_text().strip())
    return process, handle, port, result_file, log_file


def finish_python_server(process, handle, result_file, log_file, lines, tag, require_ok=True):
    try:
        status = process.wait(timeout=45)
    except subprocess.TimeoutExpired:
        process.kill()
        status = process.wait(timeout=5)
        raise AssertionError("Python server %s timed out\n%s"
                             % (tag, log_file.read_text(errors="replace")))
    handle.close()
    if process in CHILDREN:
        CHILDREN.remove(process)
    if status != 0:
        raise AssertionError("Python server %s failed (%s)\n%s"
                             % (tag, status, log_file.read_text(errors="replace")))
    result = json.loads(result_file.read_text(encoding="utf-8"))
    if require_ok and not result.get("ok"):
        raise AssertionError("Python server %s rejected connection: %s"
                             % (tag, json.dumps(result, sort_keys=True)))
    if not require_ok and result.get("ok"):
        raise AssertionError("Python server %s unexpectedly succeeded: %s"
                             % (tag, json.dumps(result, sort_keys=True)))
    log(lines, "  python %s: %s" % (tag, json.dumps(result, sort_keys=True)))
    return result


def start_wz_server(program, build, tag, arguments):
    directory = build / "run"
    directory.mkdir(parents=True, exist_ok=True)
    log_file = directory / (tag + ".log")
    if log_file.exists():
        log_file.unlink()
    handle = log_file.open("w", encoding="utf-8")
    process = subprocess.Popen(
        [str(program)] + [str(item) for item in arguments],
        stdin=subprocess.DEVNULL, stdout=handle, stderr=subprocess.STDOUT)
    CHILDREN.append(process)
    deadline = time.monotonic() + 20
    port = None
    while time.monotonic() < deadline:
        text = log_file.read_text(encoding="utf-8", errors="replace")
        for line in text.splitlines():
            if line.startswith("LISTENING "):
                port = int(line.split()[1])
                break
        if port is not None:
            break
        if process.poll() is not None:
            handle.close()
            raise AssertionError("Woozy server %s exited early\n%s"
                                 % (tag, text))
        time.sleep(0.02)
    if port is None:
        process.kill()
        handle.close()
        raise AssertionError("Woozy server %s did not publish a port" % tag)
    return process, handle, port, log_file


def finish_wz_server(process, handle, log_file, lines, tag, expected):
    try:
        status = process.wait(timeout=45)
    except subprocess.TimeoutExpired:
        process.kill()
        status = process.wait(timeout=5)
        raise AssertionError("Woozy server %s timed out\n%s"
                             % (tag, log_file.read_text(errors="replace")))
    handle.close()
    if process in CHILDREN:
        CHILDREN.remove(process)
    text = log_file.read_text(encoding="utf-8", errors="replace")
    if status != 0:
        raise AssertionError("Woozy server %s failed (%s)\n%s"
                             % (tag, status, text))
    for needle in expected:
        if needle not in text:
            raise AssertionError("Woozy server %s missing %r\n%s"
                                 % (tag, needle, text))
    log(lines, "  woozy %s: %s" % (tag, " | ".join(
        line for line in text.splitlines() if line.startswith("RESULT"))))


def run_client(program, arguments, timeout=60, env=None):
    result = run([program] + [str(item) for item in arguments],
                 timeout=timeout, env=env)
    text = result.stdout + result.stderr
    if result.returncode != 0:
        raise AssertionError("Woozy client failed (%s): %s\n%s"
                             % (result.returncode,
                                " ".join(map(str, arguments)), text))
    return result


def check_elf(program, lines, tag, dynamic):
    result = ok(["readelf", "-d", program], timeout=30)
    needed = [line for line in result.stdout.splitlines() if "NEEDED" in line]
    interp = ok(["readelf", "-l", program], timeout=30).stdout
    if dynamic:
        joined = "\n".join(needed)
        for library in ("libssl.so.3", "libcrypto.so.3", "libstdc++.so.6"):
            if library not in joined:
                raise AssertionError("%s lacks explicit %s dependency"
                                     % (tag, library))
        log(lines, "PASS elf %s: explicit OpenSSL/C++ dependencies" % tag)
    else:
        if needed or "INTERP" in interp:
            raise AssertionError("%s is not a static freestanding ELF" % tag)
        log(lines, "PASS elf %s: no INTERP/NEEDED" % tag)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", type=Path,
                        default=ROOT / "build/woozy-stdlib-native/woozyc")
    parser.add_argument("--cxx", type=Path, default=DEFAULT_CXX)
    parser.add_argument("--cc", type=Path, default=None)
    parser.add_argument("--python", default=sys.executable or "python3")
    parser.add_argument("--openssl", default="openssl")
    parser.add_argument("--out", type=Path, default=HERE / ".build",
                        help="test outputs, addon and throwaway certificates")
    parser.add_argument("--http-regression", action="store_true",
                        help="also run tests/native/http/run_http.py")
    args = parser.parse_args()
    args.cli = args.cli.resolve()
    if args.cc is None:
        args.cc = args.cxx.with_name("clang")
    for path, label in ((args.cli, "Woozy compiler"), (args.cxx, "clang++"),
                        (args.cc, "clang")):
        if not Path(path).exists():
            raise SystemExit("missing %s: %s" % (label, path))
    if shutil.which(str(args.python)) is None:
        raise SystemExit("missing Python: %s" % args.python)
    if shutil.which(str(args.openssl)) is None:
        raise SystemExit("missing openssl CLI: %s" % args.openssl)

    build = args.out.resolve()
    build.mkdir(parents=True, exist_ok=True)
    lines = []
    report = {"status": "failed", "runs": [], "environment": {}}
    paths = {
        "tls_client": PROJECTS / "tls_client/lang.toml",
        "tls_server": PROJECTS / "tls_server/lang.toml",
        "https_client": PROJECTS / "https_client/lang.toml",
        "https_server": PROJECTS / "https_server/lang.toml",
    }
    try:
        version = ok([args.cli, "--version"], timeout=30).stdout.strip()
        cxx_version = ok([args.cxx, "--version"], timeout=30).stdout.splitlines()[0]
        python_version = ok([args.python, "-c",
                             "import ssl,sys;print(sys.version.split()[0],ssl.OPENSSL_VERSION)"],
                            timeout=30).stdout.strip()
        report["environment"] = {
            "compiler": version, "cxx": cxx_version,
            "python": python_version,
            "root": str(ROOT),
        }
        log(lines, "Woozy NET02 source TLS/HTTPS runner")
        log(lines, "compiler: %s (%s)" % (args.cli, version))
        log(lines, "cxx: %s" % cxx_version)
        log(lines, "python: %s" % python_version)
        certs = build / "certs"
        make_certs(build, args.openssl, lines)
        build_addon(build, args.cxx, args.cc, lines)
        paths = {name: stage_project(project, build / "projects" / name,
                                     build / "addon")
                 for name, project in paths.items()}
        env = {"LD_LIBRARY_PATH": str(LEGACY_XML)} if LEGACY_XML.exists() else {}

        for opt in (0, 2):
            log(lines, "=== O%d ===" % opt)
            outputs = {}
            for name, project in paths.items():
                outputs[name] = build_project(
                    args.cli, project, opt,
                    build / "programs" / ("%s-O%d" % (name, opt)), env)
            check_elf(outputs["tls_client"], lines, "tls-client-O%d" % opt, True)
            check_elf(outputs["https_client"], lines, "https-client-O%d" % opt, True)

            client = outputs["tls_client"]
            run_client(client, ["invalid", certs / "ca.pem"])
            run_client(client, ["resources", certs / "ca.pem"])
            log(lines, "PASS invalid: empty/NUL hostname rejected; resources: 256 stream copies")

            cases = [
                ("clean", "tls-clean", "localhost", "localhost", "dns", "ok",
                 65536, None),
                ("truncate", "tls-truncate", "localhost", "localhost", "dns",
                 "truncate", 65536, None),
                ("wrong-host", "tls-clean", "wrong.example", "wrong.example",
                 "dns", "certificate", 1024, None),
                ("wrong-ca", "tls-clean", "localhost", "localhost", "dns",
                 "certificate", 1024, certs / "other-ca.pem"),
                ("expired", "tls-clean", "localhost", "localhost", "dns",
                 "certificate", 1024, None),
                ("ipv4", "tls-clean", "127.0.0.1", "none", "ip", "ok",
                 65536, None),
                ("auto-ip", "tls-clean", "127.0.0.1", "none", "auto",
                 "ok", 65536, None),
            ]
            for tag, mode, name, sni, kind, expectation, size, ca in cases:
                server_cert = certs / "server-chain.pem"
                if tag == "expired":
                    server_cert = certs / "server-expired-chain.pem"
                # The helper always uses server-chain.pem; override by
                # temporarily starting the expired server with the same files.
                if tag == "expired":
                    directory = build / "run"
                    directory.mkdir(parents=True, exist_ok=True)
                    port_file = directory / (tag + "-O%d.port" % opt)
                    result_file = directory / (tag + "-O%d.json" % opt)
                    for path in (port_file, result_file):
                        if path.exists():
                            path.unlink()
                    handle = (directory / (tag + "-O%d.log" % opt)).open("w")
                    process = subprocess.Popen(
                        [str(args.python), str(PEER), "server",
                         "--cert", str(server_cert),
                         "--key", str(certs / "server.key"),
                         "--host", "127.0.0.1", "--port-file", str(port_file),
                         "--result-file", str(result_file),
                         "--mode", mode, "--payload-size", str(size),
                         "--expect-sni", sni],
                        stdin=subprocess.DEVNULL, stdout=handle,
                        stderr=subprocess.STDOUT)
                    CHILDREN.append(process)
                    deadline = time.monotonic() + 20
                    while time.monotonic() < deadline and not (
                            port_file.exists() and port_file.stat().st_size):
                        if process.poll() is not None:
                            break
                        time.sleep(0.02)
                    if not port_file.exists():
                        process.kill()
                        handle.close()
                        raise AssertionError("expired server did not start")
                    port = int(port_file.read_text().strip())
                    client_ca = ca or certs / "ca.pem"
                    run_client(client, ["python", "127.0.0.1", port, client_ca,
                                        name, kind, expectation, size])
                    handle.close()
                    if process.wait(timeout=30) != 0:
                        raise AssertionError("expired Python peer failed")
                    if process in CHILDREN:
                        CHILDREN.remove(process)
                    result = json.loads(result_file.read_text())
                    if result.get("ok"):
                        raise AssertionError("expired peer unexpectedly succeeded")
                    log(lines, "PASS TLS %s O%d: expected certificate rejection"
                        % (tag, opt))
                    continue
                process, handle, port, result_file, log_file = start_python_server(
                    build, args.python, mode, certs, size, sni,
                    "%s-O%d" % (tag, opt))
                client_ca = ca or certs / "ca.pem"
                run_client(client, ["python", "127.0.0.1", port, client_ca,
                                    name, kind, expectation, size])
                finish_python_server(process, handle, result_file, log_file,
                                     lines, "%s-O%d" % (tag, opt),
                                     require_ok=(expectation != "certificate"))
                log(lines, "PASS TLS client-Python %s O%d" % (tag, opt))

            # Default trust is exercised through OpenSSL's SSL_CERT_FILE.
            process, handle, port, result_file, log_file = start_python_server(
                build, args.python, "tls-clean", certs, 4096, "localhost",
                "default-trust-O%d" % opt)
            run_client(client, ["python", "127.0.0.1", port, "default",
                                "localhost", "dns", "ok", 4096],
                       env={"SSL_CERT_FILE": str(certs / "ca.pem")})
            finish_python_server(process, handle, result_file, log_file,
                                 lines, "default-trust-O%d" % opt)
            log(lines, "PASS TLS default trust O%d" % opt)

            # The same server is not trusted by the platform default store.
            process, handle, port, result_file, log_file = start_python_server(
                build, args.python, "tls-clean", certs, 1024, "localhost",
                "default-untrusted-O%d" % opt)
            run_client(client, ["python", "127.0.0.1", port, "default",
                                "localhost", "dns", "certificate", 1024],
                       env={"SSL_CERT_FILE": "/dev/null"})
            finish_python_server(process, handle, result_file, log_file,
                                 lines, "default-untrusted-O%d" % opt,
                                 require_ok=False)
            log(lines, "PASS TLS default untrusted rejection O%d" % opt)

            # Non-blocking WANT/partial write against an independent echo peer.
            process, handle, port, result_file, log_file = start_python_server(
                build, args.python, "tls-echo", certs, 1048576, "localhost",
                "nonblock-O%d" % opt)
            nonblock = run_client(client, ["nonblock", "127.0.0.1", port,
                                           certs / "ca.pem", "localhost",
                                           1048576], timeout=90)
            if "want" not in nonblock.stdout or "PARTIAL yes" not in nonblock.stdout:
                raise AssertionError("nonblocking test did not observe retry/partial write\n"
                                     + nonblock.stdout)
            finish_python_server(process, handle, result_file, log_file,
                                 lines, "nonblock-O%d" % opt)
            log(lines, "PASS TLS nonblocking WANT/partial O%d" % opt)

            # Wz TLS server <- Wz TLS client, then independent Python client.
            server = outputs["tls_server"]
            process, handle, port, server_log = start_wz_server(
                server, build, "tls-server-wz-O%d" % opt,
                [certs / "server-chain.pem", certs / "server.key",
                 "127.0.0.1", 0, 1048576])
            wz_client = run_client(client, ["echo", "127.0.0.1", port,
                                            certs / "ca.pem", "localhost",
                                            1048576], timeout=90)
            if "ECHO ok" not in wz_client.stdout:
                raise AssertionError("Wz-Wz TLS echo missing\n" + wz_client.stdout)
            finish_wz_server(process, handle, server_log, lines,
                             "tls-server-wz-O%d" % opt,
                             ["HANDSHAKE ok", "RESULT ok"])
            log(lines, "PASS TLS Wz-to-Wz O%d" % opt)

            process, handle, port, server_log = start_wz_server(
                server, build, "tls-server-python-O%d" % opt,
                [certs / "server-chain.pem", certs / "server.key",
                 "127.0.0.1", 0, 65536])
            py_result = build / "run" / ("tls-python-client-O%d.json" % opt)
            if py_result.exists():
                py_result.unlink()
            ok([args.python, PEER, "client", "--host", "127.0.0.1",
                "--port", port, "--ca", certs / "ca.pem",
                "--name", "localhost", "--mode", "tls-echo",
                "--payload-size", "65536", "--result-file", py_result],
               timeout=60)
            py_client = json.loads(py_result.read_text())
            if not py_client.get("ok"):
                raise AssertionError("Python TLS client failed: %s"
                                     % json.dumps(py_client, sort_keys=True))
            finish_wz_server(process, handle, server_log, lines,
                             "tls-server-python-O%d" % opt,
                             ["HANDSHAKE ok", "RESULT ok"])
            log(lines, "PASS TLS Wz-server/Python-client O%d" % opt)

            # Wz HTTPS client <- independent Python HTTPS server.
            https_client = outputs["https_client"]
            for tag, mode in (("https-single-python", "https-single"),
                              ("https-reuse-python", "https-reuse")):
                process, handle, port, result_file, log_file = start_python_server(
                    build, args.python, mode, certs, 4096, "none",
                    "%s-O%d" % (tag, opt))
                expected = "single" if mode == "https-single" else "reuse"
                run_client(https_client, ["127.0.0.1", port,
                                          certs / "ca.pem", "localhost",
                                          expected], timeout=60)
                finish_python_server(process, handle, result_file, log_file,
                                     lines, "%s-O%d" % (tag, opt))
                log(lines, "PASS HTTPS client-Python %s O%d" % (tag, opt))

            # Wz HTTPS server <- independent Python HTTPS client, then Wz client.
            https_server = outputs["https_server"]
            process, handle, port, server_log = start_wz_server(
                https_server, build, "https-server-python-O%d" % opt,
                [certs / "server-chain.pem", certs / "server.key",
                 "127.0.0.1", 0])
            py_result = build / "run" / ("https-python-client-O%d.json" % opt)
            if py_result.exists():
                py_result.unlink()
            ok([args.python, PEER, "client", "--host", "127.0.0.1",
                "--port", port, "--ca", certs / "ca.pem",
                "--name", "localhost", "--mode", "https-chunked",
                "--result-file", py_result], timeout=60)
            py_client = json.loads(py_result.read_text())
            if not py_client.get("ok"):
                raise AssertionError("Python HTTPS client failed: %s"
                                     % json.dumps(py_client, sort_keys=True))
            finish_wz_server(process, handle, server_log, lines,
                             "https-server-python-O%d" % opt,
                             ["REQUEST method=POST target=/chunk",
                              "RESULT ok served=2"])
            log(lines, "PASS HTTPS Wz-server/Python-client chunked O%d" % opt)

            process, handle, port, server_log = start_wz_server(
                https_server, build, "https-server-wz-O%d" % opt,
                [certs / "server-chain.pem", certs / "server.key",
                 "127.0.0.1", 0])
            wz_client = run_client(https_client, ["127.0.0.1", port,
                                                  certs / "ca.pem",
                                                  "localhost", "reuse"],
                                   timeout=60)
            if "RESULT ok reuse" not in wz_client.stdout:
                raise AssertionError("Wz-Wz HTTPS reuse missing\n" + wz_client.stdout)
            finish_wz_server(process, handle, server_log, lines,
                             "https-server-wz-O%d" % opt,
                             ["RESULT ok served=2"])
            log(lines, "PASS HTTPS Wz-to-Wz reuse O%d" % opt)

            report["runs"].append({"opt": opt, "status": "passed"})
            log(lines, "PASS O%d source TLS/HTTPS suite" % opt)

        focused = run([
            args.python, Path(__file__).resolve().parent / "focused_cleanup.py",
            "--cli", args.cli, "--cxx", args.cxx, "--cc", args.cc,
            "--addon", build / "addon", "--certs", build / "certs",
            "--out", build / "focused",
        ], timeout=300, env=env)
        if focused.returncode != 0:
            raise AssertionError("focused cleanup regression failed\n%s\n%s"
                                 % (focused.stdout, focused.stderr))
        for line in focused.stdout.splitlines():
            if line.startswith("PASS "):
                log(lines, line)
        report["focused_cleanup"] = "passed"

        tokens = run([
            args.python, HERE / "token_checks.py", "--cli", args.cli,
            "--cxx", args.cxx, "--addon", build / "addon",
            "--out", build / "tokens",
        ], timeout=420, env=env)
        if tokens.returncode != 0:
            raise AssertionError("token lifetime checks failed\n%s\n%s"
                                 % (tokens.stdout, tokens.stderr))
        for line in tokens.stdout.splitlines():
            if line.startswith("PASS "):
                log(lines, line)
        report["token_lifetime"] = "passed"

        if args.http_regression:
            out = build / "http-results"
            result = run([args.python, ROOT / "tests/native/http/run_http.py",
                          "--cli", args.cli, "--out", out], timeout=600, env=env)
            if result.returncode != 0:
                raise AssertionError("original HTTP suite failed\n%s\n%s"
                                     % (result.stdout, result.stderr))
            log(lines, "PASS HTTP regression: original std/http suite O0/O2")

        # Core static ELF remains freestanding: build a non-TLS project with
        # the same compiler and verify no dynamic loader dependency.
        core_out = build / "programs" / "core-freestanding"
        if core_out.exists():
            shutil.rmtree(core_out)
        core = run([args.cli, "build", "--project",
                    ROOT / "tests/fixtures/core/lang.toml", "--backend",
                    "native", "--opt", "0", "--out-dir", core_out],
                   timeout=120, env=env)
        if core.returncode != 0:
            raise AssertionError("freestanding core build failed\n%s\n%s"
                                 % (core.stdout, core.stderr))
        check_elf(core_out / "program", lines, "freestanding-core", False)

        report["status"] = "passed"
        return 0
    except BaseException as error:
        report["error"] = "%s: %s" % (type(error).__name__, error)
        log(lines, "FAIL: %s" % report["error"])
        return 1
    finally:
        for child in list(CHILDREN):
            if child.poll() is None:
                child.kill()
                try:
                    child.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    pass
            try:
                CHILDREN.remove(child)
            except ValueError:
                pass
        (build / "summary.txt").write_text("\n".join(lines) + "\n",
                                           encoding="utf-8")
        (build / "results.json").write_text(
            json.dumps(report, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8")
        print("summary: %s" % (build / "summary.txt"), flush=True)


if __name__ == "__main__":
    sys.exit(main())
