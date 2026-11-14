#!/usr/bin/env bash
# Pure check-helper test. It never invokes a compiler, target program, readelf,
# or nm: run.sh is sourced in check-only mode and the tools are shell fixtures.
set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
OUT=${TID_CACHE_SELFTEST_DIR:-$ROOT/.work/SH15-tid-cache-test-correction-20261115-selftest}
mkdir -p "$OUT"

TID_CACHE_LIB_ONLY=1 TID_CACHE_BUILD_DIR="$OUT/build" source "$SCRIPT_DIR/run.sh" || {
  echo "FAIL selftest: could not source run.sh helpers" >&2
  exit 90
}

BOTH="$OUT/both.nm"
ONLY_CACHED="$OUT/only_cached.nm"
ONLY_CLEAR="$OUT/only_clear.nm"
printf '                 w lang_native_cached_tid\n                 w lang_native_clear_tid_cache\n' >"$BOTH"
printf '                 w lang_native_cached_tid\n' >"$ONLY_CACHED"
printf '                 w lang_native_clear_tid_cache\n' >"$ONLY_CLEAR"

FAKE_READELF_STATUS=0
readelf() { return "$FAKE_READELF_STATUS"; }
FAKE_NM_STATUS=0
FAKE_NM_FIXTURE=$BOTH
nm() {
  [ "$FAKE_NM_STATUS" -eq 0 ] || return "$FAKE_NM_STATUS"
  cat "$FAKE_NM_FIXTURE"
}

SELFTEST_FAILURES=0
expect_failure() {
  local name=$1
  shift
  local before=$FAIL
  local status=0
  local log=$OUT/${name// /_}.log
  "$@" >"$log" 2>&1 || status=$?
  if [ "$status" -eq 0 ]; then
    echo "FAIL selftest: $name unexpectedly passed"
    SELFTEST_FAILURES=$((SELFTEST_FAILURES + 1))
  elif [ "$FAIL" -ne $((before + 1)) ]; then
    echo "FAIL selftest: $name did not record exactly one failure (status=$status, fail $before->$FAIL)"
    SELFTEST_FAILURES=$((SELFTEST_FAILURES + 1))
  else
    echo "PASS selftest: $name failed as required"
  fi
}

expect_success() {
  local name=$1
  shift
  local before=$FAIL
  local status=0
  local log=$OUT/${name// /_}.log
  "$@" >"$log" 2>&1 || status=$?
  if [ "$status" -ne 0 ] || [ "$FAIL" -ne "$before" ]; then
    echo "FAIL selftest: $name unexpectedly failed (status=$status, fail $before->$FAIL)"
    SELFTEST_FAILURES=$((SELFTEST_FAILURES + 1))
  else
    echo "PASS selftest: $name passed"
  fi
}

FAKE_READELF_STATUS=7
expect_failure readelf-d-nonzero check_core_no_needed /fixture/elf "$OUT/readelf_d.out"
expect_failure readelf-l-nonzero check_core_no_interp /fixture/elf "$OUT/readelf_l.out"

FAKE_READELF_STATUS=0
FAKE_NM_STATUS=7
expect_failure nm-strong-nonzero check_core_no_strong_undefined /fixture/elf "$OUT/nm_strong.out"
expect_failure nm-hosted-nonzero check_hosted_weak_hooks /fixture/object "$OUT/nm_hosted_nonzero.out"

FAKE_NM_STATUS=0
FAKE_NM_FIXTURE=$ONLY_CACHED
expect_failure hosted-missing-clear_tid_cache check_hosted_weak_hooks \
  /fixture/object "$OUT/nm_missing_clear.out"
FAKE_NM_FIXTURE=$ONLY_CLEAR
expect_failure hosted-missing-cached_tid check_hosted_weak_hooks \
  /fixture/object "$OUT/nm_missing_cached.out"

FAKE_NM_FIXTURE=$BOTH
expect_success hosted-both-weak-hooks check_hosted_weak_hooks \
  /fixture/object "$OUT/nm_both.out"

echo "SELFTEST failures=$SELFTEST_FAILURES"
[ "$SELFTEST_FAILURES" -eq 0 ]
