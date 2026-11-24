#!/usr/bin/env bash
# Single-entry verification for the Vec graph-publication runtime stage.
#
# This is a C++ runtime suite: it uses the real LangVec API and the accepted
# lang-gc-program/world/runtime layers. It does NOT compile or claim a .wz
# Source program. Emitter registration, polls, critical emission and the main
# wrapper remain outside this task, so the Source automatic-GC target is still
# RED.
#
# Every build and executing process has a finite timeout. No raw-clone language
# threads are used by the new probes, so ASAN covers every host mode except the
# seccomp disabled-fast probe (which deliberately denies gettid).

set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)

TOOLCHAINS=${LANG_TOOLCHAINS:-$ROOT/../toolchains}
LLVM_ROOT=${LANG_LLVM_ROOT:-$TOOLCHAINS/llvm-22.1.8}
LIBXML2_ROOT=${LANG_LIBXML2_ROOT:-$TOOLCHAINS/libxml2-legacy-2.13.9}
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

if [ -n "${VEC_GRAPH_PUBLISH_BUILD_DIR:-}" ]; then
  BUILD_DIR=$VEC_GRAPH_PUBLISH_BUILD_DIR
  KEEP_BUILD=1
else
  BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/woozy-vec-graph-publish.XXXXXX")
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
  "$SCRIPT_DIR/vec_graph_publish_test.cpp"
  "$ROOT/runtime/native/gc_program.cpp"
  "$ROOT/runtime/native/gc_runtime.cpp"
  "$ROOT/runtime/native/gc_world.cpp"
  "$ROOT/runtime/native/gc.cpp"
  "$ROOT/runtime/native/weak.cpp"
  "$ROOT/runtime/native/panic.cpp"
  "$ROOT/runtime/native/alloc.cpp"
  "$ROOT/tests/native/gc_runtime/test_alloc_wrap.cpp"
)
HOST_WRAP=(-Wl,--wrap=lang_alloc_v1 -Wl,--wrap=lang_free_v1)
ASAN_SOURCES=(
  "$SCRIPT_DIR/vec_graph_publish_test.cpp"
  "$ROOT/runtime/native/gc_program.cpp"
  "$ROOT/runtime/native/gc_runtime.cpp"
  "$ROOT/runtime/native/gc_world.cpp"
  "$ROOT/runtime/native/gc.cpp"
  "$ROOT/runtime/native/weak.cpp"
  "$ROOT/runtime/native/panic.cpp"
  "$ROOT/tests/native/gc_runtime/test_alloc_asan.cpp"
)

MODE_MARKERS=(
  "init-publish:VEC_GRAPH_INIT_PUBLISH_OK"
  "set-reentrant:VEC_GRAPH_SET_REENTRANT_OK"
  "clear-reentrant:VEC_GRAPH_CLEAR_REENTRANT_OK"
  "shrink-reentrant:VEC_GRAPH_SHRINK_REENTRANT_OK"
  "resize-copy-publish:VEC_GRAPH_RESIZE_COPY_PUBLISH_OK"
  "selfalias-growth:VEC_GRAPH_SELFALIAS_GROWTH_OK"
  "final-release:VEC_GRAPH_FINAL_RELEASE_OK"
  "pending-stop:VEC_GRAPH_PENDING_STOP_OK"
  "slice-copy-snapshot:VEC_GRAPH_SLICE_COPY_SNAPSHOT_OK"
  "resize-fill-alias-snapshot:VEC_GRAPH_RESIZE_FILL_ALIAS_SNAPSHOT_OK"
  "self-set-pre-copied:VEC_GRAPH_SELF_SET_PRE_COPIED_OK"
  "rotate-multichunk-gc:VEC_GRAPH_ROTATE_MULTICHUNK_GC_OK"
  "disabled-fast:VEC_GRAPH_DISABLED_FAST_OK"
)

