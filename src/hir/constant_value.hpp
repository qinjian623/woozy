#pragma once
#include "lang/hir/hir.hpp"
#include <functional>
namespace lang::hir {
bool same_constant_value(const ConstantValue &, const ConstantValue &);
void transform_constant(ConstantValue &, const Substitutions &, TypeContext &,
                        const std::function<void(FunctionValue &)> &);
std::optional<ConstantValue> evaluate_hir_constant(const HirModule &, HirNodeId,
                                                   std::stop_token = {});
bool aggregate_constant_expression(const HirModule &, HirNodeId);
// Source-only evaluation. The callbacks use the consumer's real type and
// function-source resolution; no producer literal/aggregate data is read.
ConstantValue source_constant_value(
    const HirModule &, TypeContext &, modules::NodeRef, TypeId,
    const std::function<TypeId(modules::NodeRef)> &,
    const std::function<FunctionValue(modules::NodeRef, TypeId)> &,
    std::stop_token = {});
} // namespace lang::hir
