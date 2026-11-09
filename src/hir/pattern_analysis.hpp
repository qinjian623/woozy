#pragma once
#include "lang/hir/hir.hpp"
#include <functional>
#include <map>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

namespace lang::hir {
// Internal, non-public support shared by semantic checking and the independent
// HIR verifier. It never becomes part of HirModule or a backend interface.
enum class PatternDomainKind {
  nominal_enum,
  bool_domain,
  integer_domain,
  float_domain,
  string_domain,
  opaque
};
struct PatternConstructor {
  enum class Kind { variant, literal, other } kind = Kind::other;
  SymbolId variant;        // variant kind only
  std::string literal_key; // literal kind only; canonical target value
  std::vector<TypeId> fields;
};
struct PatternDomain {
  PatternDomainKind kind = PatternDomainKind::opaque;
  std::vector<PatternConstructor> constructors; // nominal enum variants only
  unsigned bits = 0; // integer/float target width
};
using PatternDomainLookup = std::function<PatternDomain(TypeId)>;

// Shared constructor view for a concrete nominal enum type. Field types are
// substituted with the type's actual arguments; interning stays in the
// caller's TypeContext copy.
std::vector<PatternConstructor>
pattern_constructors(const HirModule &module, TypeId type, TypeContext &types);

// Complete type-domain classification for coverage. An ordinary empty enum is
// uninhabited; every non-enum type has at least the internal Other class.
PatternDomain pattern_domain(const HirModule &module, TypeId type,
                             TypeContext &types);

class PatternAnalysisLimit : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

// Maranget-style usefulness over the current pattern column. Enum variant
// constructors are specialized only when reachable from the query/matrix; the
// analysis does not enumerate a payload Cartesian product up front. Literal
// columns use the distinct values already present plus one internal Other
// class, except a genuinely complete small bool/integer domain.
class PatternCoverage {
public:
  PatternCoverage(TypeId scrutinee, PatternDomainLookup domain,
                  std::stop_token stop, size_t budget = 100000);

  // Returns false when the pattern cannot match any value not already covered.
  // A useful pattern is appended to the matrix.
  bool add(const Pattern &pattern);

  // True only when the accumulated matrix covers every value.
  bool exhaustive();

private:
  using Row = std::vector<Pattern>;
  struct Specialized {
    std::vector<Row> matrix;
    Row query;
  };

  const PatternDomain &domain(TypeId type);
  void spend(size_t amount) const;
  std::vector<PatternConstructor>
  constructors(TypeId type, const std::vector<Row> &matrix, const Row &query);
  std::string literal_key(const Pattern &pattern, const PatternDomain &d);
  std::vector<Row> specialize(const PatternConstructor &constructor,
                              const std::vector<Row> &matrix);
  bool useful(const std::vector<Row> &matrix, const Row &query, size_t depth);

  TypeId scrutinee_;
  PatternDomainLookup lookup_;
  std::stop_token stop_;
  mutable size_t budget_;
  std::map<uint64_t, PatternDomain> cache_;
  std::vector<Row> matrix_;
};
} // namespace lang::hir
