#!/usr/bin/env bash
# Targeted core/hosted checks for the private per-thread tid cache.
#
# Core: a real -nostdlib/-static ELF linked with start.cpp, thread.cpp and the
# GC runtime checks main/Native TCBs and fork-child cache refresh.
# Hosted: links gc_world.cpp without start.cpp and checks that the libc FS
# words are not modified by the raw-getttid fallback.
set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
TOOLCHAINS=${LANG_TOOLCHAINS:-$ROOT/../toolchains}
LLVM_ROOT=${LANG_LLVM_ROOT:-$TOOLCHAINS/llvm-22.1.8}
COMPILER=${CXX:-$LLVM_ROOT/bin/clang++}
BUILD_DIR=${TID_CACHE_BUILD_DIR:-$ROOT/.work/gc_tid_cache-build}

PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); printf 'PASS  %s\n' "$1"; }
fail() { FAIL=$((FAIL + 1)); printf 'FAIL  %s\n' "$1"; }

show_saved() {
  [ -f "$1" ] && sed -e 's/^/    /' "$1" | tail -n 40
}

# Save the tool output first, then inspect only a successful command. A failed
# readelf/nm is a test failure, never evidence of "nothing found".
check_core_no_needed() {
  local elf=$1 out=$2
  if ! readelf -d "$elf" >"$out" 2>&1; then
    fail "core ELF readelf -d failed"
    show_saved "$out"
    return 1
  fi
  if grep -q NEEDED "$out"; then
    fail "core ELF has dynamic NEEDED entries"
    show_saved "$out"
    return 1
  fi
  pass "core ELF has no dynamic dependencies"
}

check_core_no_interp() {
  local elf=$1 out=$2
  if ! readelf -l "$elf" >"$out" 2>&1; then
    fail "core ELF readelf -l failed"
    show_saved "$out"
    return 1
  fi
  if grep -q INTERP "$out"; then
    fail "core ELF has an interpreter"
    show_saved "$out"
    return 1
  fi
  pass "core ELF has no interpreter"
}

check_core_no_strong_undefined() {
  local elf=$1 out=$2
  if ! nm -u "$elf" >"$out" 2>&1; then
    fail "core ELF nm -u failed"
    show_saved "$out"
    return 1
  fi
  if grep -Eq '(^|[[:space:]])U[[:space:]]' "$out"; then
    fail "core ELF has strong undefined symbols"
    show_saved "$out"
    return 1
  fi
  pass "core ELF has no strong undefined symbols"
}

check_hosted_weak_hooks() {
  local object=$1 out=$2
  if ! nm -u "$object" >"$out" 2>&1; then
    fail "hosted gc_world nm -u failed"
    show_saved "$out"
    return 1
  fi
  local missing=""
  if ! grep -Eq '^[[:space:]]*w[[:space:]]+lang_native_cached_tid$' "$out"; then
    missing="lang_native_cached_tid"
  fi
  if ! grep -Eq '^[[:space:]]*w[[:space:]]+lang_native_clear_tid_cache$' "$out"; then
    missing="${missing:+$missing }lang_native_clear_tid_cache"
  fi
  if [ -n "$missing" ]; then
    fail "hosted gc_world missing weak hook(s): $missing"
    show_saved "$out"
    return 1
  fi
  pass "hosted gc_world keeps both tid-cache hooks weak"
}

# Pure check-helper tests source this file and stop before tool/compiler setup.
if [ "${TID_CACHE_LIB_ONLY:-0}" = "1" ] &&
    [ "${BASH_SOURCE[0]}" != "$0" ]; then
  return 0
fi

for tool in timeout file readelf nm sha256sum; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "FAIL: required tool not found: $tool" >&2
    exit 1
  fi
done
if [ ! -x "$COMPILER" ]; then
  echo "FAIL: compiler not executable: $COMPILER" >&2
  exit 1
fi
mkdir -p "$BUILD_DIR"

FREESTANDING=(-std=c++20 -O2 -ffreestanding -fno-builtin -fno-exceptions
  -fno-rtti -fno-threadsafe-statics -fno-stack-protector -fno-pic -fno-pie
  -Wall -Wextra -Werror -pedantic-errors -I"$ROOT/runtime/native")
HOSTED=(-std=c++20 -O2 -g -pthread -Wall -Wextra -Werror
  -I"$ROOT/runtime/native")

