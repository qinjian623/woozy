#!/usr/bin/env bash
# GC + Weak integration verification with the fixed LLVM 22.1.8 toolchain.
#
# Covers the white-cycle Weak invalidation ordering, root-reachable Weak
# preservation, closed-slot final release, and unsafe clear resurrection at
# host O0/O2, ASAN/UBSAN, and -nostdlib static O0/O2.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${CYCLE_GC_WEAK_OUT:-$here/.build_weak}"
toolchains="${LANG_TOOLCHAINS:-$root/../toolchains}"
llvm_root="${LANG_LLVM_ROOT:-$toolchains/llvm-22.1.8}"
libxml2_root="${LANG_LIBXML2_ROOT:-$toolchains/libxml2-legacy-2.13.9}"
cxx="${CXX:-$llvm_root/bin/clang++}"
nm_bin="${NM:-$llvm_root/bin/llvm-nm}"
readelf_bin="${READELF:-$llvm_root/bin/llvm-readelf}"
mkdir -p "$out"

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
if [[ ! -f "$libxml2_root/usr/lib/libxml2.so.2.13.9" ]]; then
  echo "missing legacy libxml2 under $libxml2_root" >&2
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

WRAP=(-fuse-ld=lld -Wl,--wrap=lang_alloc_v1 -Wl,--wrap=lang_free_v1)
INCLUDES=(-I "$root/runtime/native")
TEST="$here/gc_weak_integration_test.cpp"

build_host() {
  local opt="$1"
  local runtime_flags=(
    -std=c++20 "-$opt" -g
    -ffreestanding -fno-builtin -fno-exceptions -fno-rtti
    -fno-sanitize=all -fno-builtin-memcpy -fno-builtin-memset
    -fno-builtin-memmove -fno-stack-protector
  )
  local host_flags=(
    -std=c++20 "-$opt" -g -pthread -Wall -Wextra -Wpedantic
  )
  run_limited 120 "$cxx" "${host_flags[@]}" "${INCLUDES[@]}" \
    -c "$TEST" -o "$out/integration_${opt}.o"
  run_limited 120 "$cxx" "${runtime_flags[@]}" "${INCLUDES[@]}" \
    -c "$root/runtime/native/gc.cpp" -o "$out/gc_${opt}.o"
  run_limited 120 "$cxx" "${runtime_flags[@]}" "${INCLUDES[@]}" \
    -c "$root/runtime/native/weak.cpp" -o "$out/weak_${opt}.o"
  run_limited 120 "$cxx" "${runtime_flags[@]}" "${INCLUDES[@]}" \
    -c "$root/runtime/native/alloc.cpp" -o "$out/alloc_${opt}.o"
  run_limited 120 "$cxx" "${runtime_flags[@]}" "${INCLUDES[@]}" \
    -c "$root/runtime/native/panic.cpp" -o "$out/panic_${opt}.o"
  run_limited 120 "$cxx" "${WRAP[@]}" "${host_flags[@]}" \
    "$out/integration_${opt}.o" "$out/gc_${opt}.o" "$out/weak_${opt}.o" \
    "$out/alloc_${opt}.o" "$out/panic_${opt}.o" \
    -o "$out/gc_weak_host_${opt}"
  run_limited 30 "$out/gc_weak_host_${opt}"
}

