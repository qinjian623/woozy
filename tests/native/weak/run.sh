#!/usr/bin/env bash
# Isolated MEM01 Weak-runtime verification.
#
# Every compile/link/run below has a finite timeout. The runtime sources are
# compiled separately with the freestanding flags used by lang_native_runtime.
# The production check links the real allocator/panic runtime. A second host
# build uses ASAN/UBSAN as an oracle, and a third is a -nostdlib static ELF with
# no libc/C++ runtime at all.
set -euo pipefail

SOURCE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
LLVM_ROOT="${LANG_LLVM_ROOT:-/home/qin/Work/language-dev-20261103/toolchains/llvm-22.1.8}"
LIBXML2_ROOT="${LANG_LIBXML2_ROOT:-/home/qin/Work/language-dev-20261103/toolchains/libxml2-legacy-2.13.9}"
CXX="${CXX:-${LLVM_ROOT}/bin/clang++}"
NM="${NM:-${LLVM_ROOT}/bin/llvm-nm}"
READELF="${READELF:-${LLVM_ROOT}/bin/llvm-readelf}"
LLD="${LLD:-${LLVM_ROOT}/bin/ld.lld}"
BUILD_DIR="${LANG_WEAK_BUILD_DIR:-$(mktemp -d "${TMPDIR:-/tmp}/lang-weak.XXXXXX")}"

# The fixed LLVM release's lld was built against the legacy libxml2 artifact.
export PATH="${LLVM_ROOT}/bin:${PATH}"
export LD_LIBRARY_PATH="${LIBXML2_ROOT}/usr/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

cleanup() {
  if [[ "${LANG_WEAK_KEEP_BUILD:-0}" != "1" ]]; then
    rm -rf "${BUILD_DIR}"
  else
    printf 'artifacts kept in %s\n' "${BUILD_DIR}"
  fi
}
trap cleanup EXIT

run_limited() {
  local seconds="$1"
  shift
  printf '+ '
  printf '%q ' "$@"
  printf '\n'
  timeout "${seconds}s" "$@"
}

if [[ ! -x "${CXX}" ]]; then
  printf 'missing fixed LLVM 22.1.8 clang++: %s\n' "${CXX}" >&2
  exit 2
fi
VERSION="$("${CXX}" --version | head -n 1)"
if [[ "${VERSION}" != *"clang version 22.1.8"* ]]; then
  printf 'expected fixed LLVM 22.1.8, got: %s\n' "${VERSION}" >&2
  exit 2
fi
if [[ ! -f "${LIBXML2_ROOT}/usr/lib/libxml2.so.2.13.9" ]]; then
  printf 'missing legacy libxml2 artifact under %s\n' "${LIBXML2_ROOT}" >&2
  exit 2
fi
if [[ ! -x "${LLD}" ]]; then
  printf 'missing fixed LLVM 22.1.8 ld.lld: %s\n' "${LLD}" >&2
  exit 2
fi
LLD_VERSION="$("${LLD}" --version | head -n 1)"
if [[ "${LLD_VERSION}" != *"LLD 22.1.8"* ]]; then
  printf 'expected fixed LLD 22.1.8, got: %s\n' "${LLD_VERSION}" >&2
  exit 2
fi

RUNTIME_FLAGS=(
  -std=c++20
  -O2
  -ffreestanding
  -fno-builtin
  -fno-exceptions
  -fno-rtti
  -fno-sanitize=all
  -fno-builtin-memcpy
  -fno-builtin-memset
  -fno-builtin-memmove
  -fno-stack-protector
)
RUNTIME_INCLUDES=(-I "${SOURCE_ROOT}/runtime/native")
HOST_FLAGS=(
  -std=c++20
  -O2
  -g
  -pthread
  -Wall
  -Wextra
  -Wpedantic
  -I "${SOURCE_ROOT}/runtime/native"
)
WRAP_FLAGS=(
  -Wl,--wrap=lang_alloc_v1
  -Wl,--wrap=lang_free_v1
)
LINK_FLAGS=(-fuse-ld=lld)

printf 'Weak isolated verification\n'
printf 'source: %s\n' "${SOURCE_ROOT}"
printf 'compiler: %s\n' "${VERSION}"
printf 'linker: %s\n' "${LLD_VERSION}"
printf 'legacy libxml2 (present for the fixed linker; not linked by this isolated test): %s\n' \
  "${LIBXML2_ROOT}"
