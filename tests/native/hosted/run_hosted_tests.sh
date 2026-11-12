#!/usr/bin/env bash
# Build the hosted runtime archive and run the standalone hosted C++ harnesses.
# This script is deliberately self-contained: it does not modify the existing
# root CMake projects or the freestanding runtime archive.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
CLANG_DIR="${CLANG_DIR:-/home/qin/Work/language-dev-20261103/toolchains/llvm-22.1.8/bin}"
CXX="${CXX:-$CLANG_DIR/clang++}"
CC="${CC:-$CLANG_DIR/clang}"
AR="${AR:-$CLANG_DIR/llvm-ar}"
READELF="${READELF:-$CLANG_DIR/llvm-readelf}"
NM="${NM:-$CLANG_DIR/llvm-nm}"
BUILD="${BUILD:-$ROOT/tests/native/hosted/.build}"
# The fixed LLVM 22.1.8 ld.lld was built against the legacy libxml2 package.
LIBXML_LEGACY="${LIBXML_LEGACY:-/home/qin/Work/language-dev-20261103/toolchains/libxml2-legacy-2.13.9/usr/lib}"
export LD_LIBRARY_PATH="$LIBXML_LEGACY${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
COMPILE_TIMEOUT="${COMPILE_TIMEOUT:-120}"
RUN_TIMEOUT="${RUN_TIMEOUT:-90}"

LOG="$BUILD/session.log"
REPORT="$BUILD/summary.txt"
rm -rf "$BUILD"
mkdir -p "$BUILD"
: >"$LOG"

log() { printf '%s\n' "$*" | tee -a "$LOG" >&2; }
need_file() { [[ -x "$1" ]] || { log "missing tool: $1"; exit 1; }; }
for tool in "$CXX" "$CC" "$AR" "$READELF" "$NM"; do need_file "$tool"; done

VERSION="$("$CXX" --version | head -n1)"
case "$VERSION" in
  *22.1.8*) ;;
  *) log "expected Clang 22.1.8, got: $VERSION"; exit 1 ;;
esac
log "toolchain: $VERSION"

PUBLIC_SOURCES=(
  alloc panic os os_buffers sync text system system_buffers ipc dns
  network_buffers network numbers
)
CXX_COMMON=(
  "$CXX" -std=c++20 -O2 -fPIC -ffreestanding -fno-builtin -fno-exceptions
  -fno-rtti -fno-sanitize=all -fno-builtin-memcpy -fno-builtin-memset
  -fno-builtin-memmove -I"$ROOT/runtime/native"
)
CXX_HOSTED=(
  "$CXX" -std=c++20 -O2 -fPIC -pthread -fstack-protector-strong -fno-rtti
  -fno-exceptions -I"$ROOT/runtime/native"
)
C_MATH=(
  "$CC" -std=c99 -O2 -fPIC -ffreestanding -fno-builtin -fno-common
  -fno-strict-aliasing -fno-math-errno -fno-trapping-math -fno-fast-math
  -ffp-contract=off -Wno-parentheses -Wno-shift-op-parentheses
  -Wno-unknown-pragmas -I"$ROOT/runtime/native/math"
  -include "$ROOT/runtime/native/math/math_prefix.h"
)

compile_cxx() {
  local source="$1" output="$2"
  shift 2
  timeout "$COMPILE_TIMEOUT" "${CXX_COMMON[@]}" "$@" -c "$source" -o "$output"
}
compile_hosted() {
  local source="$1" output="$2"
  timeout "$COMPILE_TIMEOUT" "${CXX_HOSTED[@]}" -c "$source" -o "$output"
}
compile_c() {
  local source="$1" output="$2"
  timeout "$COMPILE_TIMEOUT" "${C_MATH[@]}" -c "$source" -o "$output"
}

build_common() {
  local out="$1"
  mkdir -p "$out"
  for name in "${PUBLIC_SOURCES[@]}"; do
    compile_cxx "$ROOT/runtime/native/$name.cpp" "$out/$name.o"
  done
  compile_cxx "$ROOT/runtime/native/math/support.cpp" "$out/math_support.o"
  while IFS= read -r source; do
    local name
    name="$(basename "$source" .c)"
    compile_c "$source" "$out/math_$name.o"
  done < <(find "$ROOT/runtime/native/math/vendor" -maxdepth 1 -name '*.c' -print | sort)
}

