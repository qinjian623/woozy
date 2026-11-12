#!/usr/bin/env bash
# Real-object Weak runtime verification for native and hosted runtimes.
#
# Covers:
#   * real LangVec, Box-cell Vec aliases, Mutex/AtomicI64 LangHandle
#   * runner-last and waiter-last Thread handle final release
#   * repeated downgrade, closed-slot no-reinstall, address reuse
#   * upgrade racing the final Vec release
#   * fork before/after slot installation and fork-failure parent hooks
#   * host O0/O2 native+hosted, hosted ASAN/UBSAN, native -nostdlib O0/O2
#
# It does not modify root CMake state. The test registration patch in this
# directory is the integration handoff for the CMake owner.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
OUT="${WEAK_OBJECTS_OUT:-$HERE/.build}"
LLVM_ROOT="${LANG_LLVM_ROOT:-/home/qin/Work/language-dev-20261103/toolchains/llvm-22.1.8}"
LIBXML2_ROOT="${LANG_LIBXML2_ROOT:-/home/qin/Work/language-dev-20261103/toolchains/libxml2-legacy-2.13.9}"
CXX="${CXX:-$LLVM_ROOT/bin/clang++}"
AR="${AR:-$LLVM_ROOT/bin/llvm-ar}"
NM="${NM:-$LLVM_ROOT/bin/llvm-nm}"
READELF="${READELF:-$LLVM_ROOT/bin/llvm-readelf}"
mkdir -p "$OUT"

export PATH="$LLVM_ROOT/bin:$PATH"
export LD_LIBRARY_PATH="$LIBXML2_ROOT/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

if [[ ! -x "$CXX" ]]; then
  echo "missing fixed LLVM 22.1.8 clang++: $CXX" >&2
  exit 2
fi
VERSION="$("$CXX" --version | head -n 1)"
if [[ "$VERSION" != *"clang version 22.1.8"* ]]; then
  echo "expected fixed LLVM 22.1.8, got: $VERSION" >&2
  exit 2
fi

run_limited() {
  local seconds="$1"
  shift
  printf '+ '
  printf '%q ' "$@"
  printf '\n'
  timeout "${seconds}s" "$@"
}

RUNTIME_INCLUDES=(-I "$ROOT/runtime/native")
WRAP=(-fuse-ld=lld -Wl,--wrap=lang_alloc_v1 -Wl,--wrap=lang_free_v1)
TEST="$HERE/weak_objects_test.cpp"
OBSERVER="$HERE/weak_objects_alloc_observer.cpp"
FAILURE_TEST="$HERE/weak_objects_fork_failure_test.cpp"

runtime_flags() {
  local opt="$1"
  printf '%s\n' -std=c++20 "-$opt" -g -ffreestanding -fno-builtin \
    -fno-exceptions -fno-rtti -fno-sanitize=all -fno-builtin-memcpy \
    -fno-builtin-memset -fno-builtin-memmove -fno-stack-protector
}

host_flags() {
  local opt="$1"
  printf '%s\n' -std=c++20 "-$opt" -g -pthread -Wall -Wextra -Wpedantic
}

compile_runtime_object() {
  local source="$1" output="$2" opt="$3"
  mapfile -t flags < <(runtime_flags "$opt")
  run_limited 120 "$CXX" "${flags[@]}" "${RUNTIME_INCLUDES[@]}" \
    -c "$source" -o "$output"
}