printf 'build: %s\n\n' "${BUILD_DIR}"

# ---------------------------------------------------------------------------
# Production-flags host concurrency test.
# ---------------------------------------------------------------------------
run_limited 120 "${CXX}" "${RUNTIME_FLAGS[@]}" "${RUNTIME_INCLUDES[@]}" \
  -c "${SOURCE_ROOT}/runtime/native/weak.cpp" -o "${BUILD_DIR}/weak.o"
run_limited 120 "${CXX}" "${RUNTIME_FLAGS[@]}" "${RUNTIME_INCLUDES[@]}" \
  -c "${SOURCE_ROOT}/runtime/native/alloc.cpp" -o "${BUILD_DIR}/alloc.o"
run_limited 120 "${CXX}" "${RUNTIME_FLAGS[@]}" "${RUNTIME_INCLUDES[@]}" \
  -c "${SOURCE_ROOT}/runtime/native/panic.cpp" -o "${BUILD_DIR}/panic.o"
run_limited 120 "${CXX}" "${HOST_FLAGS[@]}" \
  -c "${SOURCE_ROOT}/tests/native/weak/weak_test.cpp" \
  -o "${BUILD_DIR}/weak_test.o"
run_limited 120 "${CXX}" "${HOST_FLAGS[@]}" \
  -c "${SOURCE_ROOT}/tests/native/weak/weak_alloc_observer.cpp" \
  -o "${BUILD_DIR}/weak_alloc_observer.o"
run_limited 120 "${CXX}" "${LINK_FLAGS[@]}" "${HOST_FLAGS[@]}" \
  "${BUILD_DIR}/weak_test.o" "${BUILD_DIR}/weak.o" "${BUILD_DIR}/alloc.o" \
  "${BUILD_DIR}/panic.o" "${BUILD_DIR}/weak_alloc_observer.o" \
  "${WRAP_FLAGS[@]}" -o "${BUILD_DIR}/weak_test"
run_limited 120 "${BUILD_DIR}/weak_test"

# Confirm the source object has no libc/pthread dependency beyond the three
# expected runtime entry points.
UNDEFINED="$("${NM}" -u "${BUILD_DIR}/weak.o")"
printf '\nweak.o undefined symbols:\n%s\n' "${UNDEFINED}"
EXPECTED_UNDEFINED="$(printf '%s\n' lang_alloc_v1 lang_free_v1 lang_native_panic_v1 | sort)"
ACTUAL_UNDEFINED="$(awk '$1 == "U" { print $2 }' <<<"${UNDEFINED}" | sort)"
if [[ "${ACTUAL_UNDEFINED}" != "${EXPECTED_UNDEFINED}" ]]; then
  printf 'weak.o undefined symbol set changed; expected exactly:\n%s\n' \
    "${EXPECTED_UNDEFINED}" >&2
  exit 1
fi

# ---------------------------------------------------------------------------
# ASAN/UBSAN oracle. weak.cpp keeps the freestanding code model but drops only
# -fno-sanitize=all so released control blocks can be checked. alloc.cpp and
# panic.cpp remain the production objects.
# ---------------------------------------------------------------------------
ASAN_RUNTIME_FLAGS=(
  -std=c++20
  -O1
  -g
  -ffreestanding
  -fno-builtin
  -fno-exceptions
  -fno-rtti
  -fno-builtin-memcpy
  -fno-builtin-memset
  -fno-builtin-memmove
  -fno-stack-protector
  -fno-omit-frame-pointer
  -fsanitize=address,undefined
  -fno-sanitize-recover=all
)
ASAN_HOST_FLAGS=(
  -std=c++20
  -O1
  -g
  -pthread
  -Wall
  -Wextra
  -Wpedantic
  -fno-omit-frame-pointer
  -fsanitize=address,undefined
  -fno-sanitize-recover=all
  -DLANG_WEAK_ASAN_ORACLE=1
  -I "${SOURCE_ROOT}/runtime/native"
)
run_limited 180 "${CXX}" "${ASAN_RUNTIME_FLAGS[@]}" "${RUNTIME_INCLUDES[@]}" \
  -c "${SOURCE_ROOT}/runtime/native/weak.cpp" -o "${BUILD_DIR}/weak_asan.o"
