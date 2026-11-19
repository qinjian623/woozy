#!/usr/bin/env bash
# Focused lifetime tests for runtime/native/async.cpp without changing CMake.
#
#   tests/async_runtime/run_tests.sh
#
# Outputs and logs go to $ASYNC_RUNTIME_OUT (default: tests/async_runtime/.build).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
out="${ASYNC_RUNTIME_OUT:-$here/.build}"
toolchains="${LANG_TOOLCHAINS:-$root/../toolchains}"
llvm_root="${LANG_LLVM_ROOT:-$toolchains/llvm-22.1.8}"
libxml2_root="${LANG_LIBXML2_ROOT:-$toolchains/libxml2-legacy-2.13.9}"
cxx="${CXX:-$llvm_root/bin/clang++}"
nm_bin="${NM:-$llvm_root/bin/llvm-nm}"
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

test_source="$here/async_runtime_lifetime_test.cpp"
runtime_source="$root/runtime/native/async.cpp"

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

# Hosted builds of the executor unit under the test harness. The sanitizer
# build is the primary evidence: every scenario is checked for use-after-free,
# double free, registry residue and leaked futures.
build_host() {
  local name="$1"
  shift
  local binary="$out/async_runtime_lifetime_$name"
  "$cxx" -std=c++20 -g -fno-exceptions -fno-rtti \
    -Wall -Wextra -Wno-unused-const-variable "$@" \
    "$test_source" -o "$binary"
  printf '%s\n' "$binary"
}

asan_binary="$(build_host sanitize -O1 -fno-omit-frame-pointer \
  -fsanitize=address,undefined)"
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  run_logged sanitize "$asan_binary"

run_logged O0 "$(build_host O0 -O0)"
run_logged O2 "$(build_host O2 -O2)"

# The two independent review counterexamples, built exactly like the external
# ASan harness (a hosted sanitizer binary that includes the executor unit).
review_binary="$out/async_review_counterexamples"
"$cxx" -std=c++20 -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined \
  -fno-exceptions -fno-rtti "$here/review_counterexamples.cpp" -o "$review_binary"
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  run_logged review-registry "$review_binary"
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  run_logged review-cancel-waiter "$review_binary" second

# Freestanding build with the exact flags of lang_native_runtime. The executor
# must stay self-contained: only the three documented runtime entry points may
# remain undefined, so including gc_program.hpp must not add link dependencies.
freestanding_object="$out/async.freestanding.o"
"$cxx" -std=c++20 -O2 -ffreestanding -fno-builtin -fno-exceptions -fno-rtti \
  -fno-sanitize=all -fno-builtin-memcpy -fno-builtin-memset \
  -fno-builtin-memmove -c "$runtime_source" -o "$freestanding_object"
undefined="$("$nm_bin" -u "$freestanding_object" | awk '{print $NF}' | sort -u)"
expected=$'lang_alloc_v1\nlang_free_v1\nlang_native_panic_v1'
if [[ "$undefined" != "$expected" ]]; then
  echo "unexpected undefined symbols in freestanding async.o:" >&2
  printf '%s\n' "$undefined" >&2
  exit 1
fi
echo "== freestanding =="
echo "async.cpp freestanding object clean (only lang_alloc_v1/lang_free_v1/lang_native_panic_v1 undefined)"

echo "All async runtime lifetime tests passed. Logs: $out"
