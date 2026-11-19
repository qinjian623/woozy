#!/usr/bin/env bash
# Acceptance test for the frozen system-operation metadata.
#
# Verifies, with real stage0 builds:
#   * oracle.cpp (compiling include/lang/hir/system_operation.hpp) reproduces
#     every operation field and the exact all_system_operations order;
#   * the checked-in Woozy literal is exactly what the generator emits;
#   * builtin_types() matches the resolver prelude order extracted from
#     src/modules/resolve.cpp;
#   * --opt 0 and --opt 2 dumps are identical, so the metadata is static;
#   * counts, per-operation parameter counts, operation/name uniqueness.
#
# Environment overrides: WOOZYC, WOOZY_TOOLCHAIN_ROOT, WOOZY_LD_LIBRARY_PATH, CXX.
set -u -o pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
stage0=${WOOZYC:-$root/build/woozy-stdlib-native/woozyc}
toolchains=${WOOZY_TOOLCHAIN_ROOT:-$root/../toolchains}
libs=${WOOZY_LD_LIBRARY_PATH:-$toolchains/llvm-22.1.8/lib:$toolchains/libxml2-legacy-2.13.9/usr/lib}
cxx=${CXX:-c++}
tmp=$(mktemp -d "${TMPDIR:-/tmp}/system-metadata.XXXXXXXX") || exit 1
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp"

failures=0
report() { printf '%s %s\n' "$1" "$2"; }
bad() { failures=$((failures + 1)); report FAIL "$1"; }
good() { report ok "$1"; }
not_run() { report not_run "$1"; }

module=$root/selfhost/src/wzcc/system/system.wz
resolver=$root/src/modules/resolve.cpp

# 1. Build the header oracle (header-only; no compiler library is linked).
if "$cxx" -std=c++20 -I "$root/include" -O2 -o "$tmp/oracle" "$here/oracle.cpp"; then
  good "built C++ header oracle"
else
  bad "C++ header oracle did not compile (${cxx} -std=c++20 -I include)"
  not_run "Wz/O0 and Wz/O2 dumps (oracle unavailable)"
  exit 1
fi

"$tmp/oracle" --dump > "$tmp/oracle.dump" || bad "oracle --dump failed"
"$tmp/oracle" --emit-wz > "$tmp/generated.wz" || bad "oracle --emit-wz failed"

# 2. The checked-in Woozy metadata must be the generator output byte for byte.
if cmp -s "$tmp/generated.wz" "$module"; then
  good "checked-in system.wz equals generated literal table"
else
  bad "system.wz drifted from oracle --emit-wz output"
fi

# 3. The module must stay pure Woozy metadata: no imports, no FFI/exec, and
#    only the two public API functions plus the OperationSpec record.
if grep -qE '^[[:space:]]*import[[:space:]]' "$module"; then
  bad "system.wz has imports (must be static literal metadata)"
else
  good "system.wz has no imports"
fi
if awk '/\(/ && !/^[[:space:]]*OperationSpec\(/ && !/^pub func (operations|builtin_types)\(\)/ {print; bad=1} END{exit bad}' "$module" > "$tmp/calls.txt"; then
  good "system.wz contains only literal rows plus the two API declarations"
else
  bad "system.wz contains a call/exec spelling: $(head -n 3 "$tmp/calls.txt")"
fi
pub_funcs=$(grep -cE '^pub func ' "$module")
if [ "$pub_funcs" -eq 2 ] && grep -qE '^pub func operations\(\) Vec\[OperationSpec\]' "$module" \
   && grep -qE '^pub func builtin_types\(\) Vec\[string\]' "$module"; then
  good "system.wz exposes exactly operations() and builtin_types()"
else
  bad "unexpected public API in system.wz (pub func count=$pub_funcs)"
fi

# 4. Extract the resolver's ordered builtin-type prelude from the C++ source.
sed -n '/for (const auto \*s : {"Unit"/,/builtin_type);/p' "$resolver" \
  | grep -o '"[^"]*"' | tr -d '"' > "$tmp/resolver_types.txt"
resolver_count=$(wc -l < "$tmp/resolver_types.txt")
if [ "$resolver_count" -eq 29 ] && [ "$(head -n 1 "$tmp/resolver_types.txt")" = "Unit" ] \
   && [ "$(tail -n 1 "$tmp/resolver_types.txt")" = "__woozy_IpcHandle" ]; then
  good "extracted resolver builtin prelude ($resolver_count types)"
else
  bad "unexpected resolver builtin prelude extraction (count=$resolver_count)"
fi
{
  printf 'builtin_types %s\n' "$resolver_count"
  sed 's/^/builtin /' "$tmp/resolver_types.txt"
} > "$tmp/expected_builtins.dump"

