#!/usr/bin/env bash
# Build the hosted runtime archive with the capturing-registration unit and run
# the private C callback runtime harnesses.
#
# This script is self-contained: it uses the published hosted manifest flags but
# does not modify the root CMake projects, the shared headers or any other
# worktree. Defaults to a temporary build directory; override with
# C_CALLBACK_BUILD_DIR to keep the artifacts.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
CLANG_DIR="${CLANG_DIR:-/home/qin/Work/language-dev-20261103/toolchains/llvm-22.1.8/bin}"
CXX="${CXX:-$CLANG_DIR/clang++}"
CC="${CC:-$CLANG_DIR/clang}"
AR="${AR:-$CLANG_DIR/llvm-ar}"
# Canonical hosted library path; ld.lld needs the legacy libxml2 package.
LIBXML_LEGACY="${LIBXML_LEGACY:-/home/qin/Work/language-dev-20261103/toolchains/libxml2-legacy-2.13.9/usr/lib}"
LLVM_LIB="${LLVM_LIB:-/home/qin/Work/language-dev-20261103/toolchains/llvm-22.1.8/lib}"
export LD_LIBRARY_PATH="$LLVM_LIB:$LIBXML_LEGACY${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

if [[ -n "${C_CALLBACK_BUILD_DIR:-}" ]]; then
  BUILD="$C_CALLBACK_BUILD_DIR"
  rm -rf "$BUILD"
else
  BUILD="$(mktemp -d "${TMPDIR:-/tmp}/woozy-c-callback.XXXXXX")"
fi
mkdir -p "$BUILD/obj" "$BUILD/log"
COMPILE_TIMEOUT="${COMPILE_TIMEOUT:-180}"
RUN_TIMEOUT="${RUN_TIMEOUT:-120}"

log() { printf '%s\n' "$*" >&2; }

PUBLIC_SOURCES=(
  alloc panic weak gc gc_world gc_runtime gc_program async os os_buffers sync
  text system system_buffers ipc dns network_buffers network numbers slice
  unicode/unicode
)
CXX_PUBLIC=(
  "$CXX" -std=c++20 -O2 -fPIC -ffreestanding -fno-builtin -fno-exceptions
  -fno-rtti -fno-sanitize=all -fno-builtin-memcpy -fno-builtin-memset
  -fno-builtin-memmove -I"$ROOT/runtime/native"
)
CXX_HOSTED=(
  "$CXX" -std=c++20 -O2 -fPIC -pthread -fstack-protector-strong -fno-rtti
  -fno-exceptions -I"$ROOT/runtime/hosted" -I"$ROOT/runtime/native"
)
C_MATH=(
  "$CC" -std=c99 -O2 -fPIC -ffreestanding -fno-builtin -fno-common
  -fno-strict-aliasing -fno-math-errno -fno-trapping-math -fno-fast-math
  -ffp-contract=off -Wno-parentheses -Wno-shift-op-parentheses
  -Wno-unknown-pragmas -I"$ROOT/runtime/native/math"
  -include "$ROOT/runtime/native/math/math_prefix.h"
)

compile_public() {
  local source="$1" object="$2"
  shift 2
  timeout "$COMPILE_TIMEOUT" "${CXX_PUBLIC[@]}" "$@" -c "$source" -o "$object"
}
compile_hosted() {
  local source="$1" object="$2"
  timeout "$COMPILE_TIMEOUT" "${CXX_HOSTED[@]}" -c "$source" -o "$object"
}
compile_math_c() {
  local source="$1" object="$2"
  timeout "$COMPILE_TIMEOUT" "${C_MATH[@]}" -c "$source" -o "$object"
}

log "toolchain: $("$CXX" --version | head -n1)"
log "build: $BUILD"

for name in "${PUBLIC_SOURCES[@]}"; do
  safe="${name//\//_}"
  compile_public "$ROOT/runtime/native/$name.cpp" "$BUILD/obj/$safe.o"
done
compile_public "$ROOT/runtime/native/math/support.cpp" "$BUILD/obj/math_support.o"
compile_public "$ROOT/runtime/native/process.cpp" "$BUILD/obj/process.o" \
  -DLANG_RUNTIME_HOSTED
# The vendored utf8proc adapter is a normal hosted public unit; the CMake build
# compiles it as C++ too (only LANG_MATH_C_SOURCES use the math C flags).
compile_public "$ROOT/runtime/native/unicode/utf8proc_adapter.c" \
  "$BUILD/obj/utf8proc_adapter.o"
while IFS= read -r source; do
  base="$(basename "$source" .c)"
  compile_math_c "$source" "$BUILD/obj/math_$base.o"
done < <(find "$ROOT/runtime/native/math/vendor" -maxdepth 1 -name '*.c' -print |
  sort)

# Hosted replacement units, including the capturing-registration runtime. The
# root build must add runtime/hosted/c_callback.cpp to
# LANG_HOSTED_REPLACEMENT_SOURCES with exactly these options.
compile_hosted "$ROOT/runtime/hosted/start.cpp" "$BUILD/obj/hosted_start.o"
compile_hosted "$ROOT/runtime/hosted/thread.cpp" "$BUILD/obj/hosted_thread.o"
compile_hosted "$ROOT/runtime/hosted/process.cpp" "$BUILD/obj/hosted_process.o"
compile_hosted "$ROOT/runtime/hosted/c_callback.cpp" "$BUILD/obj/c_callback.o"

rm -f "$BUILD/libhosted_runtime.a"
timeout "$COMPILE_TIMEOUT" "$AR" rcD "$BUILD/libhosted_runtime.a" \
  "$BUILD/obj"/*.o
log "archive: $BUILD/libhosted_runtime.a"

HARNESS=(
  "$CXX" -std=c++20 -O2 -g -pthread -fstack-protector-strong
  -I"$ROOT/runtime/hosted" -I"$ROOT/runtime/native"
)

status=0
run_suite() {
  local name="$1" source="$2"
  shift 2
  local binary="$BUILD/$name"
  timeout "$COMPILE_TIMEOUT" "${HARNESS[@]}" \
    "$ROOT/tests/native/c_callback_runtime/$source" \
    "$BUILD/libhosted_runtime.a" "$@" -o "$binary"
  log "running $name"
  set +e
  timeout "$RUN_TIMEOUT" "$binary" >"$BUILD/log/$name.out" \
    2>"$BUILD/log/$name.err"
  local code=$?
  set -e
  cat "$BUILD/log/$name.out" >&2
  if [[ $code -ne 0 ]]; then
    echo "$name: FAILED (exit $code)" >&2
    cat "$BUILD/log/$name.err" >&2
    status=1
  else
    echo "$name: passed" >&2
  fi
}

# The runtime suite wraps pthread_cond_wait so it can prove the follower closer
# reaches its condition wait while the runtime holds it SAFE.
run_suite c_callback_runtime_test c_callback_runtime_test.cpp \
  -Wl,--wrap=pthread_cond_wait
run_suite c_callback_process_test c_callback_process_test.cpp

if [[ $status -ne 0 ]]; then
  log "FAIL"
  exit 1
fi
log "PASS: C callback runtime suites"
