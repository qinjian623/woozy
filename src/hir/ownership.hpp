#pragma once
#include "lang/hir/hir.hpp"
namespace lang::hir::detail {
// Producer interns all concrete member types before publishing records.
PhaseResult<std::monostate> produce_ownership(HirModule &, std::stop_token);
// Rebuilds obligations without consulting any supplied ownership/exit actions.
PhaseResult<std::monostate> verify_ownership(const HirModule &,
                                             std::stop_token);
} // namespace lang::hir::detail
