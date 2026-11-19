#pragma once
#include "lang/hir/hir.hpp"
#include <stdexcept>
#include <unordered_map>

namespace lang::ir::detail {
// Substitution may intern storage-only types in this private copy. Such IDs
// never enter Native IR. A function field is a leaf, irrespective of its result.
class AggregateTypes {
  std::unordered_map<uint64_t, const hir::NominalDecl *> nominals_by_symbol_;

public:
  hir::TypeContext types;
  explicit AggregateTypes(const hir::HirModule &m) : types(m.types) {
    nominals_by_symbol_.reserve(m.nominals.size());
    for (const auto &d : m.nominals)
      nominals_by_symbol_.emplace(d.symbol.value, &d);
  }
  const hir::NominalDecl &declaration(TypeId id) const {
    const auto *t = types.find(id);
    if (t) {
      if (auto it = nominals_by_symbol_.find(t->declaration.value);
          it != nominals_by_symbol_.end())
        return *it->second;
    }
    throw std::logic_error("unknown aggregate declaration");
  }
  std::vector<hir::FieldDecl> fields(TypeId id,
                                    std::optional<SymbolId> variant = {}) {
    const auto t = *types.find(id);
    const auto &d = declaration(id);
    auto result = d.fields;
    if (variant) {
      bool found = false;
      for (const auto &v : d.variants)
        if (v.symbol == *variant) {
          result = v.payload;
          found = true;
          break;
        }
      if (!found)
        throw std::logic_error("unknown enum variant");
    }
    hir::Substitutions subst;
    for (size_t p = 0; p < d.params.size(); ++p)
      subst.emplace_back(d.params[p].symbol, t.type_args.at(p));
    for (auto &f : result)
      f.type = hir::substitute(f.type, subst, types);
    return result;
  }
  size_t field_index(TypeId id, SymbolId field,
                     std::optional<SymbolId> variant = {}) {
    auto fs = fields(id, variant);
    for (size_t p = 0; p < fs.size(); ++p)
      if (fs[p].symbol == field)
        return p;
    throw std::logic_error("unknown aggregate field");
  }
  size_t variant_index(TypeId id, SymbolId variant) const {
    const auto &vs = declaration(id).variants;
    for (size_t p = 0; p < vs.size(); ++p)
      if (vs[p].symbol == variant)
        return p;
    throw std::logic_error("unknown enum variant");
  }
};
} // namespace lang::ir::detail
