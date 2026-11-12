#!/usr/bin/env bash
# New callback-API suite for lang-gc-world/1. The accepted 42-check world suite
# remains separate and untouched.

set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)

if [ -n "${CXX:-}" ]; then
  COMPILER=$CXX
elif command -v clang++ >/dev/null 2>&1; then
  COMPILER=clang++
elif command -v g++ >/dev/null 2>&1; then
  COMPILER=g++
else
  echo "FAIL: no C++ compiler found" >&2
  exit 1
fi

for tool in timeout ld nm file readelf; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "FAIL: required tool not found: $tool" >&2
    exit 1
  fi
done

if [ -n "${GC_WORLD_CALLBACK_BUILD_DIR:-}" ]; then
  BUILD_DIR=$GC_WORLD_CALLBACK_BUILD_DIR
  KEEP_BUILD=1
else
  BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/woozy-gc-world-callback.XXXXXX")
  KEEP_BUILD=0
fi
mkdir -p "$BUILD_DIR"

PASS_COUNT=0
FAIL_COUNT=0
SKIP_COUNT=0
pass() { PASS_COUNT=$((PASS_COUNT + 1)); printf 'PASS  %s\n' "$1"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); printf 'FAIL  %s\n' "$1"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); printf 'SKIP  %s (%s)\n' "$1" "$2"; }
show_log() { [ -f "$1" ] && sed -e 's/^/    /' "$1" | tail -n 50; }

cleanup() {
  if [ "$KEEP_BUILD" = "0" ] && [ "$FAIL_COUNT" = "0" ]; then
    rm -rf "$BUILD_DIR"
  else
    echo "build artifacts: $BUILD_DIR"
  fi
}
trap cleanup EXIT

COMMON=(-std=c++20 -pthread -Wall -Wextra -Werror -I"$ROOT/runtime/native")
FREESTANDING=(-std=c++20 -O2 -ffreestanding -fno-exceptions -fno-rtti
  -fno-threadsafe-statics -fno-stack-protector -fno-pic -fno-pie
  -Wall -Wextra -Werror -I"$ROOT/runtime/native")

