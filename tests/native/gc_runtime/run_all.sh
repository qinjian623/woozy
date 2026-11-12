#!/usr/bin/env bash
# Single-entry verification suite for lang-gc-runtime/1.
#
# Every executing test process is wrapped in `timeout`. The suite links the
# accepted lang-gc/1, lang-gc-world/1 and Weak layers; it does not modify them.

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

if [ -n "${GC_RUNTIME_BUILD_DIR:-}" ]; then
  BUILD_DIR=$GC_RUNTIME_BUILD_DIR
  KEEP_BUILD=1
else
  BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/woozy-gc-runtime.XXXXXX")
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

run_expect_panic() {
  local name=$1 timeout_s=$2 reason=$3
  shift 3
  local log=$BUILD_DIR/${name// /_}.log
  timeout --signal=TERM --kill-after=5s "${timeout_s}s" "$@" >"$log" 2>&1
  local status=$?
  if [ "$status" -ne 101 ]; then
    fail "$name (exit $status, expected 101)"
    show_log "$log"
    return 1
  fi
  if ! grep -q "LANG_NATIVE_PANIC:v1:gc-runtime:$reason:" "$log"; then
    fail "$name (missing panic reason $reason)"
    show_log "$log"
    return 1
  fi
  pass "$name"
  return 0
}

HOST_SOURCES=(
  "$SCRIPT_DIR/gc_runtime_test.cpp"
  "$ROOT/runtime/native/gc_runtime.cpp"
  "$ROOT/runtime/native/gc_world.cpp"
  "$ROOT/runtime/native/gc.cpp"
  "$ROOT/runtime/native/weak.cpp"
  "$ROOT/runtime/native/panic.cpp"
  "$ROOT/runtime/native/alloc.cpp"
  "$SCRIPT_DIR/test_alloc_wrap.cpp"
)
HOST_WRAP=(-Wl,--wrap=lang_alloc_v1 -Wl,--wrap=lang_free_v1)

if build_step "host O0" "$COMPILER" "${COMMON[@]}" -O0 -g \
    "${HOST_SOURCES[@]}" "${HOST_WRAP[@]}" -o "$BUILD_DIR/gc_runtime_o0"; then
  run_expect "host O0" 300 0 "GC_RUNTIME_HOST_OK" "$BUILD_DIR/gc_runtime_o0"
fi

if build_step "host O2" "$COMPILER" "${COMMON[@]}" -O2 -g \
    "${HOST_SOURCES[@]}" "${HOST_WRAP[@]}" -o "$BUILD_DIR/gc_runtime_o2"; then
  run_expect "host O2" 300 0 "GC_RUNTIME_HOST_OK" "$BUILD_DIR/gc_runtime_o2"
fi

if build_step "host ASAN+UBSAN" "$COMPILER" "${COMMON[@]}" -O1 -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    "$SCRIPT_DIR/gc_runtime_test.cpp" \
    "$ROOT/runtime/native/gc_runtime.cpp" "$ROOT/runtime/native/gc_world.cpp" \
    "$ROOT/runtime/native/gc.cpp" "$ROOT/runtime/native/weak.cpp" \
    "$ROOT/runtime/native/panic.cpp" "$SCRIPT_DIR/test_alloc_asan.cpp" \
    -o "$BUILD_DIR/gc_runtime_asan"; then
  run_expect "host ASAN+UBSAN" 360 0 "GC_RUNTIME_HOST_OK" \
    env ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    "$BUILD_DIR/gc_runtime_asan"
fi

if build_step "controller misuse binary" "$COMPILER" "${COMMON[@]}" -O2 -g \
    "$SCRIPT_DIR/gc_runtime_misuse_test.cpp" \
    "$ROOT/runtime/native/gc_runtime.cpp" "$ROOT/runtime/native/gc_world.cpp" \
    "$ROOT/runtime/native/gc.cpp" "$ROOT/runtime/native/weak.cpp" \
    "$ROOT/runtime/native/panic.cpp" "$ROOT/runtime/native/alloc.cpp" \
    -o "$BUILD_DIR/gc_runtime_misuse"; then
  run_expect_panic "fork in gc callback" 30 "fork-in-gc-callback" \
    "$BUILD_DIR/gc_runtime_misuse"
fi

if build_step "fastpoll seccomp binary" "$COMPILER" "${COMMON[@]}" -O2 -g \
    "$SCRIPT_DIR/gc_runtime_fastpoll_test.cpp" \
    "$ROOT/runtime/native/gc_runtime.cpp" "$ROOT/runtime/native/gc_world.cpp" \
    "$ROOT/runtime/native/gc.cpp" "$ROOT/runtime/native/weak.cpp" \
    "$ROOT/runtime/native/panic.cpp" "$ROOT/runtime/native/alloc.cpp" \
    -o "$BUILD_DIR/gc_runtime_fastpoll"; then
  run_expect "fastpoll 100k, gettid denied" 30 0 "FAST_OK" \
    "$BUILD_DIR/gc_runtime_fastpoll"
fi

STATIC_OK=1
for source in gc_runtime gc_world gc weak panic alloc freestanding; do
  build_step "static object $source" "$COMPILER" "${FREESTANDING[@]}" \
    -c "$ROOT/runtime/native/$source.cpp" \
    -o "$BUILD_DIR/$source.o" || STATIC_OK=0
done
build_step "static object test" "$COMPILER" "${FREESTANDING[@]}" \
  -c "$SCRIPT_DIR/gc_runtime_static_test.cpp" -o "$BUILD_DIR/static_test.o" \
  || STATIC_OK=0

if [ "$STATIC_OK" = "1" ]; then
  if build_step "static ELF link" ld -static -z noexecstack -e _start \
      -o "$BUILD_DIR/gc_runtime_static" \
      "$BUILD_DIR/gc_runtime.o" "$BUILD_DIR/gc_world.o" "$BUILD_DIR/gc.o" \
      "$BUILD_DIR/weak.o" "$BUILD_DIR/panic.o" "$BUILD_DIR/alloc.o" \
      "$BUILD_DIR/freestanding.o" "$BUILD_DIR/static_test.o"; then
    if file "$BUILD_DIR/gc_runtime_static" | grep -q 'statically linked'; then
      pass "static ELF is statically linked"
    else
      fail "static ELF is not statically linked"
    fi
    if readelf -d "$BUILD_DIR/gc_runtime_static" 2>&1 | grep -q NEEDED; then
      fail "static ELF has dynamic NEEDED entries"
    else
      pass "static ELF has no dynamic dependencies"
    fi
    if readelf -l "$BUILD_DIR/gc_runtime_static" 2>&1 | grep -q INTERP; then
      fail "static ELF has an interpreter"
    else
      pass "static ELF has no interpreter"
    fi
    if nm -u "$BUILD_DIR/gc_runtime_static" | grep -q .; then
      fail "static ELF has undefined symbols"
      nm -u "$BUILD_DIR/gc_runtime_static" | sed -e 's/^/    /' | head -20
    else
      pass "static ELF has no undefined symbols"
    fi
    run_expect "static ELF execution" 120 0 "GC_RUNTIME_STATIC_OK" \
      "$BUILD_DIR/gc_runtime_static"
  fi
fi

if command -v g++ >/dev/null 2>&1; then
  build_step "GCC freestanding controller object" g++ "${FREESTANDING[@]}" \
    -c "$ROOT/runtime/native/gc_runtime.cpp" -o "$BUILD_DIR/gc_runtime_gcc.o"
else
  skip "GCC freestanding controller object" "g++ not found"
fi

echo
echo "SUMMARY pass=$PASS_COUNT fail=$FAIL_COUNT skip=$SKIP_COUNT"
if [ "$FAIL_COUNT" -ne 0 ]; then
  exit 1
fi
exit 0