build() {
  local name=$1
  shift
  local log=$BUILD_DIR/${name// /_}.log
  if timeout 240s "$@" >"$log" 2>&1; then
    pass "build: $name"
    return 0
  fi
  fail "build: $name"
  sed -e 's/^/    /' "$log" | tail -n 60
  return 1
}

run() {
  local name=$1 expected=$2 marker=$3
  shift 3
  local log=$BUILD_DIR/${name// /_}.log
  timeout --signal=TERM --kill-after=5s 30s "$@" >"$log" 2>&1
  local status=$?
  if [ "$status" -ne "$expected" ]; then
    fail "$name (exit $status, expected $expected)"
    sed -e 's/^/    /' "$log" | tail -n 40
    return 1
  fi
  if [ -n "$marker" ] && ! grep -q "$marker" "$log"; then
    fail "$name (missing $marker)"
    sed -e 's/^/    /' "$log" | tail -n 40
    return 1
  fi
  pass "$name"
  return 0
}

CORE_OBJECTS_OK=1
for source in start gc_program gc_runtime gc_world gc weak panic alloc freestanding thread; do
  build "core object $source" "$COMPILER" "${FREESTANDING[@]}" \
    -c "$ROOT/runtime/native/$source.cpp" -o "$BUILD_DIR/$source.o" \
    || CORE_OBJECTS_OK=0
done
build "core test object" "$COMPILER" "${FREESTANDING[@]}" \
  -c "$SCRIPT_DIR/core_static_test.cpp" -o "$BUILD_DIR/core_test.o" \
  || CORE_OBJECTS_OK=0

if [ "$CORE_OBJECTS_OK" = "1" ]; then
  if build "core static ELF link" "$COMPILER" -nostdlib -static \
      -Wl,-e,_start -Wl,-z,noexecstack \
      "$BUILD_DIR/start.o" "$BUILD_DIR/gc_program.o" \
      "$BUILD_DIR/gc_runtime.o" "$BUILD_DIR/gc_world.o" "$BUILD_DIR/gc.o" \
      "$BUILD_DIR/weak.o" "$BUILD_DIR/panic.o" "$BUILD_DIR/alloc.o" \
      "$BUILD_DIR/freestanding.o" "$BUILD_DIR/thread.o" \
      "$BUILD_DIR/core_test.o" -o "$BUILD_DIR/core_static"; then
    ELF=$BUILD_DIR/core_static
    if file "$ELF" | grep -q 'statically linked'; then
      pass "core ELF is statically linked"
    else
      fail "core ELF is not statically linked"
    fi
    check_core_no_needed "$ELF" "$BUILD_DIR/core_readelf_dynamic.txt"
    check_core_no_interp "$ELF" "$BUILD_DIR/core_readelf_program_headers.txt"
    check_core_no_strong_undefined "$ELF" "$BUILD_DIR/core_nm_undefined.txt"
    if grep -a -q 'libstdc++\|libc\.so\|libgcc_s' "$ELF"; then
      fail "core ELF references libc/libstdc++ strings"
    else
      pass "core ELF has no libc/libstdc++ dependency strings"
    fi
    run "core static execution" 0 GC_TID_CACHE_STATIC_OK "$ELF"
  fi
fi

if build "hosted object gc_world" "$COMPILER" "${HOSTED[@]}" \
    -c "$ROOT/runtime/native/gc_world.cpp" -o "$BUILD_DIR/hosted_gc_world.o" &&
   build "hosted object alloc" "$COMPILER" "${HOSTED[@]}" \
    -c "$ROOT/runtime/native/alloc.cpp" -o "$BUILD_DIR/hosted_alloc.o" &&
   build "hosted test binary" "$COMPILER" "${HOSTED[@]}" \
    "$SCRIPT_DIR/hosted_tls_test.cpp" "$BUILD_DIR/hosted_gc_world.o" \
    "$BUILD_DIR/hosted_alloc.o" -o "$BUILD_DIR/hosted_tls"; then
  check_hosted_weak_hooks "$BUILD_DIR/hosted_gc_world.o" \
    "$BUILD_DIR/hosted_nm_undefined.txt"
  run "hosted TLS fallback execution" 0 GC_TID_CACHE_HOSTED_OK \
    "$BUILD_DIR/hosted_tls"
fi

echo
echo "source hashes:"
sha256sum "$ROOT/runtime/native/start.cpp" "$ROOT/runtime/native/start.hpp" \
  "$ROOT/runtime/native/thread.cpp" "$ROOT/runtime/native/gc_world.cpp" \
  "$SCRIPT_DIR/core_static_test.cpp" "$SCRIPT_DIR/hosted_tls_test.cpp"
echo "SUMMARY pass=$PASS fail=$FAIL"
[ "$FAIL" -eq 0 ]