build_step() {
  local name=$1
  shift
  local log=$BUILD_DIR/${name// /_}.log
  if timeout 300s "$@" >"$log" 2>&1; then
    pass "build: $name"
    return 0
  else
    fail "build: $name"
    show_log "$log"
    return 1
  fi
}

run_expect() {
  local name=$1 timeout_s=$2 expected=$3 marker=$4
  shift 4
  local log=$BUILD_DIR/${name// /_}.log
  timeout --signal=TERM --kill-after=5s "${timeout_s}s" "$@" >"$log" 2>&1
  local status=$?
  if [ "$status" -ne "$expected" ]; then
    fail "$name (exit $status, expected $expected)"
    show_log "$log"
    return 1
  fi
  if [ -n "$marker" ] && ! grep -q "$marker" "$log"; then
    fail "$name (missing output marker: $marker)"
    show_log "$log"
    return 1
  fi
  pass "$name"
  return 0
}

run_panic() {
  local mode=$1
  local reason=$2
  local log=$BUILD_DIR/callback-misuse-$mode.log
  timeout --signal=TERM --kill-after=5s 20s \
    "$BUILD_DIR/gc_world_callback_misuse" "$mode" >"$log" 2>&1
  local status=$?
  if [ "$status" -ne 101 ]; then
    fail "callback-misuse/$mode (exit $status, expected 101)"
    show_log "$log"
    return 1
  fi
  if ! grep -q "LANG_NATIVE_PANIC:v1:gc-world:$reason:" "$log"; then
    fail "callback-misuse/$mode (missing reason $reason)"
    show_log "$log"
    return 1
  fi
  pass "callback-misuse/$mode"
  return 0
}

HOST_SOURCES=(
  "$SCRIPT_DIR/gc_world_callback_test.cpp"
  "$ROOT/runtime/native/gc_world.cpp"
  "$ROOT/runtime/native/alloc.cpp"
  "$SCRIPT_DIR/test_alloc_wrap.cpp"
)
HOST_WRAP=(-Wl,--wrap=lang_alloc_v1 -Wl,--wrap=lang_free_v1)

if build_step "callback host O0" "$COMPILER" "${COMMON[@]}" -O0 -g \
    "${HOST_SOURCES[@]}" "${HOST_WRAP[@]}" -o "$BUILD_DIR/callback_o0"; then
  run_expect "callback host O0" 180 0 "GC_WORLD_CALLBACK_HOST_OK" \
    "$BUILD_DIR/callback_o0"
fi
if build_step "callback host O2" "$COMPILER" "${COMMON[@]}" -O2 -g \
    "${HOST_SOURCES[@]}" "${HOST_WRAP[@]}" -o "$BUILD_DIR/callback_o2"; then
  run_expect "callback host O2" 180 0 "GC_WORLD_CALLBACK_HOST_OK" \
    "$BUILD_DIR/callback_o2"
fi
if build_step "callback host ASAN+UBSAN" "$COMPILER" "${COMMON[@]}" -O1 -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    "$SCRIPT_DIR/gc_world_callback_test.cpp" \
    "$ROOT/runtime/native/gc_world.cpp" \
    "$SCRIPT_DIR/test_alloc_asan.cpp" -o "$BUILD_DIR/callback_asan"; then
  run_expect "callback host ASAN+UBSAN" 240 0 "GC_WORLD_CALLBACK_HOST_OK" \
    env ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    "$BUILD_DIR/callback_asan"
fi

if build_step "callback misuse binary" "$COMPILER" "${COMMON[@]}" -O2 -g \
    "$SCRIPT_DIR/gc_world_callback_misuse_test.cpp" \
    "$ROOT/runtime/native/gc_world.cpp" "$ROOT/runtime/native/alloc.cpp" \
    -o "$BUILD_DIR/gc_world_callback_misuse"; then
  run_panic callback-in-critical callback-in-critical
  run_panic callback-unbalanced callback-token-invalid
  run_panic callback-left-safe callback-left-safe
  run_panic callback-left-critical callback-left-critical
  run_panic unregister-in-callback unregister-in-callback
  run_panic callback-in-collector callback-in-collector
  run_panic callback-token-order callback-token-order
  run_panic callback-token-active callback-token-active
fi

STATIC_OK=1
for source in gc_world alloc freestanding; do
  build_step "callback static object $source" "$COMPILER" "${FREESTANDING[@]}" \
    -c "$ROOT/runtime/native/$source.cpp" -o "$BUILD_DIR/cb_$source.o" \
    || STATIC_OK=0
done
build_step "callback static test object" "$COMPILER" "${FREESTANDING[@]}" \
  -c "$SCRIPT_DIR/gc_world_callback_static_test.cpp" \
  -o "$BUILD_DIR/cb_test.o" || STATIC_OK=0

if [ "$STATIC_OK" = "1" ]; then
  if build_step "callback static ELF link" ld -static -z noexecstack -e _start \
      -o "$BUILD_DIR/callback_static" "$BUILD_DIR/cb_gc_world.o" \
      "$BUILD_DIR/cb_alloc.o" "$BUILD_DIR/cb_freestanding.o" \
      "$BUILD_DIR/cb_test.o"; then
    if file "$BUILD_DIR/callback_static" | grep -q 'statically linked'; then
      pass "callback static ELF is statically linked"
    else
      fail "callback static ELF is not statically linked"
    fi
    if readelf -d "$BUILD_DIR/callback_static" 2>&1 | grep -q NEEDED; then
      fail "callback static ELF has dynamic NEEDED entries"
    else
      pass "callback static ELF has no dynamic dependencies"
    fi
    if readelf -l "$BUILD_DIR/callback_static" 2>&1 | grep -q INTERP; then
      fail "callback static ELF has an interpreter"
    else
      pass "callback static ELF has no interpreter"
    fi
    if nm -u "$BUILD_DIR/callback_static" | grep -q .; then
      fail "callback static ELF has undefined symbols"
      nm -u "$BUILD_DIR/callback_static" | sed -e 's/^/    /' | head -20
    else
      pass "callback static ELF has no undefined symbols"
    fi
    run_expect "callback static ELF execution" 60 0 \
      "GC_WORLD_CALLBACK_STATIC_OK" "$BUILD_DIR/callback_static"
  fi
fi

if command -v g++ >/dev/null 2>&1; then
  build_step "GCC freestanding callback world object" g++ "${FREESTANDING[@]}" \
    -c "$ROOT/runtime/native/gc_world.cpp" -o "$BUILD_DIR/gc_world_callback_gcc.o"
else
  skip "GCC freestanding callback world object" "g++ not found"
fi

echo
echo "SUMMARY pass=$PASS_COUNT fail=$FAIL_COUNT skip=$SKIP_COUNT"
if [ "$FAIL_COUNT" -ne 0 ]; then
  exit 1
fi
exit 0
