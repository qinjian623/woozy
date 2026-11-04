#pragma once
#include "lang/hir/hir.hpp"

namespace lang::sema {
// A supplied configuration must match Project's target. Omitting it uses the
// manifest's backend and word_bits; Python has no dependency on the host width.
using CheckConfig = hir::Config;
// Static T07 checking; this result does not grant permission to generate code.
PhaseResult<hir::HirModule>
    check_types(std::shared_ptr<const modules::ResolvedProject>,
                std::optional<CheckConfig> = {}, std::stop_token = {});
PhaseResult<hir::HirModule>
check_types(const PhaseResult<modules::ResolvedProject> &,
            std::optional<CheckConfig> = {}, std::stop_token = {});
PhaseResult<hir::VerifiedHirModule>
    check_project(std::shared_ptr<const modules::ResolvedProject>,
                  std::optional<CheckConfig> = {}, std::stop_token = {});
// This overload preserves failed/recovered/cancelled resolve results. A value
// carried by a failed phase is not permission to enter the checker.
PhaseResult<hir::VerifiedHirModule>
check_project(const PhaseResult<modules::ResolvedProject> &,
              std::optional<CheckConfig> = {}, std::stop_token = {});
} // namespace lang::sema
