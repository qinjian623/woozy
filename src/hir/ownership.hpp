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
// Canonical source Weak targets. The declaration is selected by its real
// resolved SymbolId and logical std module, never by a same-shaped user type
// or a type_key string.
bool weak_target(const HirModule &, TypeId);
// Logical copyability used by closure captures and bound receivers. A
// CForeignResource is copyable only when its declaration provides retain.
bool ordinary_copyable(const HirModule &, TypeId, std::stop_token = {});
// Move/copy intent for a closure capture before all concrete member types have
// been interned publicly. This publishes no records.
bool owning_value(const HirModule &, TypeId, std::stop_token = {});
namespace detail {
// Producer interns all concrete member types before publishing records.
PhaseResult<std::monostate> produce_ownership(HirModule &, std::stop_token);
// Rebuilds obligations without consulting any supplied ownership/exit actions.
PhaseResult<std::monostate> verify_ownership(const HirModule &,
                                             std::stop_token);
} // namespace lang::hir::detail
} // namespace lang::hir