archive() {
  local output="$1"
  shift
  local objects=("$@")
  rm -f "$output"
  timeout "$COMPILE_TIMEOUT" "$AR" rcD "$output" "${objects[@]}"
}

build_native() {
  local out="$1"
  log "building native freestanding archive in $out"
  build_common "$out"
  compile_cxx "$ROOT/runtime/native/process.cpp" "$out/process.o"
  compile_cxx "$ROOT/runtime/native/start.cpp" "$out/native_start.o"
  compile_cxx "$ROOT/runtime/native/thread.cpp" "$out/native_thread.o"
  compile_cxx "$ROOT/runtime/native/freestanding.cpp" "$out/native_freestanding.o"
  archive "$out/libnative_runtime.a" "$out"/*.o
}

build_hosted_archive() {
  local out="$1"
  log "building hosted archive in $out"
  build_common "$out"
  compile_cxx "$ROOT/runtime/native/process.cpp" "$out/process.o" \
    -DLANG_RUNTIME_HOSTED
  compile_hosted "$ROOT/runtime/hosted/start.cpp" "$out/hosted_start.o"
  compile_hosted "$ROOT/runtime/hosted/thread.cpp" "$out/hosted_thread.o"
  compile_hosted "$ROOT/runtime/hosted/process.cpp" "$out/hosted_process.o"
  archive "$out/libhosted_runtime.a" "$out"/*.o
  if "$NM" -u "$out/libhosted_runtime.a" 2>/dev/null |
      grep -Eq '(__cxa_|__gxx_personality|_ZSt9terminatev|_Unwind_)'; then
    log "hosted runtime acquired a C++ exception dependency"
    "$NM" -u "$out/libhosted_runtime.a" 2>/dev/null |
      grep -E '(__cxa_|__gxx_personality|_ZSt9terminatev|_Unwind_)' >&2 || true
    exit 1
  fi
}

build_native_program() {
  local directory="$1" archive_path="$2" program="$3"
  mkdir -p "$directory"
  printf 'extern "C" int main() { return 23; }\n' >"$directory/main.cpp"
  timeout "$COMPILE_TIMEOUT" "$CXX" -std=c++20 -O2 -ffreestanding -fno-builtin \
    -fno-exceptions -fno-rtti -fno-stack-protector -fno-pic -fno-pie \
    -c "$directory/main.cpp" -o "$directory/main.o"
  timeout "$COMPILE_TIMEOUT" "$CXX" --target=x86_64-unknown-linux-gnu -nostdlib \
    -static --ld-path="$CLANG_DIR/ld.lld" -Wl,-e,_start -Wl,--build-id=none \
    "$directory/main.o" "$archive_path" -o "$program"
}

run_native_program() {
  local program="$1"
  set +e
  timeout "$RUN_TIMEOUT" "$program" >"$BUILD/native.stdout" 2>"$BUILD/native.stderr"
  local status=$?
  set -e
  [[ $status -eq 23 ]] || {
    log "native program returned $status, expected 23"
    cat "$BUILD/native.stderr" >&2
    exit 1
  }
  log "native freestanding program ran and returned 23"
}

record_readelf() {
  local program="$1" output="$2"
  "$READELF" -h -l -d -s "$program" >"$output"
  grep -E 'INTERP|\(NEEDED\)' "$output" >"$output.needed" || true
  log "readelf: $(basename "$program") -> $output"
  cat "$output.needed" | tee -a "$LOG" >&2
}

source_digest() {
  find "$ROOT/runtime/native" -type f -print0 | sort -z |
    xargs -0 sha256sum | sha256sum | cut -d' ' -f1
}

# Preserve the current inputs, including unrelated existing worktree changes.
# This standalone build writes only BUILD and compares source/archive/program
# digests before and after; it does not require a clean shared checkout.
SOURCE_BEFORE="$(source_digest)"

# Prove the freestanding branch of process.cpp still compiles to the same
# object as HEAD when LANG_RUNTIME_HOSTED is not defined.
mkdir -p "$BUILD/process-baseline"
git -C "$ROOT" show HEAD:runtime/native/process.cpp >"$BUILD/process-baseline/process.cpp"
compile_cxx "$ROOT/runtime/native/process.cpp" "$BUILD/process-baseline/current-no-hosted.o"
compile_cxx "$BUILD/process-baseline/process.cpp" "$BUILD/process-baseline/head-no-hosted.o"
PROCESS_CURRENT="$(sha256sum "$BUILD/process-baseline/current-no-hosted.o" | cut -d' ' -f1)"
PROCESS_HEAD="$(sha256sum "$BUILD/process-baseline/head-no-hosted.o" | cut -d' ' -f1)"
[[ "$PROCESS_CURRENT" == "$PROCESS_HEAD" ]] || {
  log "freestanding process.cpp object differs from HEAD without LANG_RUNTIME_HOSTED"
  exit 1
}
log "freestanding process.cpp branch unchanged: $PROCESS_CURRENT"

# 1. Freeze and run the original freestanding archive/program.
build_native "$BUILD/native-before"
build_native_program "$BUILD/native-program-before" \
  "$BUILD/native-before/libnative_runtime.a" "$BUILD/native-program-before/program"
record_readelf "$BUILD/native-program-before/program" \
  "$BUILD/native-program-before/readelf.txt"
run_native_program "$BUILD/native-program-before/program"
log "running the existing native thread_runtime_test against the untouched archive"
timeout "$COMPILE_TIMEOUT" "$CXX" -std=c++20 -O2 -g -pthread \
  -fstack-protector-strong -I"$ROOT" \
  "$ROOT/tests/native/thread_runtime_test.cpp" \
  "$BUILD/native-before/libnative_runtime.a" \
  -o "$BUILD/native_thread_runtime_test"
timeout "$RUN_TIMEOUT" "$BUILD/native_thread_runtime_test" \
  2>&1 | tee -a "$LOG" >&2
NATIVE_ARCHIVE_BEFORE="$(sha256sum "$BUILD/native-before/libnative_runtime.a" | cut -d' ' -f1)"
NATIVE_PROGRAM_BEFORE="$(sha256sum "$BUILD/native-program-before/program" | cut -d' ' -f1)"

# 2. Build the hosted archive from the same public sources with the hosted
#    start/thread replacements, then link normal CRT programs.
build_hosted_archive "$BUILD/hosted"
HOSTED_ARCHIVE="$BUILD/hosted/libhosted_runtime.a"
for opt in 2 0; do
  start="$BUILD/hosted_start_test-O$opt"
  crt="$BUILD/hosted_crt_main_test-O$opt"
  alloc_crt="$BUILD/hosted_alloc_crt_test-O$opt"
  alloc_fallback="$BUILD/hosted_alloc_fallback_test-O$opt"
  process="$BUILD/hosted_process_test-O$opt"
  thread="$BUILD/hosted_thread_test-O$opt"
  timeout "$COMPILE_TIMEOUT" "$CXX" -std=c++20 "-O$opt" -g -pthread \
    -fstack-protector-strong -I"$ROOT" \
    "$ROOT/tests/native/hosted/hosted_start_test.cpp" \
    -Wl,-u,lang_hosted_startup_v1 "$HOSTED_ARCHIVE" -o "$start"
  timeout "$COMPILE_TIMEOUT" "$CXX" -std=c++20 "-O$opt" -g -pthread \
    -fno-exceptions -fno-rtti -fstack-protector-strong -I"$ROOT" \
    "$ROOT/tests/native/hosted/hosted_crt_main_test.cpp" \
    -Wl,-u,lang_hosted_startup_v1 "$HOSTED_ARCHIVE" -nostdlib++ \
    -static-libgcc -o "$crt"
  timeout "$COMPILE_TIMEOUT" "$CXX" -std=c++20 "-O$opt" -g -pthread \
    -fno-exceptions -fno-rtti -fstack-protector-strong -I"$ROOT" \
    -c "$ROOT/tests/native/hosted/hosted_alloc_crt_test.cpp" -o "$alloc_crt.o"
  "$NM" -u "$alloc_crt.o" >"$alloc_crt.undefined.txt"
  for hook in lang_hosted_process_atfork_prepare_v1 lang_hosted_process_atfork_child_v1; do
    grep -Eq "[[:space:]]w $hook\$" "$alloc_crt.undefined.txt" || {
      log "allocator-only fixture must refer weakly to $hook"; exit 1;
    }
  done
  timeout "$COMPILE_TIMEOUT" "$CXX" -pthread "$alloc_crt.o" \
    -Wl,-u,lang_hosted_startup_v1 "$HOSTED_ARCHIVE" -nostdlib++ \
    -static-libgcc -o "$alloc_crt"
  # Rename the weak data references to intentionally undefined weak symbols.
  # This reaches the fallback without removing any real glibc/CRT symbols.
  timeout "$COMPILE_TIMEOUT" "$CXX" -pthread "$alloc_crt.o" \
    -Wl,-u,lang_hosted_startup_v1 -Wl,--wrap=__libc_stack_end \
    -Wl,--wrap=environ "$HOSTED_ARCHIVE" -nostdlib++ \
    -static-libgcc -o "$alloc_fallback"
  "$NM" -u "$alloc_fallback" >"$alloc_fallback.undefined.txt"
  for symbol in __wrap___libc_stack_end __wrap_environ; do
    grep -Eq "[[:space:]]w $symbol\$" "$alloc_fallback.undefined.txt" || {
      log "startup fallback must have an unresolved weak $symbol"; exit 1;
    }
  done
  timeout "$COMPILE_TIMEOUT" "$CXX" -std=c++20 "-O$opt" -g -pthread \
    -fstack-protector-strong -I"$ROOT" \
    "$ROOT/tests/native/hosted/hosted_process_test.cpp" \
    -Wl,-u,lang_hosted_startup_v1 -Wl,--wrap=fork \
    -Wl,--wrap=lang_hosted_process_atfork_prepare_v1 \
    "$HOSTED_ARCHIVE" -o "$process"
  timeout "$COMPILE_TIMEOUT" "$CXX" -std=c++20 "-O$opt" -g -pthread \
    -fstack-protector-strong -I"$ROOT" \
    "$ROOT/tests/native/hosted/hosted_thread_test.cpp" \
    -Wl,-u,lang_hosted_startup_v1 -Wl,--wrap=lang_alloc_v1 \
    -Wl,--wrap=lang_free_v1 -Wl,--wrap=pthread_cond_init \
    -Wl,--wrap=pthread_cond_timedwait -Wl,--wrap=clock_gettime \
    "$HOSTED_ARCHIVE" -lcrypto -o "$thread"
  record_readelf "$start" "$start.readelf.txt"
  record_readelf "$crt" "$crt.readelf.txt"
  record_readelf "$alloc_crt" "$alloc_crt.readelf.txt"
  record_readelf "$alloc_fallback" "$alloc_fallback.readelf.txt"
  record_readelf "$process" "$process.readelf.txt"
  record_readelf "$thread" "$thread.readelf.txt"
  grep -q 'libc.so.6' "$start.readelf.txt.needed" || {
    log "hosted start fixture has no libc dependency"; exit 1;
  }
  grep -q 'libcrypto' "$thread.readelf.txt.needed" || {
    log "hosted thread fixture has no libcrypto dependency"; exit 1;
  }
  if grep -q 'libstdc++' "$crt.readelf.txt.needed" \
      "$alloc_crt.readelf.txt.needed" "$alloc_fallback.readelf.txt.needed"; then
    log "hosted no-exception CRT fixture unexpectedly needs libstdc++"
    exit 1
  fi
  log "running hosted no-argument CRT main fixture at O$opt"
  timeout "$RUN_TIMEOUT" env HOSTED_NOARG_ENV=hosted-noarg-value \
    "$crt" >/dev/null 2>&1 || {
      log "hosted no-argument CRT main fixture failed"; exit 1;
    }
  log "hosted no-argument CRT main fixture passed at O$opt"
  log "running hosted allocator-only CRT fixture at O$opt"
  timeout "$RUN_TIMEOUT" "$alloc_crt" 2>&1 | tee -a "$LOG" >&2
  log "running hosted absent-weak-startup-vectors fixture at O$opt"
  timeout "$RUN_TIMEOUT" "$alloc_fallback" 2>&1 | tee -a "$LOG" >&2
  log "running hosted startup fixture at O$opt"
  timeout "$RUN_TIMEOUT" env HOSTED_START_ENV=hosted-start-value \
    "$start" alpha beta 2>&1 | tee -a "$LOG" >&2
  log "running hosted thread fixture at O$opt"
  timeout "$RUN_TIMEOUT" env HOSTED_TEST_ENV=hosted-env-value \
    "$thread" 2>&1 | tee -a "$LOG" >&2
  log "running hosted process/fork fixture at O$opt"
  timeout "$RUN_TIMEOUT" env HOSTED_FORK_ENV=hosted-fork-value \
    "$process" 2>&1 | tee -a "$LOG" >&2
done

# 3. Rebuild the native archive/program after the hosted tests and compare.
build_native "$BUILD/native-after"
build_native_program "$BUILD/native-program-after" \
  "$BUILD/native-after/libnative_runtime.a" "$BUILD/native-program-after/program"
record_readelf "$BUILD/native-program-after/program" \
  "$BUILD/native-program-after/readelf.txt"
run_native_program "$BUILD/native-program-after/program"
NATIVE_ARCHIVE_AFTER="$(sha256sum "$BUILD/native-after/libnative_runtime.a" | cut -d' ' -f1)"
NATIVE_PROGRAM_AFTER="$(sha256sum "$BUILD/native-program-after/program" | cut -d' ' -f1)"
SOURCE_AFTER="$(source_digest)"
[[ "$SOURCE_BEFORE" == "$SOURCE_AFTER" ]] || {
  log "runtime/native source digest changed during the hosted test"; exit 1;
}
[[ "$NATIVE_ARCHIVE_BEFORE" == "$NATIVE_ARCHIVE_AFTER" ]] || {
  log "freestanding archive hash changed"; exit 1;
}
[[ "$NATIVE_PROGRAM_BEFORE" == "$NATIVE_PROGRAM_AFTER" ]] || {
  log "freestanding program hash changed"; exit 1;
}

{
  echo "hosted runtime validation"
  echo "toolchain: $VERSION"
  echo "source root: $ROOT"
  echo "runtime/native source digest before: $SOURCE_BEFORE"
  echo "runtime/native source digest after:  $SOURCE_AFTER"
  echo "freestanding archive before: $NATIVE_ARCHIVE_BEFORE"
  echo "freestanding archive after:  $NATIVE_ARCHIVE_AFTER"
  echo "freestanding program before: $NATIVE_PROGRAM_BEFORE"
  echo "freestanding program after:  $NATIVE_PROGRAM_AFTER"
  echo "hosted archive: $(sha256sum "$HOSTED_ARCHIVE" | cut -d' ' -f1)"
  echo "freestanding process.cpp branch object: $PROCESS_CURRENT"
  if "$NM" -u "$HOSTED_ARCHIVE" 2>/dev/null |
      grep -Eq '(__cxa_|__gxx_personality|_ZSt9terminatev|_Unwind_)'; then
    echo "hosted archive C++ EH symbols: PRESENT"
  else
    echo "hosted archive C++ EH symbols: none"
  fi
  echo
  echo "hosted start NEEDED:"
  cat "$BUILD/hosted_start_test-O2.readelf.txt.needed"
  echo "hosted thread NEEDED:"
  cat "$BUILD/hosted_thread_test-O2.readelf.txt.needed"
  echo "hosted no-exception CRT NEEDED:"
  cat "$BUILD/hosted_crt_main_test-O2.readelf.txt.needed"
  echo "hosted process NEEDED:"
  cat "$BUILD/hosted_process_test-O2.readelf.txt.needed"
  echo "native program NEEDED/INTERP:"
  cat "$BUILD/native-program-after/readelf.txt.needed"
  echo
  echo "test log:"
  cat "$LOG"
} >"$REPORT"

log "passed: hosted startup/thread/process fixtures; freestanding archive and program hashes unchanged"
log "summary: $REPORT"