check_counters() {
  local dump=$1 label=$2
  awk -v label="$label" '
    /^operations / { declared = $2; next }
    /^op / { ops++; number = $2; if (seen_op[number]++) duplicate_op = 1; next }
    /^name / { names++; if (seen_name[$2]++) duplicate_name = 1; next }
    /^params / { want = $2; got = 0; next }
    /^param / { got++; params++; if (got > want) excess = 1; next }
    /^end$/ { if (got != want) short_params = 1; next }
    /^builtin_types / { builtin_declared = $2; next }
    /^builtin / { builtins++; if (seen_builtin[$2]++) duplicate_builtin = 1; next }
    END {
      failed = 0
      if (ops != declared) { printf "FAIL %s: %d op lines, declared %d\n", label, ops, declared; failed = 1 }
      if (duplicate_op) { printf "FAIL %s: duplicate operation numbers\n", label; failed = 1 }
      if (duplicate_name) { printf "FAIL %s: duplicate operation names\n", label; failed = 1 }
      if (short_params || excess) { printf "FAIL %s: parameter count mismatch\n", label; failed = 1 }
      if (builtins != builtin_declared) { printf "FAIL %s: %d builtin lines, declared %d\n", label, builtins, builtin_declared; failed = 1 }
      if (duplicate_builtin) { printf "FAIL %s: duplicate builtin types\n", label; failed = 1 }
      printf "ok %s: operations=%d params=%d unique ops=%d unique names=%d builtin_types=%d\n",
             label, ops, params, length(seen_op), length(seen_name), builtins
      exit failed
    }
  ' "$dump" || return 1
}

check_counters "$tmp/oracle.dump" "oracle" || bad "oracle counters/uniqueness"
check_counters <(cat "$tmp/oracle.dump"; cat "$tmp/expected_builtins.dump") "oracle+resolver" \
  || bad "oracle+resolver counters/uniqueness"

# 5. Real stage0 builds and dumps at -O0 and -O2.
if [ ! -x "$stage0" ]; then
  not_run "stage0 woozyc not executable: $stage0"
  not_run "Wz --opt 0 dump comparison"
  not_run "Wz --opt 2 dump comparison"
  report SUMMARY "failures=$failures not_run=3"
  exit 1
fi

for opt in 0 2; do
  out=$tmp/out$opt
  mkdir -p "$out"
  if LD_LIBRARY_PATH="$libs" "$stage0" build --project "$here/lang.toml" \
       --backend native --out-dir "$out" --opt "$opt" > "$tmp/build$opt.log" 2>&1; then
    good "woozyc build --opt $opt"
  else
    bad "woozyc build --opt $opt failed"
    sed -n '1,20p' "$tmp/build$opt.log" | sed 's/^/  | /'
    not_run "Wz --opt $opt dump comparison"
    continue
  fi
  if [ ! -x "$out/program" ]; then
    bad "build --opt $opt did not produce program"
    not_run "Wz --opt $opt dump comparison"
    continue
  fi
  if "$out/program" > "$tmp/wz$opt.dump"; then
    good "ran Wz --opt $opt dump"
  else
    bad "Wz --opt $opt dump exited nonzero"
    continue
  fi

  awk '/^builtin_types /{exit} {print}' "$tmp/wz$opt.dump" > "$tmp/wz$opt.ops"
  awk '/^builtin_types /{found=1} found{print}' "$tmp/wz$opt.dump" > "$tmp/wz$opt.builtins"

  if cmp -s "$tmp/oracle.dump" "$tmp/wz$opt.ops"; then
    good "Wz --opt $opt matches header oracle (all fields and order, $(wc -l < "$tmp/oracle.dump") lines)"
  else
    bad "Wz --opt $opt differs from header oracle"
    diff "$tmp/oracle.dump" "$tmp/wz$opt.ops" | sed -n '1,20p' | sed 's/^/  | /'
  fi
  if cmp -s "$tmp/expected_builtins.dump" "$tmp/wz$opt.builtins"; then
    good "Wz --opt $opt builtin_types matches resolver prelude order ($resolver_count types)"
  else
    bad "Wz --opt $opt builtin_types differs from resolver prelude"
    diff "$tmp/expected_builtins.dump" "$tmp/wz$opt.builtins" | sed -n '1,20p' | sed 's/^/  | /'
  fi
  check_counters "$tmp/wz$opt.dump" "Wz/O$opt" || bad "Wz --opt $opt counters/uniqueness"
done

if [ -f "$tmp/wz0.dump" ] && [ -f "$tmp/wz2.dump" ]; then
  if cmp -s "$tmp/wz0.dump" "$tmp/wz2.dump"; then
    good "O0 and O2 dumps are identical (static metadata)"
  else
    bad "O0 and O2 dumps differ"
  fi
fi

report SUMMARY "failures=$failures"
[ "$failures" -eq 0 ]
