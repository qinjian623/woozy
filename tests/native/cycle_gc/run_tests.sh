#!/usr/bin/env bash
# Build/run the standalone cycle collector tests without changing CMake.
#
#   tests/native/cycle_gc/run_tests.sh
#
# Outputs and logs go to $CYCLE_GC_OUT (default: tests/native/cycle_gc/.build).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${CYCLE_GC_OUT:-$here/.build}"
llvm_root="${LANG_LLVM_ROOT:-/home/qin/Work/language-dev-20261103/toolchains/llvm-22.1.8}"
libxml2_root="${LANG_LIBXML2_ROOT:-/home/qin/Work/language-dev-20261103/toolchains/libxml2-legacy-2.13.9}"
cxx="${CXX:-$llvm_root/bin/clang++}"
nm_bin="${NM:-$llvm_root/bin/llvm-nm}"
readelf_bin="${READELF:-$llvm_root/bin/llvm-readelf}"
mkdir -p "$out"

# The fixed LLVM 22.1.8 lld needs the legacy libxml2 runtime.
export PATH="$llvm_root/bin:$PATH"
export LD_LIBRARY_PATH="$libxml2_root/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

if [[ ! -x "$cxx" ]]; then
  echo "missing fixed LLVM 22.1.8 clang++: $cxx" >&2
  exit 2
fi
version="$("$cxx" --version | head -n 1)"
if [[ "$version" != *"clang version 22.1.8"* ]]; then
  echo "expected fixed LLVM 22.1.8, got: $version" >&2
  exit 2
fi

wrap=(-fuse-ld=lld -Wl,--wrap=lang_alloc_v1 -Wl,--wrap=lang_free_v1)
runtime=("$root/runtime/native/gc.cpp" "$root/runtime/native/weak.cpp" "$root/runtime/native/panic.cpp")
host_test="$here/cycle_gc_test.cpp"
static_test="$here/cycle_gc_test.cpp"

run_logged() {
  local name="$1"
  shift
  local log="$out/$name.log"
  echo "== $name =="
  if ! "$@" >"$log" 2>&1; then
    echo "FAILED: $name"
    cat "$log"
    exit 1
  fi
  cat "$log"
}

build_host() {
  local opt="$1"
  local binary="$out/cycle_gc_host_${opt}"
  "$cxx" -std=c++20 "-$opt" -g -fno-exceptions -fno-rtti \
    -fno-builtin "$host_test" \
    "${runtime[@]}" "$root/runtime/native/alloc.cpp" \
    "${wrap[@]}" -pthread -o "$binary"
  printf '%s\n' "$binary"
}

build_static() {
  local opt="$1"
  local binary="$out/cycle_gc_static_${opt}"
  "$cxx" -std=c++20 "-$opt" -g -fno-exceptions -fno-rtti \
    -ffreestanding -fno-builtin -fno-sanitize=all -fno-stack-protector \
    -fno-pic -fno-pie -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -DLANG_GC_FREESTANDING "$static_test" \
    "${runtime[@]}" "$root/runtime/native/alloc.cpp" \
    "$root/runtime/native/freestanding.cpp" "$root/runtime/native/start.cpp" \
    "${wrap[@]}" -nostdlib -static -Wl,--no-pie \
    -Wl,--build-id=none -o "$binary"
  printf '%s\n' "$binary"
}

check_static_elf() {
  local binary="$1"
  if "$readelf_bin" -l "$binary" | grep -q 'INTERP'; then
    echo "static ELF unexpectedly has INTERP: $binary" >&2
    return 1
  fi
  if "$readelf_bin" -d "$binary" 2>/dev/null | grep -q 'NEEDED'; then
    echo "static ELF unexpectedly has NEEDED: $binary" >&2
    return 1
  fi
  if "$nm_bin" -u "$binary" | grep -Eq '^[[:space:]]*U[[:space:]]'; then
    echo "static ELF has strong undefined symbols: $binary" >&2
    "$nm_bin" -u "$binary" >&2
    return 1
  fi
  if ! "$readelf_bin" -h "$binary" | grep -q 'Type:.*EXEC'; then
    echo "static ELF is not a fixed EXEC image: $binary" >&2
    return 1
  fi
  echo "static ELF check passed: $binary"
}

run_logged host-O0 "$(build_host O0)"
run_logged host-O2 "$(build_host O2)"

static_O0="$(build_static O0)"
check_static_elf "$static_O0"
run_logged static-O0 "$static_O0"

static_O2="$(build_static O2)"
check_static_elf "$static_O2"
run_logged static-O2 "$static_O2"

# ASAN/UBSAN use a malloc-backed allocator so poisoned memory is visible to
# the sanitizers.  The real allocator is exercised by the host/static builds.
san_binary="$out/cycle_gc_sanitize"
"$cxx" -std=c++20 -O1 -g -fno-omit-frame-pointer \
  -fsanitize=address,undefined -fno-exceptions -fno-rtti \
  "$host_test" "$here/sanitizer_alloc.cpp" "${runtime[@]}" \
  "${wrap[@]}" -pthread -o "$san_binary"
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  run_logged sanitize "$san_binary"

echo "All cycle_gc test variants passed. Logs: $out"
