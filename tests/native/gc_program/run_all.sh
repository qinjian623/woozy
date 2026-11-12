#!/usr/bin/env bash
# Single-entry verification for lang-gc-program/1.
#
# Every build and every executing process is wrapped in `timeout`. This suite
# is a C++ runtime/ABI suite: it uses real LangVec, language threads and real
# fork paths, but it does NOT compile or claim a .wz Source program. The Source
# pressure target stays RED until the Native emitter emits registration, polls,
# critical regions and the main wrapper.

set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)

LLVM_ROOT=${LANG_LLVM_ROOT:-/home/qin/Work/language-dev-20261103/toolchains/llvm-22.1.8}
LIBXML2_ROOT=${LANG_LIBXML2_ROOT:-/home/qin/Work/language-dev-20261103/toolchains/libxml2-legacy-2.13.9}
if [ -n "${CXX:-}" ]; then
  COMPILER=$CXX
else
  COMPILER=$LLVM_ROOT/bin/clang++
fi
export PATH="$LLVM_ROOT/bin:$PATH"
export LD_LIBRARY_PATH="$LIBXML2_ROOT/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

for tool in timeout nm readelf file sha256sum; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "FAIL: required tool not found: $tool" >&2
    exit 1
  fi
done
if [ ! -x "$COMPILER" ]; then
  echo "FAIL: compiler not executable: $COMPILER" >&2
  exit 1
fi
VERSION=$("$COMPILER" --version | head -n 1)
case "$VERSION" in
  *"clang version 22.1.8"*) ;;
  *) echo "FAIL: expected fixed LLVM 22.1.8, got: $VERSION" >&2; exit 1 ;;
esac

if [ -n "${GC_PROGRAM_BUILD_DIR:-}" ]; then
  BUILD_DIR=$GC_PROGRAM_BUILD_DIR
  KEEP_BUILD=1
else
  BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/woozy-gc-program.XXXXXX")
  KEEP_BUILD=0
fi
mkdir -p "$BUILD_DIR"

PASS_COUNT=0
FAIL_COUNT=0
SKIP_COUNT=0

pass() { PASS_COUNT=$((PASS_COUNT + 1)); printf 'PASS  %s\n' "$1"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); printf 'FAIL  %s\n' "$1"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); printf 'SKIP  %s (%s)\n' "$1" "$2"; }
show_log() { [ -f "$1" ] && sed -e 's/^/    /' "$1" | tail -n 80; }

cleanup() {
  if [ "$KEEP_BUILD" = "0" ] && [ "$FAIL_COUNT" = "0" ]; then
    rm -rf "$BUILD_DIR"
  else
    echo "build artifacts: $BUILD_DIR"
  fi
}
trap cleanup EXIT

COMMON=(-std=c++20 -pthread -Wall -Wextra -Werror -pedantic-errors
  -I"$ROOT/runtime/native")
FREESTANDING=(-std=c++20 -O2 -ffreestanding -fno-builtin -fno-exceptions
  -fno-rtti -fno-threadsafe-statics -fno-stack-protector -fno-pic -fno-pie
  -Wall -Wextra -Werror -pedantic-errors -I"$ROOT/runtime/native")