run_modes() {
  local suffix=$1
  shift
  local allowed=("$@")
  local entry mode marker wanted
  for entry in "${MODE_MARKERS[@]}"; do
    mode=${entry%%:*}
    marker=${entry#*:}
    wanted=0
    for allowed_mode in "${allowed[@]}"; do
      [ "$mode" = "$allowed_mode" ] && wanted=1
    done
    if [ "$wanted" = "1" ]; then
      run_expect "host $suffix $mode" 60 "$marker" \
        "$BUILD_DIR/vec_graph_publish_$suffix" "$mode"
    fi
  done
}

ALL_MODES=()
for entry in "${MODE_MARKERS[@]}"; do ALL_MODES+=("${entry%%:*}"); done
ASAN_MODES=()
for mode in "${ALL_MODES[@]}"; do
  [ "$mode" = "disabled-fast" ] || ASAN_MODES+=("$mode")
done

for opt in O0 O2; do
  suffix=${opt,,}
  if build_step "host $opt" "$COMPILER" "${COMMON[@]}" "-$opt" -g \
      "${HOST_SOURCES[@]}" "${HOST_WRAP[@]}" \
      -o "$BUILD_DIR/vec_graph_publish_$suffix"; then
    run_modes "$suffix" "${ALL_MODES[@]}"
  fi
done

if build_step "host ASAN+UBSAN" "$COMPILER" "${COMMON[@]}" -O1 -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    "${ASAN_SOURCES[@]}" -o "$BUILD_DIR/vec_graph_publish_asan"; then
  for mode in "${ASAN_MODES[@]}"; do
    marker=""
    for entry in "${MODE_MARKERS[@]}"; do
      [ "${entry%%:*}" = "$mode" ] && marker=${entry#*:}
    done
    run_expect "host asan $mode" 120 "$marker" \
      env ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 \
      UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
      "$BUILD_DIR/vec_graph_publish_asan" "$mode"
  done
  skip "host asan disabled-fast" "seccomp deny-gettid conflicts with sanitizer runtime"
fi

# Default single-unit static ELF: panic/alloc/Weak/Vec only, no gc_program.cpp
# and no lang_gc_program_init_v1 call.
STATIC_OK=1
for source in panic alloc weak freestanding start; do
  build_step "static object $source" "$COMPILER" "${FREESTANDING[@]}" \
    -c "$ROOT/runtime/native/$source.cpp" \
    -o "$BUILD_DIR/static_$source.o" || STATIC_OK=0
done
build_step "static test object" "$COMPILER" "${FREESTANDING[@]}" \
  -c "$SCRIPT_DIR/vec_graph_publish_static_test.cpp" \
  -o "$BUILD_DIR/static_test.o" || STATIC_OK=0

if [ "$STATIC_OK" = "1" ] &&
   build_step "default static ELF link" "$COMPILER" -nostdlib -static \
     -Wl,-e,_start -Wl,-z,noexecstack \
     "$BUILD_DIR/static_start.o" "$BUILD_DIR/static_panic.o" \
     "$BUILD_DIR/static_alloc.o" "$BUILD_DIR/static_weak.o" \
     "$BUILD_DIR/static_freestanding.o" "$BUILD_DIR/static_test.o" \
     -o "$BUILD_DIR/vec_graph_publish_static"; then
  ELF=$BUILD_DIR/vec_graph_publish_static
  if file "$ELF" | grep -q 'statically linked'; then
    pass "default static ELF is statically linked"
  else
    fail "default static ELF is not statically linked"
  fi
  if readelf -d "$ELF" 2>&1 | grep -q NEEDED; then
    fail "default static ELF has dynamic NEEDED entries"
  else
    pass "default static ELF has no dynamic dependencies"
  fi
  if readelf -l "$ELF" 2>&1 | grep -q INTERP; then
    fail "default static ELF has an interpreter"
  else
    pass "default static ELF has no interpreter"
  fi
  if nm -u "$ELF" | grep -q .; then
    fail "default static ELF has undefined symbols"
    nm -u "$ELF" | sed -e 's/^/    /' | head -20
  else
    pass "default static ELF has no undefined symbols"
  fi
  if grep -a -q 'libstdc++\|libc\.so\|libgcc_s' "$ELF"; then
    fail "default static ELF references libc/libstdc++ strings"
  else
    pass "default static ELF has no libc/libstdc++ dependency strings"
  fi
  run_expect "default static ELF execution" 30 "" "$ELF"
fi

# Existing VecRuntime / growth / Slice regressions, rebuilt against the same
# panic.cpp. Each harness supplies its own allocator wrappers.
VEC_RUNTIME_SOURCES=(
  "$ROOT/tests/native/vec_runtime_test.cpp"
  "$ROOT/runtime/native/panic.cpp"
  "$ROOT/runtime/native/alloc.cpp"
  "$ROOT/runtime/native/weak.cpp"
)
if build_step "legacy VecRuntime harness" "$COMPILER" "${COMMON[@]}" -O2 -g \
    "${VEC_RUNTIME_SOURCES[@]}" "${HOST_WRAP[@]}" \
    -o "$BUILD_DIR/legacy_vec_runtime"; then
  run_expect "legacy VecRuntime normal" 60 "reverse-index-and-prefix-ok" \
    "$BUILD_DIR/legacy_vec_runtime"
  run_expect "legacy VecRuntime null-observe" 60 "" \
    "$BUILD_DIR/legacy_vec_runtime" null-observe
fi

VEC_GROWTH_SOURCES=(
  "$ROOT/tests/native/vec_growth_test.cpp"
  "$ROOT/runtime/native/panic.cpp"
  "$ROOT/runtime/native/alloc.cpp"
  "$ROOT/runtime/native/weak.cpp"
)
if build_step "legacy VecGrowth harness" "$COMPILER" "${COMMON[@]}" -O2 -g \
    "${VEC_GROWTH_SOURCES[@]}" "${HOST_WRAP[@]}" \
    -o "$BUILD_DIR/legacy_vec_growth"; then
  run_expect "legacy VecGrowth" 60 "vec-growth-ok" \
    "$BUILD_DIR/legacy_vec_growth"
fi

SLICE_SOURCES=(
  "$ROOT/tests/native/slice/slice_runtime_test.cpp"
  "$ROOT/runtime/native/slice.cpp"
  "$ROOT/runtime/native/panic.cpp"
  "$ROOT/runtime/native/alloc.cpp"
  "$ROOT/runtime/native/weak.cpp"
)
if build_step "legacy Slice runtime harness" "$COMPILER" "${COMMON[@]}" -O2 -g \
    "${SLICE_SOURCES[@]}" "${HOST_WRAP[@]}" \
    -o "$BUILD_DIR/legacy_slice_runtime"; then
  run_expect "legacy Slice runtime" 60 "slice runtime harness: passed" \
    "$BUILD_DIR/legacy_slice_runtime"
fi

if [ "${RUN_LEGACY_WEAK:-1}" = "1" ]; then
  if timeout 120s env CXX="$COMPILER" \
      LANG_WEAK_BUILD_DIR="$BUILD_DIR/legacy-weak" \
      bash "$ROOT/tests/native/weak/run.sh" \
      >"$BUILD_DIR/legacy_weak.log" 2>&1; then
    pass "legacy Weak suite"
  else
    fail "legacy Weak suite"
    show_log "$BUILD_DIR/legacy_weak.log"
  fi
fi

echo
echo "compiler: $VERSION"
echo "sha256:"
for file in \
  runtime/native/panic.cpp runtime/native/vec.hpp \
  tests/native/vec_graph_publish/vec_graph_publish_test.cpp \
  tests/native/vec_graph_publish/vec_graph_publish_static_test.cpp \
  tests/native/vec_graph_publish/run_all.sh \
  tests/native/vec_graph_publish/CONTRACT.md; do
  sha256sum "$ROOT/$file"
done
echo
echo "SUMMARY pass=$PASS_COUNT fail=$FAIL_COUNT skip=$SKIP_COUNT"
if [ "$FAIL_COUNT" -ne 0 ]; then
  exit 1
fi
exit 0
