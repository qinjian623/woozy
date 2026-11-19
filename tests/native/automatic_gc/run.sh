#!/usr/bin/env bash
# Reproducible wrapper for the MEM02 automatic-GC tests.
# The first argument is the output directory; remaining arguments go to the
# Python runner (for example --baseline-only, --case self-cycle, --opt 0,
# --compact-out FILE). For stable evidence while Root rebuilds woozyc, copy the
# CLI to /tmp and pass --cli PATH plus --expect-cli-sha256/--expect-helper-sha256/
# --expect-runtime-sha256; a pin mismatch is reported as inconclusive, not as a
# GC result.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/../../.." && pwd)"
TOOLCHAINS="${LANG_TOOLCHAINS:-$REPO_ROOT/../toolchains}"
BUILD="${WOOZY_NATIVE_BUILD:-$REPO_ROOT/build/woozy-stdlib-native}"
LLVM="${WOOZY_LLVM:-$TOOLCHAINS/llvm-22.1.8}"
LIBXML2="${WOOZY_LIBXML2:-$TOOLCHAINS/libxml2-legacy-2.13.9/usr/lib}"
OUT="${1:-/tmp/woozy-automatic-gc}"
if [[ "$OUT" == --* || -z "$OUT" ]]; then
  OUT=/tmp/woozy-automatic-gc
else
  shift || true
fi

exec python3 "$HERE/run_automatic_gc.py" \
  --cli "$BUILD/woozyc" \
  --helper "$BUILD/native/lang_native_helper" \
  --runtime "$BUILD/native/liblang_native_runtime.a" \
  --clang "$LLVM/bin/clang" \
  --readelf "$LLVM/bin/llvm-readelf" \
  --libxml2-lib "$LIBXML2" \
  --root "$REPO_ROOT" \
  --out "$OUT" \
  "$@"
