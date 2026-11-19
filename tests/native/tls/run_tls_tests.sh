#!/usr/bin/env bash
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
TOOLCHAINS=${LANG_TOOLCHAINS:-$ROOT/../toolchains}
BUILD="$HERE/.build"
CERTS="$BUILD/certs"
LOGS="$BUILD/logs"
RUN="$BUILD/run"
SUMMARY="$BUILD/summary.txt"

CXX=${CXX:-$TOOLCHAINS/llvm-22.1.8/bin/clang++}
CC=${CC:-$(dirname "$CXX")/clang}
PYTHON=${PYTHON:-python3}
OPENSSL=${OPENSSL:-openssl}

PY_PID=0
SERVER_PID=0
PORTFILE=
RESULTFILE=

cleanup() {
  if [ "$PY_PID" -ne 0 ] && kill -0 "$PY_PID" 2>/dev/null; then
    kill "$PY_PID" 2>/dev/null || true
    wait "$PY_PID" 2>/dev/null || true
  fi
  if [ "$SERVER_PID" -ne 0 ] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

rm -rf "$BUILD"
mkdir -p "$CERTS" "$LOGS" "$RUN"

log() {
  printf '%s\n' "$*" | tee -a "$SUMMARY"
}

die() {
  log "FAIL: $*"
  exit 1
}

wait_for_file() {
  local file=$1
  local pid=$2
  local description=$3
  local index
  for index in $(seq 1 500); do
    if [ -s "$file" ]; then
      return 0
    fi
    if ! kill -0 "$pid" 2>/dev/null; then
      wait "$pid" 2>/dev/null || true
      die "$description exited before producing $file"
    fi
    sleep 0.02
  done
  die "$description did not produce $file"
}

run_case() {
  local name=$1
  local seconds=$2
  shift 2
  local output="$LOGS/$name.log"
  if timeout "$seconds" "$@" >"$output" 2>&1; then
    cat "$output" | tee -a "$SUMMARY"
  else
    local status=$?
    cat "$output" | tee -a "$SUMMARY" || true
    die "$name failed with exit status $status"
  fi
}

make_certs() {
  local dir=$1
  mkdir -p "$dir"
  cat >"$dir/san.ext" <<'EOF'
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=DNS:localhost,IP:127.0.0.1,IP:::1
EOF
  "$OPENSSL" req -x509 -newkey rsa:2048 -nodes -sha256 -days 2 \
    -keyout "$dir/ca.key" -out "$dir/ca.pem" -subj /CN=Woozy-Test-CA \
    >"$LOGS/cert-ca.log" 2>&1
  "$OPENSSL" req -x509 -newkey rsa:2048 -nodes -sha256 -days 2 \
    -keyout "$dir/other-ca.key" -out "$dir/other-ca.pem" \
    -subj /CN=Woozy-Other-CA >"$LOGS/cert-other.log" 2>&1
  "$OPENSSL" req -newkey rsa:2048 -nodes -sha256 \
    -keyout "$dir/server.key" -out "$dir/server.csr" \
    -subj /CN=localhost >"$LOGS/cert-server.log" 2>&1
  "$OPENSSL" x509 -req -in "$dir/server.csr" -CA "$dir/ca.pem" \
    -CAkey "$dir/ca.key" -CAcreateserial -out "$dir/server.pem" -days 2 \
    -sha256 -extfile "$dir/san.ext" >>"$LOGS/cert-server.log" 2>&1
  cat "$dir/server.pem" "$dir/ca.pem" >"$dir/server-chain.pem"

  mkdir -p "$dir/ca-db"
  : >"$dir/ca-db/index.txt"
  printf '1000\n' >"$dir/ca-db/serial"
  cat >"$dir/ca.cnf" <<EOF
[ ca ]
default_ca = CA_default
[ CA_default ]
dir = $dir/ca-db
database = \$dir/index.txt
new_certs_dir = \$dir
serial = \$dir/serial
certificate = $dir/ca.pem
private_key = $dir/ca.key
default_md = sha256
policy = policy_any
[ policy_any ]
commonName = supplied
EOF
  "$OPENSSL" ca -batch -config "$dir/ca.cnf" -in "$dir/server.csr" \
    -out "$dir/server-expired.pem" -startdate 20200101000000Z \
    -enddate 20200102000000Z -extfile "$dir/san.ext" -notext \
    >>"$LOGS/cert-expired.log" 2>&1
  cat "$dir/server-expired.pem" "$dir/ca.pem" \
    >"$dir/server-expired-chain.pem"

  "$OPENSSL" x509 -in "$dir/server-expired.pem" -noout -dates \
    >>"$LOGS/cert-expired.log" 2>&1
  "$OPENSSL" verify -CAfile "$dir/ca.pem" "$dir/server.pem" \
    >>"$LOGS/cert-verify.log" 2>&1
}

start_python_server() {
  local name=$1
  local mode=$2
  local cert=$3
  local key=$4
  local host=$5
  local expect_sni=$6
  local payload_size=$7
  PORTFILE="$RUN/$name.port"
  RESULTFILE="$RUN/$name.result.json"
  rm -f "$PORTFILE" "$RESULTFILE"
  timeout 60 "$PYTHON" "$HERE/tls_peer.py" server \
    --cert "$cert" --key "$key" --host "$host" \
    --port-file "$PORTFILE" --result-file "$RESULTFILE" \
    --mode "$mode" --payload-size "$payload_size" \
    --expect-sni "$expect_sni" >"$LOGS/$name.python.log" 2>&1 &
  PY_PID=$!
  wait_for_file "$PORTFILE" "$PY_PID" "python server $name"
  PY_PORT=$(cat "$PORTFILE")
}

stop_python_server() {
  local name=$1
  local expectation=$2
  local status=0
  wait "$PY_PID" || status=$?
  PY_PID=0
  cat "$LOGS/$name.python.log" | tee -a "$SUMMARY" || true
  [ "$status" -eq 0 ] || die "python server $name exited with $status"
  [ -s "$RESULTFILE" ] || die "python server $name produced no result"
  "$PYTHON" - "$RESULTFILE" "$expectation" <<'PY' | tee -a "$SUMMARY"
import json
import sys

result = json.load(open(sys.argv[1], encoding="utf-8"))
expectation = sys.argv[2]
summary = {
    "handshake": result.get("handshake"),
    "sni": result.get("sni"),
    "unwrap": result.get("unwrap"),
    "ok": result.get("ok"),
    "error": result.get("error"),
}
print("python-peer:", json.dumps(summary, sort_keys=True))
if expectation == "success" and not result.get("ok"):
    raise SystemExit("expected successful Python peer but it failed")
if expectation == "failure" and result.get("ok"):
    raise SystemExit("expected rejected Python peer but it succeeded")
PY
}

run_python_client() {
  local name=$1
  local host=$2
  local ca=$3
  local server_name=$4
  local kind=$5
  local expectation=$6
  local payload_size=$7
  local verify=${8:-}
  local extra=()
  if [ -n "$verify" ]; then
    extra+=(--expect-verify "$verify")
  fi
  run_case "$name" 45 "$BIN" client-python \
    --host "$host" --port "$PY_PORT" --ca "$ca" \
    --name "$server_name" --name-kind "$kind" --expect "$expectation" \
    --payload-size "$payload_size" "${extra[@]}"
}

run_openssl_client() {
  local name=$1
  local cert=$2
  local key=$3
  local ca=$4
  local portfile="$RUN/$name.port"
  local outfile="$LOGS/$name.client.out"
  local errfile="$LOGS/$name.client.err"
  local serverlog="$LOGS/$name.server.log"
  rm -f "$portfile" "$outfile" "$errfile"
  timeout 45 "$BIN" server-once --cert "$cert" --key "$key" \
    --host 127.0.0.1 --port-file "$portfile" >"$serverlog" 2>&1 &
  SERVER_PID=$!
  wait_for_file "$portfile" "$SERVER_PID" "TLS server-once"
  local port
  port=$(cat "$portfile")
  local status=0
  printf 'ping\n' | timeout 25 "$OPENSSL" s_client -quiet \
    -connect "127.0.0.1:$port" -servername localhost \
    -verify_hostname localhost -CAfile "$ca" \
    >"$outfile" 2>"$errfile" || status=$?
  wait "$SERVER_PID" || status=$?
  SERVER_PID=0
  cat "$outfile" | tee -a "$SUMMARY"
  cat "$errfile" | tee -a "$SUMMARY"
  cat "$serverlog" | tee -a "$SUMMARY"
  [ "$status" -eq 0 ] || die "$name openssl s_client/server failed with $status"
  grep -qx 'pong' "$outfile" || die "$name did not receive pong"
  log "PASS $name: independent openssl s_client verified our TLS server"
}

if [ ! -x "$CXX" ]; then
  die "LLVM 22 clang++ not found at $CXX (set CXX)"
fi
if [ ! -x "$CC" ]; then
  die "C compiler not found at $CC (set CC)"
fi
command -v "$PYTHON" >/dev/null || die "python3 not found"
command -v "$OPENSSL" >/dev/null || die "openssl CLI not found"
command -v timeout >/dev/null || die "timeout not found"

log "Woozy NET02 hosted TLS validation"
log "root: $ROOT"
log "HEAD: $(git -C "$ROOT" rev-parse HEAD)"
"$CXX" --version | head -1 | tee -a "$SUMMARY"
"$CC" --version | head -1 | tee -a "$SUMMARY"
"$PYTHON" - <<'PY' | tee -a "$SUMMARY"
import ssl
import sys
print("python:", sys.version.split()[0], "OpenSSL:", ssl.OPENSSL_VERSION)
PY
"$OPENSSL" version | tee -a "$SUMMARY"

make_certs "$CERTS"

cat >"$BUILD/header_c_probe.c" <<'EOF'
#include "runtime/hosted/tls.hpp"

int main(void) {
  LangTlsError error = {0};
  return sizeof(error) == 40 ? 0 : 1;
}
EOF
"$CC" -std=c11 -Wall -Wextra -Werror -I "$ROOT" \
  "$BUILD/header_c_probe.c" -o "$BUILD/header_c_probe" \
  >"$LOGS/header-c.log" 2>&1
"$BUILD/header_c_probe"
log "PASS header: tls.hpp compiles as C11 and LangTlsError is 40 bytes"

for optimization in O0 O2; do
  BIN="$BUILD/tls_runtime_test-$optimization"
  log "=== build/run $optimization ==="
  "$CXX" -std=c++20 -"$optimization" -g -Wall -Wextra -Wpedantic -Werror \
    -fno-omit-frame-pointer -pthread -I "$ROOT" \
    -c "$ROOT/runtime/hosted/tls.cpp" -o "$BUILD/tls-$optimization.o" \
    >"$LOGS/compile-tls-$optimization.log" 2>&1
  "$CXX" -std=c++20 -"$optimization" -g -Wall -Wextra -Wpedantic -Werror \
    -fno-omit-frame-pointer -pthread -I "$ROOT" \
    -c "$HERE/tls_runtime_test.cpp" -o "$BUILD/test-$optimization.o" \
    >>"$LOGS/compile-tls-$optimization.log" 2>&1
  "$CXX" -std=c++20 -"$optimization" -g -pthread \
    "$BUILD/tls-$optimization.o" "$BUILD/test-$optimization.o" \
    -lssl -lcrypto -o "$BIN" >>"$LOGS/compile-tls-$optimization.log" 2>&1

  nm -g --defined-only "$BUILD/tls-$optimization.o" |
    awk '$2 == "T" && $3 ~ /^lang_tls_/ {print $3}' | sort \
    >"$BUILD/api-$optimization.txt"
  cat >"$BUILD/api-expected.txt" <<'EOF'
lang_tls_context_client_default_v1
lang_tls_context_client_v1
lang_tls_context_release_v1
lang_tls_context_retain_v1
lang_tls_context_server_v1
lang_tls_handshake_v1
lang_tls_read_v1
lang_tls_shutdown_v1
lang_tls_stream_client_v1
lang_tls_stream_release_v1
lang_tls_stream_retain_v1
lang_tls_stream_server_v1
lang_tls_write_v1
EOF
  diff -u "$BUILD/api-expected.txt" "$BUILD/api-$optimization.txt" \
    >>"$SUMMARY" || die "unexpected exported lang_tls_* symbols at $optimization"
  log "PASS exports $optimization: 13 lang_tls_*_v1 symbols"

  readelf -d "$BIN" | grep NEEDED >"$BUILD/needed-$optimization.txt" || true
  cat "$BUILD/needed-$optimization.txt" | tee -a "$SUMMARY"
  grep -q 'libssl.so.3' "$BUILD/needed-$optimization.txt" ||
    die "libssl.so.3 is not a recorded dynamic dependency"
  grep -q 'libcrypto.so.3' "$BUILD/needed-$optimization.txt" ||
    die "libcrypto.so.3 is not a recorded dynamic dependency"
  grep -q 'libc.so.6' "$BUILD/needed-$optimization.txt" ||
    die "libc.so.6 is not a recorded dynamic dependency"

  run_case "self-$optimization" 120 "$BIN" self \
    --cert "$CERTS/server-chain.pem" --key "$CERTS/server.key" \
    --ca "$CERTS/ca.pem"
  run_case "nonblocking-$optimization" 120 "$BIN" nonblocking \
    --cert "$CERTS/server-chain.pem" --key "$CERTS/server.key" \
    --ca "$CERTS/ca.pem"
  run_case "eintr-$optimization" 120 "$BIN" eintr \
    --cert "$CERTS/server-chain.pem" --key "$CERTS/server.key" \
    --ca "$CERTS/ca.pem"
  run_case "sigpipe-$optimization" 120 "$BIN" sigpipe \
    --cert "$CERTS/server-chain.pem" --key "$CERTS/server.key" \
    --ca "$CERTS/ca.pem"
  run_case "validation-$optimization" 60 "$BIN" validation \
    --cert "$CERTS/server-chain.pem" --key "$CERTS/server.key" \
    --ca "$CERTS/ca.pem" --wrong-key "$CERTS/ca.key"

  start_python_server "clean-$optimization" clean \
    "$CERTS/server-chain.pem" "$CERTS/server.key" 127.0.0.1 localhost 65536
  run_python_client "client-clean-$optimization" 127.0.0.1 "$CERTS/ca.pem" \
    localhost dns ok 65536
  stop_python_server "clean-$optimization" success

  start_python_server "truncate-$optimization" truncate \
    "$CERTS/server-chain.pem" "$CERTS/server.key" 127.0.0.1 localhost 65536
  run_python_client "client-truncate-$optimization" 127.0.0.1 "$CERTS/ca.pem" \
    localhost dns unexpected-eof 65536
  stop_python_server "truncate-$optimization" success

  export SSL_CERT_FILE="$CERTS/ca.pem"
  start_python_server "default-trust-$optimization" clean \
    "$CERTS/server-chain.pem" "$CERTS/server.key" 127.0.0.1 localhost 65536
  run_python_client "client-default-trust-$optimization" 127.0.0.1 default \
    localhost dns ok 65536
  stop_python_server "default-trust-$optimization" success
  unset SSL_CERT_FILE

  start_python_server "wrong-host-$optimization" clean \
    "$CERTS/server-chain.pem" "$CERTS/server.key" 127.0.0.1 wrong.example 1024
  run_python_client "client-wrong-host-$optimization" 127.0.0.1 \
    "$CERTS/ca.pem" wrong.example dns certificate 1024 62
  stop_python_server "wrong-host-$optimization" failure

  start_python_server "wrong-ca-$optimization" clean \
    "$CERTS/server-chain.pem" "$CERTS/server.key" 127.0.0.1 localhost 1024
  run_python_client "client-wrong-ca-$optimization" 127.0.0.1 \
    "$CERTS/other-ca.pem" localhost dns certificate 1024
  stop_python_server "wrong-ca-$optimization" failure

  start_python_server "expired-$optimization" clean \
    "$CERTS/server-expired-chain.pem" "$CERTS/server.key" 127.0.0.1 localhost 1024
  run_python_client "client-expired-$optimization" 127.0.0.1 \
    "$CERTS/ca.pem" localhost dns certificate 1024 10
  stop_python_server "expired-$optimization" failure

  start_python_server "ipv4-$optimization" clean \
    "$CERTS/server-chain.pem" "$CERTS/server.key" 127.0.0.1 none 65536
  run_python_client "client-ipv4-$optimization" 127.0.0.1 "$CERTS/ca.pem" \
    127.0.0.1 ip ok 65536
  stop_python_server "ipv4-$optimization" success

  start_python_server "auto-ip-$optimization" clean \
    "$CERTS/server-chain.pem" "$CERTS/server.key" 127.0.0.1 none 65536
  run_python_client "client-auto-ip-$optimization" 127.0.0.1 \
    "$CERTS/ca.pem" 127.0.0.1 auto ok 65536
  stop_python_server "auto-ip-$optimization" success

  if "$PYTHON" - <<'PY' >/dev/null 2>&1
import socket
s = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
try:
    s.bind(("::1", 0))
finally:
    s.close()
PY
  then
    start_python_server "ipv6-$optimization" clean \
      "$CERTS/server-chain.pem" "$CERTS/server.key" ::1 none 65536
    run_python_client "client-ipv6-$optimization" ::1 "$CERTS/ca.pem" \
      ::1 ip ok 65536
    stop_python_server "ipv6-$optimization" success
  else
    log "SKIP ipv6-$optimization: ::1 is unavailable"
  fi

  run_openssl_client "openssl-client-$optimization" \
    "$CERTS/server-chain.pem" "$CERTS/server.key" "$CERTS/ca.pem"
done

CORE_PATHS=(
  runtime/native
  runtime/hosted/start.cpp
  runtime/hosted/start.hpp
  runtime/hosted/thread.cpp
  runtime/hosted/thread.hpp
  runtime/hosted/sources.cmake
)
if git -C "$ROOT" diff --quiet HEAD -- "${CORE_PATHS[@]}"; then
  log "PASS core: freestanding runtime and hosted start/thread have no tracked change"
else
  git -C "$ROOT" diff --stat HEAD -- "${CORE_PATHS[@]}" | tee -a "$SUMMARY" || true
  die "core runtime changed; hosted TLS must remain an additive hosted component"
fi
git -C "$ROOT" status --porcelain --untracked-files=all | tee -a "$SUMMARY"

log "ALL PASS: hosted TLS ABI, real loopback TLS, Python ssl and openssl CLI"
log "artifacts: $BUILD"
