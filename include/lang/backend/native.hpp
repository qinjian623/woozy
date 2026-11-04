#pragma once
#include "lang/ir/native.hpp"

namespace lang::backend::native {
inline constexpr std::string_view runtime_abi = "lang-native-scalars/1";
struct Options {
  std::string triple = "x86_64-unknown-linux-gnu";
  std::string cpu = "generic";
  std::string features;
  unsigned optimization = 0; // exactly 0 or 2
};
struct Output {
  std::vector<uint8_t> object;
  std::string native_ir, llvm_before_optimization, llvm_ir;
  std::string triple, data_layout;
};
// No parsing or name lookup. The caller selects an existing function SymbolId.
// Current executable entry supports no arguments and i32 or Unit result.
// All output is returned only after CFG + LLVM verification and object
// emission.
PhaseResult<Output> emit(const hir::VerifiedHirModule &, SymbolId entry,
                         const Options & = {});
PhaseResult<Output> emit(const ir::VerifiedModule &, SymbolId entry,
                         const Options & = {});
} // namespace lang::backend::native