build_native_archive() {
  local directory="$1" opt="$2"
  mkdir -p "$directory"
  local sources=(alloc weak panic thread sync text ipc process)
  for name in "${sources[@]}"; do
    compile_runtime_object "$ROOT/runtime/native/$name.cpp" \
      "$directory/$name.o" "$opt"
  done
  compile_runtime_object "$ROOT/runtime/native/freestanding.cpp" \
    "$directory/freestanding.o" "$opt"
  compile_runtime_object "$ROOT/runtime/native/start.cpp" \
    "$directory/start.o" "$opt"
  rm -f "$directory/libnative_runtime.a"
  run_limited 60 "$AR" rcD "$directory/libnative_runtime.a" \
    "$directory"/*.o
}

build_hosted_archive() {
  local directory="$1" opt="$2"
  mkdir -p "$directory"
  local common=(alloc weak panic sync text ipc)
  for name in "${common[@]}"; do
    compile_runtime_object "$ROOT/runtime/native/$name.cpp" \
      "$directory/$name.o" "$opt"
  done
  mapfile -t flags < <(runtime_flags "$opt")
  run_limited 120 "$CXX" "${flags[@]}" "${RUNTIME_INCLUDES[@]}" \
    -DLANG_RUNTIME_HOSTED -c "$ROOT/runtime/native/process.cpp" \
    -o "$directory/process.o"
  local hosted=(
    "$ROOT/runtime/hosted/start.cpp"
    "$ROOT/runtime/hosted/thread.cpp"
    "$ROOT/runtime/hosted/process.cpp")
  for source in "${hosted[@]}"; do
    run_limited 120 "$CXX" -std=c++20 "-$opt" -g -fPIC -pthread \
      -fno-exceptions -fno-rtti -fstack-protector-strong \
      "${RUNTIME_INCLUDES[@]}" -c "$source" \
      -o "$directory/hosted_$(basename "$source" .cpp).o"
  done
  rm -f "$directory/libhosted_runtime.a"
  run_limited 60 "$AR" rcD "$directory/libhosted_runtime.a" \
    "$directory"/*.o
}

run_native_host() {
  local opt="$1"
  local directory="$OUT/native-host-O$opt"
  build_native_archive "$directory/runtime" "$opt"
  mapfile -t flags < <(host_flags "$opt")
  run_limited 180 "$CXX" "${flags[@]}" "${RUNTIME_INCLUDES[@]}" \
    -c "$TEST" -o "$directory/test.o"
  run_limited 180 "$CXX" "${flags[@]}" "${RUNTIME_INCLUDES[@]}" \
    -c "$OBSERVER" -o "$directory/observer.o"
  run_limited 180 "$CXX" "${flags[@]}" "${WRAP[@]}" \
    "$directory/test.o" "$directory/observer.o" \
    "$directory/runtime/libnative_runtime.a" \
    -o "$directory/weak_objects_native"
  run_limited 120 "$directory/weak_objects_native"
}

run_hosted_host() {
  local opt="$1"
  local directory="$OUT/hosted-host-O$opt"
  build_hosted_archive "$directory/runtime" "$opt"
  mapfile -t flags < <(host_flags "$opt")
  run_limited 180 "$CXX" "${flags[@]}" "${RUNTIME_INCLUDES[@]}" \
    -c "$TEST" -o "$directory/test.o"
  run_limited 180 "$CXX" "${flags[@]}" "${RUNTIME_INCLUDES[@]}" \
    -c "$OBSERVER" -o "$directory/observer.o"
  run_limited 180 "$CXX" "${flags[@]}" "${WRAP[@]}" \
    -Wl,-u,lang_hosted_startup_v1 \
    "$directory/test.o" "$directory/observer.o" \
    "$directory/runtime/libhosted_runtime.a" \
    -o "$directory/weak_objects_hosted"
  run_limited 120 "$directory/weak_objects_hosted"
}

run_fork_failure() {
  local mode="$1"
  local opt="$2"
  local directory="$OUT/fork-failure-$mode-O$opt"
  if [[ "$mode" == native ]]; then
    build_native_archive "$directory/runtime" "$opt"
    archive="$directory/runtime/libnative_runtime.a"
    extra=()
  else
    build_hosted_archive "$directory/runtime" "$opt"
    archive="$directory/runtime/libhosted_runtime.a"
    extra=(-Wl,-u,lang_hosted_startup_v1)
  fi
  mapfile -t flags < <(host_flags "$opt")
  run_limited 180 "$CXX" "${flags[@]}" "${RUNTIME_INCLUDES[@]}" \
    -c "$FAILURE_TEST" -o "$directory/test.o"
  run_limited 180 "$CXX" "${flags[@]}" "${RUNTIME_INCLUDES[@]}" \
    -c "$OBSERVER" -o "$directory/observer.o"
  run_limited 180 "$CXX" "${flags[@]}" "${WRAP[@]}" "${extra[@]}" \
    "$directory/test.o" "$directory/observer.o" "$archive" \
    -o "$directory/weak_objects_fork_failure"
  run_limited 120 "$directory/weak_objects_fork_failure"
}

run_sanitize() {
  local directory="$OUT/sanitize"
  mkdir -p "$directory"
  local common=(
    -std=c++20 -O1 -g -ffreestanding -fno-builtin -fno-exceptions -fno-rtti
    -fno-builtin-memcpy -fno-builtin-memset -fno-builtin-memmove
    -fno-stack-protector -fno-omit-frame-pointer
    -fsanitize=address,undefined -fno-sanitize-recover=all
  )
  local host_sources=(weak panic sync text ipc)
  for name in "${host_sources[@]}"; do
    run_limited 180 "$CXX" "${common[@]}" "${RUNTIME_INCLUDES[@]}" \
      -c "$ROOT/runtime/native/$name.cpp" -o "$directory/$name.o"
  done
  run_limited 180 "$CXX" "${common[@]}" "${RUNTIME_INCLUDES[@]}" \
    -DLANG_RUNTIME_HOSTED -c "$ROOT/runtime/native/process.cpp" \
    -o "$directory/process.o"
  local hosted=(
    "$ROOT/runtime/hosted/start.cpp"
    "$ROOT/runtime/hosted/thread.cpp"
    "$ROOT/runtime/hosted/process.cpp")
  for source in "${hosted[@]}"; do
    run_limited 180 "$CXX" -std=c++20 -O1 -g -pthread -fno-exceptions \
      -fno-rtti -fno-omit-frame-pointer -fsanitize=address,undefined \
      -fno-sanitize-recover=all "${RUNTIME_INCLUDES[@]}" \
      -c "$source" -o "$directory/hosted_$(basename "$source" .cpp).o"
  done
  run_limited 180 "$CXX" -std=c++20 -O1 -g -pthread \
    -fno-omit-frame-pointer -fsanitize=address,undefined \
    -fno-sanitize-recover=all "${RUNTIME_INCLUDES[@]}" \
    -c "$ROOT/tests/native/cycle_gc/sanitizer_alloc.cpp" \
    -o "$directory/sanitizer_alloc.o"
  run_limited 180 "$CXX" -std=c++20 -O1 -g -pthread \
    -fno-omit-frame-pointer -fsanitize=address,undefined \
    -fno-sanitize-recover=all "${RUNTIME_INCLUDES[@]}" \
    -c "$HERE/weak_objects_sanitizer_alloc_hooks.cpp" \
    -o "$directory/sanitizer_alloc_hooks.o"
  run_limited 180 "$CXX" -std=c++20 -O1 -g -pthread \
    -fno-omit-frame-pointer -fsanitize=address,undefined \
    -fno-sanitize-recover=all "${RUNTIME_INCLUDES[@]}" \
    -c "$TEST" -o "$directory/test.o"
  run_limited 180 "$CXX" -std=c++20 -O1 -g -pthread \
    -fno-omit-frame-pointer -fsanitize=address,undefined \
    -fno-sanitize-recover=all "${RUNTIME_INCLUDES[@]}" \
    -c "$OBSERVER" -o "$directory/observer.o"
  run_limited 180 "$CXX" -std=c++20 -O1 -g -pthread \
    -fno-omit-frame-pointer -fsanitize=address,undefined \
    -fno-sanitize-recover=all "${WRAP[@]}" \
    -Wl,-u,lang_hosted_startup_v1 \
    "$directory/test.o" "$directory/observer.o" "$directory/weak.o" \
    "$directory/panic.o" "$directory/sync.o" "$directory/text.o" \
    "$directory/ipc.o" "$directory/process.o" \
    "$directory/hosted_start.o" "$directory/hosted_thread.o" \
    "$directory/hosted_process.o" \
    "$directory/sanitizer_alloc.o" "$directory/sanitizer_alloc_hooks.o" \
    -o "$directory/weak_objects_sanitize"
  ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
  UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    run_limited 180 "$directory/weak_objects_sanitize"
}

run_static() {
  local opt="$1"
  local directory="$OUT/static-O$opt"
  mkdir -p "$directory"
  local flags=(
    -std=c++20 "-$opt" -g -fno-pie -ffreestanding -fno-builtin
    -fno-exceptions -fno-rtti -fno-sanitize=all -fno-builtin-memcpy
    -fno-builtin-memset -fno-builtin-memmove -fno-stack-protector
    -fno-asynchronous-unwind-tables -fno-unwind-tables)
  local sources=(alloc weak panic thread sync text ipc process freestanding start)
  for name in "${sources[@]}"; do
    run_limited 120 "$CXX" "${flags[@]}" "${RUNTIME_INCLUDES[@]}" \
      -c "$ROOT/runtime/native/$name.cpp" -o "$directory/$name.o"
  done
  run_limited 120 "$CXX" "${flags[@]}" "${RUNTIME_INCLUDES[@]}" \
    -DLANG_WEAK_OBJECTS_FREESTANDING -c "$TEST" -o "$directory/test.o"
  run_limited 120 "$CXX" "${flags[@]}" "${RUNTIME_INCLUDES[@]}" \
    -c "$OBSERVER" -o "$directory/observer.o"
  run_limited 120 "$CXX" "${WRAP[@]}" -nostdlib -static \
    -Wl,--no-pie -Wl,-e,_start -Wl,-z,noexecstack -Wl,--build-id=none \
    "$directory/test.o" "$directory/observer.o" "$directory/alloc.o" \
    "$directory/weak.o" "$directory/panic.o" "$directory/thread.o" \
    "$directory/sync.o" "$directory/text.o" "$directory/ipc.o" \
    "$directory/process.o" "$directory/freestanding.o" "$directory/start.o" \
    -o "$directory/weak_objects_static"
  local undefined
  undefined="$("$NM" -u "$directory/weak_objects_static")"
  printf 'static undefined symbols:\n%s\n' "${undefined:-<none>}"
  if grep -qE '[[:space:]]U ' <<<"${undefined}"; then
    echo "static ELF has strong undefined symbols" >&2
    return 1
  fi
  if "$READELF" -d "$directory/weak_objects_static" 2>&1 | grep -q NEEDED; then
    echo "static ELF has NEEDED" >&2
    return 1
  fi
  if "$READELF" -l "$directory/weak_objects_static" 2>&1 | grep -q INTERP; then
    echo "static ELF has INTERP" >&2
    return 1
  fi
  if ! "$READELF" -h "$directory/weak_objects_static" | grep -q 'Type:.*EXEC'; then
    echo "static ELF is not EXEC" >&2
    return 1
  fi
  run_limited 120 "$directory/weak_objects_static"
}

printf 'Weak real-object runtime verification\n'
printf 'source: %s\n' "$ROOT"
printf 'compiler: %s\n' "$VERSION"
printf 'build: %s\n\n' "$OUT"

run_native_host O0
run_native_host O2
run_hosted_host O0
run_hosted_host O2
run_fork_failure native O2
run_fork_failure hosted O2
run_sanitize
run_static O0
run_static O2

printf '\nall Weak real-object checks passed\n'