build_step() {
  local name=$1
  shift
  local log=$BUILD_DIR/${name// /_}.log
  if timeout 300s "$@" >"$log" 2>&1; then
    pass "build: $name"
    return 0
  fi
  fail "build: $name"
  show_log "$log"
  return 1
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
  if ! grep -q "LANG_NATIVE_PANIC:v1:gc-world:$reason:" "$log"; then
    fail "$name (missing panic reason $reason)"
    show_log "$log"
    return 1
  fi
  pass "$name"
  return 0
}

run_expect() {
  local name=$1 timeout_s=$2 marker=$3
  shift 3
  local log=$BUILD_DIR/${name// /_}.log
  timeout --signal=TERM --kill-after=5s "${timeout_s}s" "$@" >"$log" 2>&1
  local status=$?
  if [ "$status" -ne 0 ]; then
    fail "$name (exit $status)"
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

HOST_SOURCES=(
  "$SCRIPT_DIR/gc_program_test.cpp"
  "$ROOT/runtime/native/gc_program.cpp"
  "$ROOT/runtime/native/gc_runtime.cpp"
  "$ROOT/runtime/native/gc_world.cpp"
  "$ROOT/runtime/native/gc.cpp"
  "$ROOT/runtime/native/weak.cpp"
  "$ROOT/runtime/native/panic.cpp"
  "$ROOT/runtime/native/alloc.cpp"
  "$ROOT/runtime/native/thread.cpp"
  "$ROOT/runtime/native/sync.cpp"
  "$ROOT/runtime/native/os.cpp"
  "$ROOT/runtime/native/system.cpp"
  "$ROOT/runtime/native/ipc.cpp"
  "$ROOT/runtime/native/process.cpp"
  "$ROOT/runtime/native/text.cpp"
  "$ROOT/tests/native/gc_runtime/test_alloc_wrap.cpp"
)
HOST_WRAP=(-Wl,--wrap=lang_alloc_v1 -Wl,--wrap=lang_free_v1)

run_host_modes() {
  local suffix=$1
  shift
  local modes=("$@")
  for mode in "${modes[@]}"; do
    local marker="GC_PROGRAM_$(echo "$mode" | tr 'a-z-' 'A-Z_')_OK"
    case "$mode" in
      lifecycle) marker=GC_PROGRAM_LIFECYCLE_OK ;;
      vec) marker=GC_PROGRAM_VEC_OK ;;
      pressure) marker=GC_PROGRAM_PRESSURE_OK ;;
      finish-survivor) marker=GC_PROGRAM_FINISH_SURVIVOR_OK ;;
      thread) marker=GC_PROGRAM_THREAD_OK ;;
      mutex) marker=GC_PROGRAM_MUTEX_OK ;;
      suspend) marker=GC_PROGRAM_SUSPEND_OK ;;
      suspend-critical) marker=GC_PROGRAM_SUSPEND_CRITICAL_OK ;;
      fork) marker=GC_PROGRAM_FORK_OK ;;
      fork-safe) marker=GC_PROGRAM_FORK_SAFE_OK ;;
      fork-critical) marker=GC_PROGRAM_FORK_CRITICAL_OK ;;
      fork-stop-safe) marker=GC_PROGRAM_FORK_STOP_SAFE_OK ;;
      fork-failure) marker=GC_PROGRAM_FORK_FAILURE_OK ;;
      disabled-fast) marker=GC_PROGRAM_DISABLED_FAST_OK ;;
    esac
    run_expect "host $suffix $mode" 60 "$marker" \
      "$BUILD_DIR/gc_program_test_$suffix" "$mode"
  done
}

if build_step "host O0" "$COMPILER" "${COMMON[@]}" -O0 -g \
    "${HOST_SOURCES[@]}" "${HOST_WRAP[@]}" \
    -o "$BUILD_DIR/gc_program_test_o0"; then
  run_host_modes o0 lifecycle vec pressure finish-survivor thread mutex suspend \
    suspend-critical fork fork-safe fork-critical fork-stop-safe fork-stop-active fork-failure \
    disabled-fast
fi

if build_step "host O2" "$COMPILER" "${COMMON[@]}" -O2 -g \
    "${HOST_SOURCES[@]}" "${HOST_WRAP[@]}" \
    -o "$BUILD_DIR/gc_program_test_o2"; then
  run_host_modes o2 lifecycle vec pressure finish-survivor thread mutex suspend \
    suspend-critical fork fork-safe fork-critical fork-stop-safe fork-stop-active fork-failure \
    disabled-fast
fi

