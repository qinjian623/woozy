#pragma once
#include "lang/hir/hir.hpp"
#include <set>

namespace lang::hir::detail {
// A public source interface may name a nominal type only through an exported
// declaration. Walk the final TypeId graph without following a nominal
// declaration's private implementation fields. Type parameters, builtins and
// foreign bindings have no source-level private nominal identity here.
inline std::optional<SymbolId> private_interface_type(
    const TypeContext &types, const modules::ResolvedProject &resolved,
    TypeId root, std::set<uint64_t> &visited) {
  if (!visited.insert(root.value).second)
    return {};
  const Type *found = types.find(root);
  if (!found)
    return {};
  const Type &t = *found;
  if (t.kind == TypeKind::NominalStruct || t.kind == TypeKind::NominalEnum ||
      t.kind == TypeKind::TraitObject) {
    if (t.declaration.value >= resolved.symbols.size())
      return {};
    const auto &s = resolved.symbols[t.declaration.value];
    if ((s.kind == modules::SymbolKind::type ||
         s.kind == modules::SymbolKind::trait) &&
        !s.exported)
      return s.id;
  }
  for (auto argument : t.type_args)
    if (auto private_type = private_interface_type(types, resolved, argument,
                                                   visited))
      return private_type;
  if (t.kind != TypeKind::Function)
    return {};
  for (const auto &parameter : t.parameters)
    if (auto private_type = private_interface_type(types, resolved,
                                                   parameter.type, visited))
      return private_type;
  if (auto private_type =
          private_interface_type(types, resolved, t.result, visited))
    return private_type;
  if (t.throws_type)
    return private_interface_type(types, resolved, *t.throws_type, visited);
  return {};
}
inline std::optional<SymbolId> private_interface_type(
    const TypeContext &types, const modules::ResolvedProject &resolved,
    TypeId root) {
  std::set<uint64_t> visited;
  return private_interface_type(types, resolved, root, visited);
}
inline std::optional<SymbolId> private_interface_type(
    const TypeContext &types, const modules::ResolvedProject &resolved,
    const std::vector<TypeId> &roots) {
  std::set<uint64_t> visited;
  for (auto root : roots)
    if (auto private_type =
            private_interface_type(types, resolved, root, visited))
      return private_type;
  return {};
}
} // namespace lang::hir::detail
