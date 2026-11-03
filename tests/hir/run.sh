#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
build_dir=$(mktemp -d /tmp/language-t04-hir.XXXXXX)
trap 'rm -rf -- "$build_dir"' EXIT
compiler=${CXX:-g++}
flags=(-std=c++20 -I"$repo_dir/include" "$@")
objects=()
for source in src/hir/hir.cpp src/support/source.cpp src/syntax/lexer.cpp src/syntax/parser.cpp src/modules/resolve.cpp; do
  object="$build_dir/$(basename -- "${source%.cpp}").o"
  "$compiler" "${flags[@]}" -c "$repo_dir/$source" -o "$object"
  objects+=("$object")
done
for name in type_test hir_test; do
  "$compiler" "${flags[@]}" "$repo_dir/tests/hir/$name.cpp" "${objects[@]}" -o "$build_dir/$name"
  "$build_dir/$name"
done