run_limited 180 "${CXX}" "${ASAN_HOST_FLAGS[@]}" \
  -c "${SOURCE_ROOT}/tests/native/weak/weak_test.cpp" \
  -o "${BUILD_DIR}/weak_test_asan.o"
run_limited 180 "${CXX}" "${ASAN_HOST_FLAGS[@]}" \
  -c "${SOURCE_ROOT}/tests/native/weak/weak_alloc_observer.cpp" \
  -o "${BUILD_DIR}/weak_alloc_observer_asan.o"
run_limited 180 "${CXX}" "${LINK_FLAGS[@]}" "${ASAN_HOST_FLAGS[@]}" \
  "${BUILD_DIR}/weak_test_asan.o" "${BUILD_DIR}/weak_asan.o" \
  "${BUILD_DIR}/alloc.o" "${BUILD_DIR}/panic.o" \
  "${BUILD_DIR}/weak_alloc_observer_asan.o" "${WRAP_FLAGS[@]}" \
  -o "${BUILD_DIR}/weak_test_asan"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=1:strict_string_checks=1"
export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"
run_limited 180 "${BUILD_DIR}/weak_test_asan"
unset ASAN_OPTIONS UBSAN_OPTIONS

# ---------------------------------------------------------------------------
# -nostdlib static ELF. This proves the Weak implementation itself does not
# need host libc, pthreads, libstdc++, or a dynamic loader.
# ---------------------------------------------------------------------------
NOSTD_FLAGS=(
  "${RUNTIME_FLAGS[@]}"
  -fno-pie
)
run_limited 120 "${CXX}" "${NOSTD_FLAGS[@]}" "${RUNTIME_INCLUDES[@]}" \
  -c "${SOURCE_ROOT}/runtime/native/weak.cpp" \
  -o "${BUILD_DIR}/weak_nostdlib.o"
run_limited 120 "${CXX}" "${NOSTD_FLAGS[@]}" "${RUNTIME_INCLUDES[@]}" \
  -c "${SOURCE_ROOT}/runtime/native/alloc.cpp" \
  -o "${BUILD_DIR}/alloc_nostdlib.o"
run_limited 120 "${CXX}" "${NOSTD_FLAGS[@]}" "${RUNTIME_INCLUDES[@]}" \
  -c "${SOURCE_ROOT}/runtime/native/panic.cpp" \
  -o "${BUILD_DIR}/panic_nostdlib.o"
run_limited 120 "${CXX}" "${NOSTD_FLAGS[@]}" "${RUNTIME_INCLUDES[@]}" \
  -c "${SOURCE_ROOT}/tests/native/weak/nostdlib_test.cpp" \
  -o "${BUILD_DIR}/nostdlib_test.o"
run_limited 120 "${CXX}" "${LINK_FLAGS[@]}" -nostdlib -static \
  -Wl,-e,_start -Wl,--no-undefined -Wl,-z,noexecstack -Wl,--build-id=none \
  "${BUILD_DIR}/nostdlib_test.o" "${BUILD_DIR}/weak_nostdlib.o" \
  "${BUILD_DIR}/alloc_nostdlib.o" "${BUILD_DIR}/panic_nostdlib.o" \
  -o "${BUILD_DIR}/weak_nostdlib"
NOSTD_UNDEFINED="$("${NM}" -u "${BUILD_DIR}/weak_nostdlib")"
printf '\nnostdlib ELF undefined symbols:\n%s\n' "${NOSTD_UNDEFINED:-<none>}"
# A weak undefined symbol is not a runtime dependency. Anything with a strong
# U record would be a real unresolved dependency and is rejected.
if grep -qE '[[:space:]]U ' <<<"${NOSTD_UNDEFINED}"; then
  printf 'nostdlib ELF unexpectedly has strong undefined symbols\n' >&2
  exit 1
fi
if "${READELF}" -d "${BUILD_DIR}/weak_nostdlib" 2>&1 | grep -q NEEDED; then
  printf 'nostdlib ELF unexpectedly has a dynamic dependency\n' >&2
  exit 1
fi
run_limited 20 "${BUILD_DIR}/weak_nostdlib"

printf '\nall Weak isolated checks passed\n'