build_sanitize() {
  local flags=(
    -std=c++20 -O1 -g -pthread -Wall -Wextra -Wpedantic
    -ffreestanding -fno-builtin -fno-exceptions -fno-rtti
    -fno-builtin-memcpy -fno-builtin-memset -fno-builtin-memmove
    -fno-stack-protector -fno-omit-frame-pointer
    -fsanitize=address,undefined -fno-sanitize-recover=all
  )
  run_limited 180 "$cxx" -std=c++20 -O1 -g -pthread \
    -Wall -Wextra -Wpedantic -fno-omit-frame-pointer \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    "${INCLUDES[@]}" -c "$TEST" -o "$out/integration_sanitize.o"
  run_limited 180 "$cxx" "${flags[@]}" "${INCLUDES[@]}" \
    -c "$root/runtime/native/gc.cpp" -o "$out/gc_sanitize.o"
  run_limited 180 "$cxx" "${flags[@]}" "${INCLUDES[@]}" \
    -c "$root/runtime/native/weak.cpp" -o "$out/weak_sanitize.o"
  run_limited 180 "$cxx" "${flags[@]}" "${INCLUDES[@]}" \
    -c "$root/runtime/native/panic.cpp" -o "$out/panic_sanitize.o"
  run_limited 180 "$cxx" -std=c++20 -O1 -g -pthread \
    -fno-omit-frame-pointer -fsanitize=address,undefined \
    -fno-sanitize-recover=all "${INCLUDES[@]}" \
    -c "$here/sanitizer_alloc.cpp" -o "$out/sanitizer_alloc.o"
  run_limited 180 "$cxx" "${WRAP[@]}" "${flags[@]}" \
    "$out/integration_sanitize.o" "$out/gc_sanitize.o" \
    "$out/weak_sanitize.o" "$out/panic_sanitize.o" \
    "$out/sanitizer_alloc.o" -o "$out/gc_weak_sanitize"
  ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
  UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    run_limited 60 "$out/gc_weak_sanitize"
}

check_static() {
  local binary="$1"
  local undefined
  undefined="$("$nm_bin" -u "$binary")"
  printf 'static undefined symbols:\n%s\n' "${undefined:-<none>}"
  if grep -qE '[[:space:]]U ' <<<"${undefined}"; then
    echo "static integration ELF has strong undefined symbols" >&2
    return 1
  fi
  if "$readelf_bin" -d "$binary" 2>&1 | grep -q NEEDED; then
    echo "static integration ELF has NEEDED" >&2
    return 1
  fi
  if "$readelf_bin" -l "$binary" 2>&1 | grep -q INTERP; then
    echo "static integration ELF has INTERP" >&2
    return 1
  fi
  if ! "$readelf_bin" -h "$binary" | grep -q 'Type:.*EXEC'; then
    echo "static integration ELF is not EXEC" >&2
    return 1
  fi
}

build_static() {
  local opt="$1"
  local flags=(
    -std=c++20 "-$opt" -g -fno-pie
    -ffreestanding -fno-builtin -fno-exceptions -fno-rtti
    -fno-sanitize=all -fno-builtin-memcpy -fno-builtin-memset
    -fno-builtin-memmove -fno-stack-protector
    -fno-asynchronous-unwind-tables -fno-unwind-tables
  )
  run_limited 120 "$cxx" "${flags[@]}" "${INCLUDES[@]}" \
    -DLANG_GC_FREESTANDING -c "$TEST" -o "$out/integration_static_${opt}.o"
  for source in gc weak alloc panic freestanding start; do
    run_limited 120 "$cxx" "${flags[@]}" "${INCLUDES[@]}" \
      -c "$root/runtime/native/${source}.cpp" \
      -o "$out/${source}_static_${opt}.o"
  done
  run_limited 120 "$cxx" "${WRAP[@]}" -nostdlib -static \
    -Wl,--no-pie -Wl,-e,_start -Wl,-z,noexecstack -Wl,--build-id=none \
    "$out/integration_static_${opt}.o" "$out/gc_static_${opt}.o" \
    "$out/weak_static_${opt}.o" "$out/alloc_static_${opt}.o" \
    "$out/panic_static_${opt}.o" "$out/freestanding_static_${opt}.o" \
    "$out/start_static_${opt}.o" -o "$out/gc_weak_static_${opt}"
  check_static "$out/gc_weak_static_${opt}"
  run_limited 30 "$out/gc_weak_static_${opt}"
}

printf 'GC + Weak fixed-toolchain verification\n'
printf 'source: %s\n' "$root"
printf 'compiler: %s\n' "$version"
printf 'legacy libxml2: %s\n' "$libxml2_root"
printf 'build: %s\n\n' "$out"

build_host O0
build_host O2
build_sanitize
build_static O0
build_static O2

printf '\nall GC+Weak integration checks passed\n'
