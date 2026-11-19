#!/usr/bin/env bash
# Run the MEM02 real-source automatic cycle collection checks.
# Usage: run.sh [OUT] [extra runner args...]
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/../../.." && pwd)"
TOOLCHAINS="${LANG_TOOLCHAINS:-$REPO_ROOT/../toolchains}"
BUILD="${WOOZY_NATIVE_BUILD:-$REPO_ROOT/build/woozy-stdlib-native}"
LLVM="${WOOZY_LLVM:-$TOOLCHAINS/llvm-22.1.8}"
LIBXML2="${WOOZY_LIBXML2:-$TOOLCHAINS/libxml2-legacy-2.13.9/usr/lib}"
OUT="${1:-/tmp/woozy-language-gc-source}"
if [[ "$OUT" == --* || -z "$OUT" ]]; then
  OUT=/tmp/woozy-language-gc-source
else
  shift || true
fi

exec python3 "$HERE/run_language_gc_source.py" \
  --cli "$BUILD/woozyc" \
  --helper "$BUILD/native/lang_native_helper" \
  --runtime "$BUILD/native/liblang_native_runtime.a" \
  --clang "$LLVM/bin/clang" \
  --readelf "$LLVM/bin/llvm-readelf" \
  --libxml2-lib "$LIBXML2" \
  --root "$REPO_ROOT" \
  --out "$OUT" \
  "$@"
