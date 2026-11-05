#include "pattern_analysis.hpp"
#include "lang/support/source.hpp"
#include <algorithm>
#include <set>

namespace lang::hir {
namespace {
Pattern wildcard(TypeId type) {
  Pattern pattern;
  pattern.type = type;
  return pattern;
}
} // namespace

std::vector<PatternConstructor>
pattern_constructors(const HirModule &module, TypeId type_id,
                     TypeContext &types) {
  const Type *type = types.find(type_id);
  if (!type)
    throw PatternAnalysisLimit("enum pattern constructor type is missing");
  if (type->kind != TypeKind::NominalEnum)
    return {};
  const NominalDecl *declaration = nullptr;
  for (const auto &candidate : module.nominals)
    if (candidate.symbol == type->declaration)
      declaration = &candidate;
  if (!declaration || declaration->params.size() != type->type_args.size())
    throw PatternAnalysisLimit(
        "enum pattern constructor declaration/type arguments mismatch");
  Substitutions substitutions;
  for (size_t i = 0; i < declaration->params.size(); ++i)
    substitutions.push_back(
        {declaration->params[i].symbol, type->type_args[i]});
  std::vector<PatternConstructor> constructors;
  for (const auto &variant : declaration->variants) {
    PatternConstructor constructor;
    constructor.variant = variant.symbol;
    for (const auto &field : variant.payload)
      constructor.fields.push_back(
          substitute(field.type, substitutions, types));
    constructors.push_back(std::move(constructor));
  }
  return constructors;
}

PatternCoverage::PatternCoverage(TypeId scrutinee,
                                 PatternConstructorLookup constructors,
                                 PatternEnumPredicate is_enum,
                                 std::stop_token stop, size_t budget)
    : scrutinee_(scrutinee), lookup_(std::move(constructors)),
      is_enum_(std::move(is_enum)), stop_(std::move(stop)), budget_(budget) {}

void PatternCoverage::spend(size_t amount) const {
  check_cancelled(stop_);
  if (amount > budget_)
    throw PatternAnalysisLimit(
        "enum pattern coverage analysis exceeded its resource budget");
  budget_ -= amount;
}

const std::vector<PatternConstructor> &
PatternCoverage::constructors(TypeId type) {
  auto found = cache_.find(type.value);
  if (found != cache_.end())
    return found->second;
  if (!lookup_)
    throw PatternAnalysisLimit("enum pattern coverage has no constructor view");
  auto entries = lookup_(type);
  spend(entries.size() + 1);
  return cache_.emplace(type.value, std::move(entries)).first->second;
}

bool PatternCoverage::useful(const std::vector<Row> &matrix, const Row &query,
                             size_t depth) {
  check_cancelled(stop_);
  if (depth > 256)
    throw PatternAnalysisLimit(
        "enum pattern coverage analysis exceeded its nesting budget");
  spend(matrix.size() + query.size() + 1);
  if (query.empty())
    return matrix.empty();
  // A row made only of wildcard/binding patterns covers every tuple in this
  // matrix, so no query can add a new value. Besides being correct, this keeps
  // a final `case _` from expanding every nested constructor combination.
  for (const auto &row : matrix)
    if (std::all_of(row.begin(), row.end(),
                    [](const Pattern &cell) { return !cell.variant; }))
      return false;

  const auto &head = query.front();
  if (head.variant) {
    const auto &all = constructors(head.type);
    auto constructor = std::find_if(
        all.begin(), all.end(),
        [&](const PatternConstructor &c) { return c.variant == *head.variant; });
    if (constructor == all.end())
      throw PatternAnalysisLimit(
          "enum pattern constructor is absent from its declared type");
    if (head.payload.size() != constructor->fields.size())
      throw PatternAnalysisLimit(
          "enum pattern payload count differs from its constructor");
    Specialized next;
    next.query = head.payload;
    next.query.insert(next.query.end(), query.begin() + 1, query.end());
    for (const auto &row : matrix) {
      if (row.empty())
        throw PatternAnalysisLimit("enum pattern coverage row is empty");
      const auto &cell = row.front();
      Row specialized;
      if (cell.variant) {
        if (*cell.variant != *head.variant)
          continue;
        if (cell.payload.size() != constructor->fields.size())
          throw PatternAnalysisLimit(
              "enum pattern matrix payload differs from its constructor");
        specialized = cell.payload;
      } else {
        for (auto type : constructor->fields)
          specialized.push_back(wildcard(type));
      }
      specialized.insert(specialized.end(), row.begin() + 1, row.end());
      next.matrix.push_back(std::move(specialized));
    }
    return useful(next.matrix, next.query, depth + 1);
  }

  // A binding and a wildcard are both total at their position. A non-enum
  // column (primitive, Vec, TypeParameter, ...) has no variant constructors,
  // but it still has following columns. Treat that column as a single total
  // value and continue with the default matrix; otherwise a later Bit.A/Bit.B
  // distinction would be discarded. An actual empty enum keeps its existing
  // uninhabited rule.
  const auto &all = constructors(head.type);
  if (all.empty()) {
    if (!is_enum_)
      throw PatternAnalysisLimit("enum pattern coverage has no type view");
    if (!is_enum_(head.type)) {
      std::vector<Row> defaults;
      for (const auto &row : matrix) {
        if (row.empty())
          throw PatternAnalysisLimit("enum pattern coverage row is empty");
        if (!row.front().variant)
          defaults.emplace_back(row.begin() + 1, row.end());
      }
      Row tail(query.begin() + 1, query.end());
      return useful(defaults, tail, depth + 1);
    }
    return matrix.empty();
  }
  std::set<uint64_t> present;
  for (const auto &row : matrix) {
    if (row.empty())
      throw PatternAnalysisLimit("enum pattern coverage row is empty");
    if (row.front().variant)
      present.insert(row.front().variant->value);
  }
  if (present.size() != all.size()) {
    // The first column is incomplete. A constructor absent from every
    // explicit head can only be matched through wildcard rows, so the
    // standard default-matrix step avoids enumerating those payloads here.
    std::vector<Row> defaults;
    for (const auto &row : matrix)
      if (!row.front().variant)
        defaults.emplace_back(row.begin() + 1, row.end());
    Row tail(query.begin() + 1, query.end());
    return useful(defaults, tail, depth + 1);
  }
  for (const auto &constructor : all) {
    Specialized next;
    for (auto type : constructor.fields)
      next.query.push_back(wildcard(type));
    next.query.insert(next.query.end(), query.begin() + 1, query.end());
    for (const auto &row : matrix) {
      if (row.empty())
        throw PatternAnalysisLimit("enum pattern coverage row is empty");
      const auto &cell = row.front();
      Row specialized;
      if (cell.variant) {
        if (*cell.variant != constructor.variant)
          continue;
        if (cell.payload.size() != constructor.fields.size())
          throw PatternAnalysisLimit(
              "enum pattern matrix payload differs from its constructor");
        specialized = cell.payload;
      } else {
        for (auto type : constructor.fields)
          specialized.push_back(wildcard(type));
      }
      specialized.insert(specialized.end(), row.begin() + 1, row.end());
      next.matrix.push_back(std::move(specialized));
    }
    if (useful(next.matrix, next.query, depth + 1))
      return true;
  }
  return false;
}

bool PatternCoverage::add(const Pattern &pattern) {
  Row query{pattern};
  if (!useful(matrix_, query, 0))
    return false;
  matrix_.push_back({pattern});
  return true;
}

bool PatternCoverage::exhaustive() {
  if (constructors(scrutinee_).empty())
    return true;
  Row query{wildcard(scrutinee_)};
  return !useful(matrix_, query, 0);
}
} // namespace lang::hir
