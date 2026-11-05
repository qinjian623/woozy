#pragma once
#include "lang/hir/hir.hpp"
#include <functional>
#include <map>
#include <stdexcept>
#include <stop_token>
#include <vector>

namespace lang::hir {
// Internal, non-public support shared by semantic checking and the independent
// HIR verifier. It never becomes part of HirModule or a backend interface.
struct PatternConstructor {
  SymbolId variant;
  std::vector<TypeId> fields;
};
using PatternConstructorLookup =
    std::function<std::vector<PatternConstructor>(TypeId)>;
using PatternEnumPredicate = std::function<bool(TypeId)>;

// Shared constructor view for a concrete nominal enum type. Field types are
// substituted with the type's actual arguments; interning stays in the
// caller's TypeContext copy.
std::vector<PatternConstructor>
pattern_constructors(const HirModule &module, TypeId type, TypeContext &types);

class PatternAnalysisLimit : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

// Maranget-style usefulness over the current pattern column. Enum variant
// constructors are specialized only when reachable from the query/matrix; the
// analysis does not enumerate a payload Cartesian product up front.
class PatternCoverage {
public:
  PatternCoverage(TypeId scrutinee, PatternConstructorLookup constructors,
                  PatternEnumPredicate is_enum, std::stop_token stop,
                  size_t budget = 100000);

  // Returns false when the pattern cannot match any value not already covered.
  // A useful pattern is appended to the matrix.
  bool add(const Pattern &pattern);

  // True only when the accumulated matrix covers every constructor value.
  bool exhaustive();

private:
  using Row = std::vector<Pattern>;
  struct Specialized {
    std::vector<Row> matrix;
    Row query;
  };

  const std::vector<PatternConstructor> &constructors(TypeId type);
  void spend(size_t amount) const;
  bool useful(const std::vector<Row> &matrix, const Row &query,
              size_t depth);

  TypeId scrutinee_;
  PatternConstructorLookup lookup_;
  PatternEnumPredicate is_enum_;
  std::stop_token stop_;
  mutable size_t budget_;
  std::map<uint64_t, std::vector<PatternConstructor>> cache_;
  std::vector<Row> matrix_;
};
} // namespace lang::hir