# Replace the real allocator with the libc-heap shim so LeakSanitizer sees all
# runtime allocations.
ASAN_SOURCES=(
  "$SCRIPT_DIR/gc_program_test.cpp"
  "$ROOT/runtime/native/gc_program.cpp"
  "$ROOT/runtime/native/gc_runtime.cpp"
  "$ROOT/runtime/native/gc_world.cpp"
  "$ROOT/runtime/native/gc.cpp"
  "$ROOT/runtime/native/weak.cpp"
  "$ROOT/runtime/native/panic.cpp"
  "$ROOT/runtime/native/thread.cpp"
  "$ROOT/runtime/native/sync.cpp"
  "$ROOT/runtime/native/os.cpp"
  "$ROOT/runtime/native/system.cpp"
  "$ROOT/runtime/native/ipc.cpp"
  "$ROOT/runtime/native/process.cpp"
  "$ROOT/runtime/native/text.cpp"
  "$ROOT/tests/native/gc_runtime/test_alloc_asan.cpp"
)
if build_step "host ASAN+UBSAN" "$COMPILER" "${COMMON[@]}" -O1 -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    "${ASAN_SOURCES[@]}" -o "$BUILD_DIR/gc_program_test_asan"; then
  # Raw-clone language threads do not have an ASAN runtime TLS block in this
  # freestanding runner. The host O0/O2 builds cover thread/mutex SAFE; the
  # ASAN build covers lifecycle, real Vec, fork and world suspension.
  skip "host asan thread" "raw-clone runner has no ASAN TLS"
  skip "host asan mutex" "raw-clone runner has no ASAN TLS"
  for mode in lifecycle vec pressure finish-survivor suspend fork fork-safe fork-critical fork-stop-active; do
    marker=GC_PROGRAM_$(echo "$mode" | tr 'a-z-' 'A-Z_')_OK
    case "$mode" in
      lifecycle) marker=GC_PROGRAM_LIFECYCLE_OK ;;
      vec) marker=GC_PROGRAM_VEC_OK ;;
      pressure) marker=GC_PROGRAM_PRESSURE_OK ;;
      finish-survivor) marker=GC_PROGRAM_FINISH_SURVIVOR_OK ;;
      thread) marker=GC_PROGRAM_THREAD_OK ;;
      mutex) marker=GC_PROGRAM_MUTEX_OK ;;
      suspend) marker=GC_PROGRAM_SUSPEND_OK ;;
      fork) marker=GC_PROGRAM_FORK_OK ;;
      fork-safe) marker=GC_PROGRAM_FORK_SAFE_OK ;;
      fork-critical) marker=GC_PROGRAM_FORK_CRITICAL_OK ;;
    esac
    run_expect "host asan $mode" 120 "$marker" \
      env ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 \
      UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
      "$BUILD_DIR/gc_program_test_asan" "$mode"
  done
fi

HOSTED_SOURCES=(
  "$SCRIPT_DIR/gc_program_hosted_test.cpp"
  "$ROOT/runtime/native/gc_program.cpp"
  "$ROOT/runtime/native/gc_runtime.cpp"
  "$ROOT/runtime/native/gc_world.cpp"
  "$ROOT/runtime/native/gc.cpp"
  "$ROOT/runtime/native/weak.cpp"
  "$ROOT/runtime/native/panic.cpp"
  "$ROOT/runtime/native/alloc.cpp"
  "$ROOT/runtime/hosted/ffi_process.cpp"
  "$ROOT/runtime/hosted/process.cpp"
  "$ROOT/runtime/hosted/thread.cpp"
  "$ROOT/runtime/native/text.cpp"
)
HOSTED_INCLUDES=(-I"$ROOT/runtime/native" -I"$ROOT/runtime/hosted")

# Permanent F1 probe: real hosted libc pthread_atfork prepare callbacks and
# language fork, plus direct fork-token callback/SAFE cases. Genuine graph
# critical remains a terminal Panic.
F1_SOURCES=(
  "$SCRIPT_DIR/gc_program_callback_fork_test.cpp"
  "$ROOT/runtime/native/gc_program.cpp"
  "$ROOT/runtime/native/gc_runtime.cpp"
  "$ROOT/runtime/native/gc_world.cpp"
  "$ROOT/runtime/native/gc.cpp"
  "$ROOT/runtime/native/weak.cpp"
  "$ROOT/runtime/native/panic.cpp"
  "$ROOT/runtime/native/alloc.cpp"
  "$ROOT/runtime/native/os.cpp"
  "$ROOT/runtime/hosted/ffi_process.cpp"
  "$ROOT/runtime/hosted/process.cpp"
  "$ROOT/runtime/hosted/thread.cpp"
  "$ROOT/runtime/native/text.cpp"
)
F1_ASAN=(
  "$SCRIPT_DIR/gc_program_callback_fork_test.cpp"
  "$ROOT/runtime/native/gc_program.cpp"
  "$ROOT/runtime/native/gc_runtime.cpp"
  "$ROOT/runtime/native/gc_world.cpp"
  "$ROOT/runtime/native/gc.cpp"
  "$ROOT/runtime/native/weak.cpp"
  "$ROOT/runtime/native/panic.cpp"
  "$ROOT/runtime/native/os.cpp"
  "$ROOT/runtime/hosted/ffi_process.cpp"
  "$ROOT/runtime/hosted/process.cpp"
  "$ROOT/runtime/hosted/thread.cpp"
  "$ROOT/runtime/native/text.cpp"
  "$ROOT/tests/native/gc_runtime/test_alloc_asan.cpp"
)

