#!/usr/bin/env bash
# Run the original GC kernel suite, the Weak control-block suite (including its
# invalidation additions), and the combined GC+Weak integration suite with the
# fixed LLVM 22.1.8 toolchain.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"

printf '=== GC kernel ===\n'
"$here/run_tests.sh"

printf '\n=== Weak control block ===\n'
"$root/tests/native/weak/run.sh"

printf '\n=== GC + Weak integration ===\n'
"$here/run_gc_weak.sh"

printf '\nall GC/Weak suites passed\n'
