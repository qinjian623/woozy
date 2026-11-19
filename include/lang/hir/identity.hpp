#pragma once
#include "lang/hir/hir.hpp"
#include <memory>

namespace lang::hir {
// Reuse one index across package emission from the same immutable verified
// snapshot. Frontend callers may use a scoped index only while declarations,
// instances and parsed files stay unchanged; intern may append new types.
class IdentityKeys {
  struct Impl;
  std::unique_ptr<Impl> impl_;
public:
  explicit IdentityKeys(const HirModule &);
  explicit IdentityKeys(const VerifiedHirModule &);
  ~IdentityKeys();
  IdentityKeys(const IdentityKeys &) = delete;
  IdentityKeys &operator=(const IdentityKeys &) = delete;
  bool matches(const HirModule &) const;
  std::vector<size_t> source_path(modules::NodeRef) const;
  std::string declaration_key(SymbolId) const;
  std::string type_key(TypeId) const;
  std::string function_name(SymbolId) const;
  ModuleId function_owner(SymbolId) const;
};
std::string declaration_key(const HirModule &, SymbolId);
std::string type_key(const HirModule &, TypeId);
std::string native_function_name(const HirModule &, SymbolId);
ModuleId function_owner(const HirModule &, SymbolId);
}