run_f1_probe() {
  local suffix=$1 binary=$2
  run_expect "f1 $suffix control-callback" 30 CALLBACK_FORK_PROBE_OK "$binary" control-callback
  run_expect "f1 $suffix control-libc-prepare-callback" 30 CALLBACK_FORK_PROBE_OK "$binary" control-libc-prepare-callback
  run_expect_panic "f1 $suffix control-graph-critical" 30 callback-in-critical "$binary" control-graph-critical
  run_expect "f1 $suffix fork-token-callback" 30 CALLBACK_FORK_PROBE_OK "$binary" fork-token-callback
  run_expect "f1 $suffix fork-token-safe" 30 CALLBACK_FORK_PROBE_OK "$binary" fork-token-safe
  run_expect "f1 $suffix language-prepare-callback" 30 CALLBACK_FORK_PROBE_OK "$binary" language-prepare-callback
  run_expect "f1 $suffix language-prepare-safe" 30 CALLBACK_FORK_PROBE_OK "$binary" language-prepare-safe
}

for opt in O0 O2; do
  if build_step "hosted $opt" "$COMPILER" "${COMMON[@]}" "${HOSTED_INCLUDES[@]}" \
      -$opt -g "${HOSTED_SOURCES[@]}" \
      -o "$BUILD_DIR/gc_program_hosted_${opt,,}"; then
    for mode in language libc libc-safe failure; do
      case "$mode" in
        language) marker=GC_PROGRAM_HOSTED_LANGUAGE_FORK_OK ;;
        libc) marker=GC_PROGRAM_HOSTED_LIBC_FORK_OK ;;
        libc-safe) marker=GC_PROGRAM_HOSTED_LIBC_FORK_SAFE_OK ;;
        failure) marker=GC_PROGRAM_HOSTED_FORK_FAILURE_OK ;;
      esac
      run_expect "hosted $opt $mode" 60 "$marker" \
        "$BUILD_DIR/gc_program_hosted_${opt,,}" "$mode"
    done
  fi
done

HOSTED_ASAN=(
  "$SCRIPT_DIR/gc_program_hosted_test.cpp"
  "$ROOT/runtime/native/gc_program.cpp"
  "$ROOT/runtime/native/gc_runtime.cpp"
  "$ROOT/runtime/native/gc_world.cpp"
  "$ROOT/runtime/native/gc.cpp"
  "$ROOT/runtime/native/weak.cpp"
  "$ROOT/runtime/native/panic.cpp"
  "$ROOT/runtime/hosted/ffi_process.cpp"
  "$ROOT/runtime/hosted/process.cpp"
  "$ROOT/runtime/hosted/thread.cpp"
  "$ROOT/runtime/native/text.cpp"
  "$ROOT/tests/native/gc_runtime/test_alloc_asan.cpp"
)
if build_step "hosted ASAN+UBSAN" "$COMPILER" "${COMMON[@]}" "${HOSTED_INCLUDES[@]}" \
    -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    "${HOSTED_ASAN[@]}" -o "$BUILD_DIR/gc_program_hosted_asan"; then
  for mode in language libc libc-safe; do
    case "$mode" in
      language) marker=GC_PROGRAM_HOSTED_LANGUAGE_FORK_OK ;;
      libc) marker=GC_PROGRAM_HOSTED_LIBC_FORK_OK ;;
      libc-safe) marker=GC_PROGRAM_HOSTED_LIBC_FORK_SAFE_OK ;;
    esac
    run_expect "hosted asan $mode" 120 "$marker" \
      env ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 \
      UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
      "$BUILD_DIR/gc_program_hosted_asan" "$mode"
  done
fi

# F1 probe at O0/O2 with real hosted atfork and language fork.
for opt in O0 O2; do
  if build_step "f1 probe $opt" "$COMPILER" "${COMMON[@]}" "${HOSTED_INCLUDES[@]}" \
      -$opt -g "${F1_SOURCES[@]}" \
      -o "$BUILD_DIR/gc_program_f1_${opt,,}"; then
    run_f1_probe "$opt" "$BUILD_DIR/gc_program_f1_${opt,,}"
  fi
done
if build_step "f1 probe ASAN+UBSAN" "$COMPILER" "${COMMON[@]}" "${HOSTED_INCLUDES[@]}" \
    -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    "${F1_ASAN[@]}" -o "$BUILD_DIR/gc_program_f1_asan"; then
  run_f1_probe "asan" "$BUILD_DIR/gc_program_f1_asan"
fi

