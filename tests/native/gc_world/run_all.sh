#!/usr/bin/env bash
# Single-entry verification suite for lang-gc-world/1.
#
# Every process that can execute user test code is run through `timeout`. The
# script never waits in an unbounded model loop; the external timeout is the
# backstop for a stuck futex or thread.

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

if [ -n "${GC_WORLD_BUILD_DIR:-}" ]; then
  BUILD_DIR=$GC_WORLD_BUILD_DIR
  KEEP_BUILD=1
else
  BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/woozy-gc-world.XXXXXX")
  KEEP_BUILD=0
fi
mkdir -p "$BUILD_DIR"

PASS_COUNT=0
FAIL_COUNT=0
SKIP_COUNT=0

pass() {
  PASS_COUNT=$((PASS_COUNT + 1))
  printf 'PASS  %s\n' "$1"
}

fail() {
  FAIL_COUNT=$((FAIL_COUNT + 1))
  printf 'FAIL  %s\n' "$1"
}

skip() {
  SKIP_COUNT=$((SKIP_COUNT + 1))
  printf 'SKIP  %s (%s)\n' "$1" "$2"
}

show_log() {
  if [ -f "$1" ]; then
    sed -e 's/^/    /' "$1" | tail -n 40
  fi
}

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
  if timeout 240s "$@" >"$log" 2>&1; then
    pass "build: $name"
    return 0
  else
    fail "build: $name"
    show_log "$log"
    return 1
  fi
}

run_expect() {
  local name=$1
  local timeout_s=$2
  local expected=$3
  local marker=$4
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
  local mode=$1
  local reason=$2
  local log=$BUILD_DIR/misuse-$mode.log
  timeout --signal=TERM --kill-after=5s 20s "$BUILD_DIR/gc_world_misuse" "$mode" \
    >"$log" 2>&1
  local status=$?
  if [ "$status" -ne 101 ]; then
    fail "misuse/$mode (exit $status, expected 101)"
    show_log "$log"
    return 1
  fi
  if ! grep -q "LANG_NATIVE_PANIC:v1:gc-world:$reason:" "$log"; then
    fail "misuse/$mode (missing panic reason $reason)"
    show_log "$log"
    return 1
  fi
  pass "misuse/$mode"
  return 0
}

run_seccomp_panic() {
  local mode=$1
  local reason=$2
  local forbidden=${3:-}
  local log=$BUILD_DIR/seccomp-$mode.log
  timeout --signal=TERM --kill-after=5s 30s "$BUILD_DIR/gc_world_seccomp" \
    "$mode" >"$log" 2>&1
  local status=$?
  if [ "$status" -ne 101 ]; then
    fail "seccomp/$mode (exit $status, expected 101)"
    show_log "$log"
    return 1
  fi
  if ! grep -q "LANG_NATIVE_PANIC:v1:gc-world:$reason:" "$log"; then
    fail "seccomp/$mode (missing panic reason $reason)"
    show_log "$log"
    return 1
  fi
  if [ -n "$forbidden" ] && grep -q "$forbidden" "$log"; then
    fail "seccomp/$mode (unexpected output: $forbidden)"
    show_log "$log"
    return 1
  fi
  pass "seccomp/$mode"
  return 0
}

run_critical_overflow() {
  local log=$BUILD_DIR/critical-overflow.log
  timeout --signal=TERM --kill-after=5s 20s     "$BUILD_DIR/gc_world_critical_overflow" >"$log" 2>&1
  local status=$?
  if [ "$status" -ne 101 ]; then
    fail "critical-depth-overflow (exit $status, expected 101)"
    show_log "$log"
    return 1
  fi
  if ! grep -q "LANG_NATIVE_PANIC:v1:gc-world:critical-depth-overflow:" "$log"; then
    fail "critical-depth-overflow (missing panic reason)"
    show_log "$log"
    return 1
  fi
  pass "critical-depth-overflow"
  return 0
}

