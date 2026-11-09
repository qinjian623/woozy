#pragma once
#include "lang/hir/hir.hpp"
#include <set>
namespace lang::hir {
// True when a whole slot of this type may be used as an inout binding.
// Numeric/bool are data leaves; Unit and Function are allowed only as members
// of a finite nominal aggregate, never as the top-level slot type. TypeParameter
// is allowed only when its declaration is in `parameters`.
bool inout_data_type(const HirModule &, TypeId,
                     const std::set<uint64_t> &parameters = {});
namespace detail {
// Producer interns all concrete member types before publishing records.
PhaseResult<std::monostate> produce_ownership(HirModule &, std::stop_token);
// Rebuilds obligations without consulting any supplied ownership/exit actions.
PhaseResult<std::monostate> verify_ownership(const HirModule &,
                                             std::stop_token);
} // namespace lang::hir::detail
} // namespace lang::hir