# Freestanding static ELF with the program lifecycle and final collection.
STATIC_OK=1
for source in gc_program gc_runtime gc_world gc weak panic alloc freestanding; do
  build_step "static object $source" "$COMPILER" "${FREESTANDING[@]}" \
    -c "$ROOT/runtime/native/$source.cpp" \
    -o "$BUILD_DIR/$source.o" || STATIC_OK=0
done
build_step "static object test" "$COMPILER" "${FREESTANDING[@]}" \
  -c "$SCRIPT_DIR/gc_program_static_test.cpp" -o "$BUILD_DIR/static_test.o" \
  || STATIC_OK=0
build_step "core static object test" "$COMPILER" "${FREESTANDING[@]}" \
  -c "$SCRIPT_DIR/gc_program_core_static_test.cpp" -o "$BUILD_DIR/core_test.o" \
  || STATIC_OK=0
build_step "core static start object" "$COMPILER" "${FREESTANDING[@]}" \
  -c "$ROOT/runtime/native/start.cpp" -o "$BUILD_DIR/start.o" \
  || STATIC_OK=0

check_static_elf() {
  local name=$1 elf=$2 marker=$3 timeout_s=$4
  if file "$elf" | grep -q 'statically linked'; then
    pass "$name is statically linked"
  else
    fail "$name is not statically linked"
  fi
  if readelf -d "$elf" 2>&1 | grep -q NEEDED; then
    fail "$name has dynamic NEEDED entries"
  else
    pass "$name has no dynamic dependencies"
  fi
  if readelf -l "$elf" 2>&1 | grep -q INTERP; then
    fail "$name has an interpreter"
  else
    pass "$name has no interpreter"
  fi
  if nm -u "$elf" | grep -q .; then
    fail "$name has undefined symbols"
    nm -u "$elf" | sed -e 's/^/    /' | head -20
  else
    pass "$name has no undefined symbols"
  fi
  if grep -a -q 'libstdc++\|libc\.so\|libgcc_s' "$elf"; then
    fail "$name references libc/libstdc++ strings"
  else
    pass "$name has no libc/libstdc++ dependency strings"
  fi
  if [ -n "$marker" ]; then
    run_expect "$name execution" "$timeout_s" "$marker" "$elf"
  else
    run_expect "$name execution" "$timeout_s" "" "$elf"
  fi
}

if [ "$STATIC_OK" = "1" ]; then
  if build_step "static ELF link" "$COMPILER" -nostdlib -static \
      -Wl,-e,_start -Wl,-z,noexecstack \
      "$BUILD_DIR/gc_program.o" "$BUILD_DIR/gc_runtime.o" \
      "$BUILD_DIR/gc_world.o" "$BUILD_DIR/gc.o" "$BUILD_DIR/weak.o" \
      "$BUILD_DIR/panic.o" "$BUILD_DIR/alloc.o" "$BUILD_DIR/freestanding.o" \
      "$BUILD_DIR/static_test.o" -o "$BUILD_DIR/gc_program_static"; then
    check_static_elf "static program ELF" "$BUILD_DIR/gc_program_static" \
      GC_PROGRAM_STATIC_OK 60
  fi
  if build_step "core static ELF link" "$COMPILER" -nostdlib -static \
      -Wl,-e,_start -Wl,-z,noexecstack \
      "$BUILD_DIR/start.o" "$BUILD_DIR/gc_program.o" \
      "$BUILD_DIR/gc_runtime.o" "$BUILD_DIR/gc_world.o" "$BUILD_DIR/gc.o" \
      "$BUILD_DIR/weak.o" "$BUILD_DIR/panic.o" "$BUILD_DIR/alloc.o" \
      "$BUILD_DIR/freestanding.o" "$BUILD_DIR/core_test.o" \
      -o "$BUILD_DIR/gc_program_core_static"; then
    check_static_elf "default core static ELF" \
      "$BUILD_DIR/gc_program_core_static" "" 30
  fi
fi