HOST_SOURCES=(
  "$SCRIPT_DIR/gc_world_test.cpp"
  "$ROOT/runtime/native/gc_world.cpp"
  "$ROOT/runtime/native/alloc.cpp"
  "$SCRIPT_DIR/test_alloc_wrap.cpp"
)
HOST_WRAP=(-Wl,--wrap=lang_alloc_v1 -Wl,--wrap=lang_free_v1)

if build_step "host O0" "$COMPILER" "${COMMON[@]}" -O0 -g \
    "${HOST_SOURCES[@]}" "${HOST_WRAP[@]}" -o "$BUILD_DIR/gc_world_o0"; then
  run_expect "host O0" 180 0 "GC_WORLD_HOST_OK" "$BUILD_DIR/gc_world_o0"
fi

if build_step "host O2" "$COMPILER" "${COMMON[@]}" -O2 -g \
    "${HOST_SOURCES[@]}" "${HOST_WRAP[@]}" -o "$BUILD_DIR/gc_world_o2"; then
  run_expect "host O2" 180 0 "GC_WORLD_HOST_OK" "$BUILD_DIR/gc_world_o2"
fi

if build_step "host ASAN+UBSAN" "$COMPILER" "${COMMON[@]}" -O1 -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    "$SCRIPT_DIR/gc_world_test.cpp" "$ROOT/runtime/native/gc_world.cpp" \
    "$SCRIPT_DIR/test_alloc_asan.cpp" -o "$BUILD_DIR/gc_world_asan"; then
  run_expect "host ASAN+UBSAN" 240 0 "GC_WORLD_HOST_OK" \
    env ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    "$BUILD_DIR/gc_world_asan"
fi

if build_step "misuse binary" "$COMPILER" "${COMMON[@]}" -O2 -g \
    "$SCRIPT_DIR/gc_world_misuse_test.cpp" \
    "$ROOT/runtime/native/gc_world.cpp" "$ROOT/runtime/native/alloc.cpp" \
    -o "$BUILD_DIR/gc_world_misuse"; then
  run_expect_panic poll-unregistered-pending poll-unregistered
  run_expect_panic enter-safe-unregistered safe-unregistered
  run_expect_panic leave-safe-unregistered safe-unregistered
  run_expect_panic leave-without-enter leave-without-enter
  run_expect_panic critical-in-safe critical-in-safe
  run_expect_panic safe-in-critical safe-in-critical
  run_expect_panic enter-critical-unregistered critical-unregistered
  run_expect_panic leave-critical-unregistered critical-unregistered
  run_expect_panic unregister-in-critical unregister-in-critical
  run_expect_panic leave-critical-without-enter leave-critical-without-enter
  run_expect_panic stop-unregistered stop-unregistered
  run_expect_panic stop-in-safe stop-in-safe
  run_expect_panic resume-unregistered resume-unregistered
  run_expect_panic resume-not-owner resume-not-owner
  run_expect_panic unregister-in-safe unregister-in-safe
  run_expect_panic unregister-collector unregister-collector
  run_expect_panic fork-unregistered fork-unregistered
  run_expect_panic fork-active-stop fork-active-stop
fi

if build_step "critical overflow binary" "$COMPILER" "${COMMON[@]}" -O2 -g \
    -DLANG_GC_WORLD_TEST_CRITICAL_MAX=2u \
    "$SCRIPT_DIR/gc_world_critical_overflow_test.cpp" \
    "$ROOT/runtime/native/gc_world.cpp" "$ROOT/runtime/native/alloc.cpp" \
    -o "$BUILD_DIR/gc_world_critical_overflow"; then
  run_critical_overflow
fi

