#!/bin/sh
# Static-ELF evidence for the no-libc Slice smoke.
set -eu
elf=$1

if readelf -lW "$elf" | grep -q 'INTERP'; then
  echo "slice-static-elf: PT_INTERP present" >&2
  exit 1
fi

dynamic=$(readelf -dW "$elf" 2>/dev/null || true)
if printf '%s\n' "$dynamic" | grep -q '(NEEDED)'; then
  echo "slice-static-elf: DT_NEEDED present" >&2
  exit 1
fi

if nm -u "$elf" | grep -Eq '(^|[[:space:]])U[[:space:]]'; then
  echo "slice-static-elf: strong undefined symbol present" >&2
  nm -u "$elf" >&2
  exit 1
fi

strong_undefined=$(readelf -sW "$elf" |
  awk '$7 == "UND" && $5 == "GLOBAL" && $6 != "WEAK" { print $8 }')
if [ -n "$strong_undefined" ]; then
  echo "slice-static-elf: strong undefined symbol: $strong_undefined" >&2
  exit 1
fi

echo "slice-static-elf: passed (no INTERP, NEEDED, or strong undefined)"
