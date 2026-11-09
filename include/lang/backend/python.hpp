#pragma once
#include "lang/hir/hir.hpp"

namespace lang::backend::python {
inline constexpr std::string_view runtime_version =
    "compiler-dev/1-python-values/7";
struct Options {
  // Without a runner, the output can be imported by an explicit test harness.
  bool entrypoint = true;
};
struct LineSource {
  uint64_t line; // one-based generated Python line
  SourceSpan source;
};
struct SymbolName {
  SymbolId symbol;
  std::string name;
};
struct Output {
  std::string code;    // write as program.py (or another Python module name)
  std::string runtime; // write alongside it as langrt.py
  std::vector<LineSource> source_map;
  std::vector<SymbolName> symbols;
};
// Pure code generation: no filesystem access, subprocesses or Python imports.
// Only a Python/64 VerifiedHirModule is accepted. Entry is a zero-argument
// function returning Unit or i32, with or without typed throws. Unsupported profiles report the shared
// E_CAPABILITY_UNAVAILABLE diagnostic; no new shared types or error codes.
PhaseResult<Output> emit(const hir::VerifiedHirModule &, SymbolId entry,
                         const Options & = {}, std::stop_token = {});
} // namespace lang::backend::python