if build_step "seccomp fault binary" "$COMPILER" "${COMMON[@]}" -O2 -g \
    "$SCRIPT_DIR/gc_world_seccomp_test.cpp" \
    "$ROOT/runtime/native/gc_world.cpp" "$ROOT/runtime/native/alloc.cpp" \
    -o "$BUILD_DIR/gc_world_seccomp"; then
  run_expect "seccomp fast path 100k, gettid denied" 30 0 "FAST_OK" \
    "$BUILD_DIR/gc_world_seccomp" fast
  run_seccomp_panic gettid gettid-failed
  run_seccomp_panic wake futex-wake-failed "RESUME_RETURNED"
fi

OBJECTS_OK=1
if build_step "freestanding gc_world object" "$COMPILER" "${FREESTANDING[@]}" \
    -c "$ROOT/runtime/native/gc_world.cpp" -o "$BUILD_DIR/gc_world.o"; then
  UNDEFINED=$BUILD_DIR/gc_world_undefined.txt
  nm -u "$BUILD_DIR/gc_world.o" >"$UNDEFINED" 2>&1
  if grep -Ev '^[[:space:]]*(U[[:space:]]+)?(lang_alloc_v1|lang_free_v1)$' \
      "$UNDEFINED" | grep -q .; then
    fail "freestanding object has unexpected undefined symbols"
    show_log "$UNDEFINED"
    OBJECTS_OK=0
  else
    pass "freestanding object symbol check"
  fi
else
  OBJECTS_OK=0
fi

if [ "$OBJECTS_OK" = "1" ]; then
  FREESTANDING_OK=1
  build_step "freestanding allocator object" "$COMPILER" "${FREESTANDING[@]}" \
    -c "$ROOT/runtime/native/alloc.cpp" -o "$BUILD_DIR/alloc.o" || FREESTANDING_OK=0
  build_step "freestanding support object" "$COMPILER" "${FREESTANDING[@]}" \
    -c "$ROOT/runtime/native/freestanding.cpp" -o "$BUILD_DIR/freestanding.o" || FREESTANDING_OK=0
  build_step "freestanding static test object" "$COMPILER" "${FREESTANDING[@]}" \
    -c "$SCRIPT_DIR/gc_world_static_test.cpp" -o "$BUILD_DIR/static_test.o" || FREESTANDING_OK=0

  if [ "$FREESTANDING_OK" = "1" ]; then
    if build_step "no-libc static ELF link" ld -static -z noexecstack -e _start \
        -o "$BUILD_DIR/gc_world_static" \
        "$BUILD_DIR/gc_world.o" "$BUILD_DIR/alloc.o" \
        "$BUILD_DIR/freestanding.o" "$BUILD_DIR/static_test.o"; then
      if file "$BUILD_DIR/gc_world_static" | grep -q 'statically linked'; then
        pass "static ELF is statically linked"
      else
        fail "static ELF is not statically linked"
      fi
      if readelf -d "$BUILD_DIR/gc_world_static" 2>&1 | grep -q NEEDED; then
        fail "static ELF has dynamic NEEDED entries"
      else
        pass "static ELF has no dynamic dependencies"
      fi
      if readelf -l "$BUILD_DIR/gc_world_static" 2>&1 | grep -q INTERP; then
        fail "static ELF has an interpreter"
      else
        pass "static ELF has no interpreter"
      fi
      run_expect "no-libc static ELF execution" 60 0 "GC_WORLD_STATIC_OK" \
        "$BUILD_DIR/gc_world_static"
    fi
  fi
fi

if command -v g++ >/dev/null 2>&1; then
  build_step "GCC freestanding gc_world object" g++ "${FREESTANDING[@]}" \
    -c "$ROOT/runtime/native/gc_world.cpp" -o "$BUILD_DIR/gc_world_gcc.o"
else
  skip "GCC freestanding gc_world object" "g++ not found"
fi

echo
echo "SUMMARY pass=$PASS_COUNT fail=$FAIL_COUNT skip=$SKIP_COUNT"
if [ "$FAIL_COUNT" -ne 0 ]; then
  exit 1
fi
exit 0