# Old accepted suites: world 42, callback, controller 25, Weak, and the
# freestanding thread/sync host harnesses.
if [ "${RUN_LEGACY:-1}" = "1" ]; then
  if timeout 1200s env CXX="$COMPILER" \
      GC_WORLD_BUILD_DIR="$BUILD_DIR/legacy-world" \
      bash "$ROOT/tests/native/gc_world/run_all.sh" \
      >"$BUILD_DIR/legacy_world.log" 2>&1; then
    pass "legacy gc_world suite"
  else
    fail "legacy gc_world suite"
    show_log "$BUILD_DIR/legacy_world.log"
  fi
  if timeout 1200s env CXX="$COMPILER" \
      GC_WORLD_BUILD_DIR="$BUILD_DIR/legacy-callback" \
      bash "$ROOT/tests/native/gc_world/run_callback_all.sh" \
      >"$BUILD_DIR/legacy_callback.log" 2>&1; then
    pass "legacy gc_world callback suite"
  else
    fail "legacy gc_world callback suite"
    show_log "$BUILD_DIR/legacy_callback.log"
  fi
  if timeout 1500s env CXX="$COMPILER" \
      GC_RUNTIME_BUILD_DIR="$BUILD_DIR/legacy-controller" \
      bash "$ROOT/tests/native/gc_runtime/run_all.sh" \
      >"$BUILD_DIR/legacy_controller.log" 2>&1; then
    pass "legacy gc_runtime controller suite"
  else
    fail "legacy gc_runtime controller suite"
    show_log "$BUILD_DIR/legacy_controller.log"
  fi
  if timeout 1500s env CXX="$COMPILER" \
      LANG_WEAK_BUILD_DIR="$BUILD_DIR/legacy-weak" \
      bash "$ROOT/tests/native/weak/run.sh" \
      >"$BUILD_DIR/legacy_weak.log" 2>&1; then
    pass "legacy Weak suite"
  else
    fail "legacy Weak suite"
    show_log "$BUILD_DIR/legacy_weak.log"
  fi

  LEGACY_THREAD_SOURCES=(
    "$ROOT/tests/native/thread_runtime_test.cpp"
    "$ROOT/runtime/native/thread.cpp" "$ROOT/runtime/native/sync.cpp"
    "$ROOT/runtime/native/alloc.cpp" "$ROOT/runtime/native/panic.cpp"
    "$ROOT/runtime/native/weak.cpp")
  if build_step "legacy thread runtime harness" "$COMPILER" \
      "${COMMON[@]}" -O1 -g "${LEGACY_THREAD_SOURCES[@]}" \
      -o "$BUILD_DIR/legacy_thread_runtime" &&
    run_expect "legacy thread runtime harness" 120 "passed:" \
      "$BUILD_DIR/legacy_thread_runtime"; then :; fi
  LEGACY_SYNC_SOURCES=(
    "$ROOT/tests/native/sync_runtime_test.cpp"
    "$ROOT/runtime/native/sync.cpp" "$ROOT/runtime/native/thread.cpp"
    "$ROOT/runtime/native/alloc.cpp" "$ROOT/runtime/native/panic.cpp"
    "$ROOT/runtime/native/weak.cpp")
  if build_step "legacy sync runtime harness" "$COMPILER" \
      "${COMMON[@]}" -O1 -g "${LEGACY_SYNC_SOURCES[@]}" \
      -Wl,--wrap=lang_free_v1 -o "$BUILD_DIR/legacy_sync_runtime" &&
    run_expect "legacy sync runtime harness" 120 "sync-runtime-ok" \
      "$BUILD_DIR/legacy_sync_runtime"; then :; fi
fi

echo
echo "compiler: $VERSION"
echo "sha256:"
for file in \
  runtime/native/gc_program.hpp runtime/native/gc_program.cpp \
  runtime/native/gc_world.hpp runtime/native/gc_world.cpp \
  runtime/native/panic.cpp runtime/native/thread.cpp \
  runtime/native/sync.cpp runtime/native/os.cpp runtime/native/system.cpp \
  runtime/native/network.cpp runtime/native/ipc.cpp \
  runtime/native/process.cpp runtime/hosted/thread.cpp \
  runtime/hosted/sources.cmake src/backend/native/CMakeLists.txt \
  tests/native/gc_program/gc_program_test.cpp \
  tests/native/gc_program/gc_program_hosted_test.cpp \
  tests/native/gc_program/gc_program_callback_fork_test.cpp \
  tests/native/gc_program/gc_program_static_test.cpp \
  tests/native/gc_program/gc_program_core_static_test.cpp \
  tests/native/gc_program/CONTRACT.md \
  tests/native/gc_program/run_all.sh; do
  sha256sum "$ROOT/$file"
done
echo
echo "SUMMARY pass=$PASS_COUNT fail=$FAIL_COUNT skip=$SKIP_COUNT"
if [ "$FAIL_COUNT" -ne 0 ]; then
  exit 1
fi
exit 0
