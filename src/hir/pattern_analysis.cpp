#include "pattern_analysis.hpp"
#include "lang/support/source.hpp"
#include <algorithm>
#include <set>
#include <type_traits>

namespace lang::hir {
namespace {
Pattern wildcard(TypeId type) {
  Pattern pattern;
  pattern.type = type;
  return pattern;
}
bool total(const Pattern &pattern) {
  return !pattern.variant && !pattern.literal;
}
bool integer(TypeKind kind) {
  return kind == TypeKind::SignedInt || kind == TypeKind::UnsignedInt ||
         kind == TypeKind::Isize || kind == TypeKind::Usize;
}
PatternConstructor literal_constructor(std::string key) {
  PatternConstructor constructor;
  constructor.kind = PatternConstructor::Kind::literal;
  constructor.literal_key = std::move(key);
  return constructor;
}
PatternConstructor other_constructor() {
  PatternConstructor constructor;
  constructor.kind = PatternConstructor::Kind::other;
  return constructor;
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
    constructor.kind = PatternConstructor::Kind::variant;
    constructor.variant = variant.symbol;
    for (const auto &field : variant.payload)
      constructor.fields.push_back(
          substitute(field.type, substitutions, types));
    constructors.push_back(std::move(constructor));
  }
  return constructors;
}

PatternDomain pattern_domain(const HirModule &module, TypeId type_id,
                             TypeContext &types) {
  const Type *type = types.find(type_id);
  if (!type)
    throw PatternAnalysisLimit("pattern coverage type is missing");
  PatternDomain result;
  switch (type->kind) {
  case TypeKind::NominalEnum:
    result.kind = PatternDomainKind::nominal_enum;
    result.constructors = pattern_constructors(module, type_id, types);
    return result;
  case TypeKind::Bool:
    result.kind = PatternDomainKind::bool_domain;
    result.bits = 1;
    return result;
  case TypeKind::SignedInt:
  case TypeKind::UnsignedInt:
  case TypeKind::Isize:
  case TypeKind::Usize:
    result.kind = PatternDomainKind::integer_domain;
    result.bits = bit_width(*type, module.config);
    return result;
  case TypeKind::Float:
    result.kind = PatternDomainKind::float_domain;
    result.bits = type->bits;
    return result;
  case TypeKind::String:
    result.kind = PatternDomainKind::string_domain;
    return result;
  default:
    result.kind = PatternDomainKind::opaque;
    return result;
  }
}

PatternCoverage::PatternCoverage(TypeId scrutinee, PatternDomainLookup domain,
                                 std::stop_token stop, size_t budget)
    : scrutinee_(scrutinee), lookup_(std::move(domain)),
      stop_(std::move(stop)), budget_(budget) {}

void PatternCoverage::spend(size_t amount) const {
  check_cancelled(stop_);
  if (amount > budget_)
    throw PatternAnalysisLimit(
        "switch pattern coverage analysis exceeded its resource budget");
  budget_ -= amount;
}

const PatternDomain &PatternCoverage::domain(TypeId type) {
  auto found = cache_.find(type.value);
  if (found != cache_.end())
    return found->second;
  if (!lookup_)
    throw PatternAnalysisLimit("switch pattern coverage has no type view");
  auto entries = lookup_(type);
  spend(entries.constructors.size() + 1);
  return cache_.emplace(type.value, std::move(entries)).first->second;
}

std::string PatternCoverage::literal_key(const Pattern &pattern,
                                         const PatternDomain &d) {
  if (!pattern.literal)
    throw PatternAnalysisLimit("missing literal pattern value");
  return std::visit(
      [&](const auto &value) -> std::string {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, BoolLiteral>) {
          if (d.kind != PatternDomainKind::bool_domain)
            throw PatternAnalysisLimit("bool literal pattern type mismatch");
          return std::string("b:") + (value.value ? "1" : "0");
        } else if constexpr (std::is_same_v<T, IntegerLiteral>) {
          if (d.kind != PatternDomainKind::integer_domain)
            throw PatternAnalysisLimit("integer literal pattern type mismatch");
          return "i:" + std::to_string(d.bits) + ":" +
                 std::to_string(value.bits);
        } else if constexpr (std::is_same_v<T, FloatLiteral>) {
          if (d.kind != PatternDomainKind::float_domain)
            throw PatternAnalysisLimit("float literal pattern type mismatch");
          uint64_t bits = value.bits;
          if (d.bits == 32) {
            bits &= 0xffffffffULL;
            if ((bits & 0x80000000ULL) && !(bits & 0x7fffffffULL))
              bits = 0;
          } else if (d.bits == 64) {
            if ((bits & 0x8000000000000000ULL) &&
                !(bits & 0x7fffffffffffffffULL))
              bits = 0;
          }
          return "f:" + std::to_string(d.bits) + ":" +
                 std::to_string(bits);
        } else {
          if (d.kind != PatternDomainKind::string_domain)
            throw PatternAnalysisLimit("string literal pattern type mismatch");
          return "s:" + std::to_string(value.utf8.size()) + ":" + value.utf8;
        }
      },
      *pattern.literal);
}

std::vector<PatternConstructor>
PatternCoverage::constructors(TypeId type, const std::vector<Row> &matrix,
                              const Row &query) {
  const auto &d = domain(type);
  spend(matrix.size() + query.size() + 1);
  auto collect_literals = [&] {
    std::set<std::string> keys;
    auto add = [&](const Pattern &cell) {
      if (cell.variant)
        throw PatternAnalysisLimit(
            "enum variant pattern outside an enum domain");
      if (cell.type != type)
        throw PatternAnalysisLimit("pattern coverage column type mismatch");
      if (cell.literal)
        keys.insert(literal_key(cell, d));
    };
    for (const auto &row : matrix)
      if (!row.empty())
        add(row.front());
    if (!query.empty())
      add(query.front());
    return keys;
  };
  std::vector<PatternConstructor> result;
  switch (d.kind) {
  case PatternDomainKind::nominal_enum:
    return d.constructors;
  case PatternDomainKind::bool_domain:
    return {literal_constructor("b:0"), literal_constructor("b:1")};
  case PatternDomainKind::integer_domain: {
    auto keys = collect_literals();
    bool full = false;
    if (d.bits < sizeof(size_t) * 8) {
      const size_t domain_size = size_t{1} << d.bits;
      full = keys.size() == domain_size;
    }
    for (const auto &key : keys)
      result.push_back(literal_constructor(key));
    if (!full)
      result.push_back(other_constructor());
    return result;
  }
  case PatternDomainKind::float_domain:
  case PatternDomainKind::string_domain: {
    auto keys = collect_literals();
    for (const auto &key : keys)
      result.push_back(literal_constructor(key));
    result.push_back(other_constructor());
    return result;
  }
  case PatternDomainKind::opaque:
    return {other_constructor()};
  }
  throw PatternAnalysisLimit("unknown pattern domain");
}

std::vector<std::vector<Pattern>>
PatternCoverage::specialize(const PatternConstructor &constructor,
                            const std::vector<Row> &matrix) {
  std::vector<Row> result;
  for (const auto &row : matrix) {
    if (row.empty())
      throw PatternAnalysisLimit("switch pattern coverage row is empty");
    const auto &cell = row.front();
    if (cell.variant && cell.literal)
      throw PatternAnalysisLimit("pattern has both variant and literal");
    Row specialized;
    if (total(cell)) {
      for (auto field : constructor.fields)
        specialized.push_back(wildcard(field));
    } else if (cell.variant) {
      if (constructor.kind != PatternConstructor::Kind::variant ||
          *cell.variant != constructor.variant)
        continue;
      if (cell.payload.size() != constructor.fields.size())
        throw PatternAnalysisLimit(
            "enum pattern matrix payload differs from its constructor");
      specialized = cell.payload;
    } else {
      if (!cell.literal)
        throw PatternAnalysisLimit("unknown pattern column category");
      if (constructor.kind != PatternConstructor::Kind::literal)
        continue;
      const auto &d = domain(cell.type);
      if (literal_key(cell, d) != constructor.literal_key)
        continue;
      if (!cell.payload.empty() || cell.binding)
        throw PatternAnalysisLimit("literal pattern has payload or binding");
    }
    specialized.insert(specialized.end(), row.begin() + 1, row.end());
    result.push_back(std::move(specialized));
  }
  return result;
}

bool PatternCoverage::useful(const std::vector<Row> &matrix, const Row &query,
                             size_t depth) {
  check_cancelled(stop_);
  if (depth > 256)
    throw PatternAnalysisLimit(
        "switch pattern coverage analysis exceeded its nesting budget");
  spend(matrix.size() + query.size() + 1);
  if (query.empty())
    return matrix.empty();
  // A row made only of wildcard/binding patterns covers every tuple in this
  // matrix, so no query can add a new value. Besides being correct, this keeps
  // a final `case _` from expanding every nested constructor combination.
  for (const auto &row : matrix)
    if (std::all_of(row.begin(), row.end(), total))
      return false;

  const auto &head = query.front();
  if (head.variant && head.literal)
    throw PatternAnalysisLimit("pattern has both variant and literal");
  if (head.variant || head.literal) {
    const auto &all = constructors(head.type, matrix, query);
    auto constructor = all.end();
    if (head.variant) {
      if (head.binding || head.literal)
        throw PatternAnalysisLimit("variant pattern has invalid fields");
      constructor = std::find_if(
          all.begin(), all.end(), [&](const PatternConstructor &c) {
            return c.kind == PatternConstructor::Kind::variant &&
                   c.variant == *head.variant;
          });
    } else {
      if (head.binding || !head.payload.empty())
        throw PatternAnalysisLimit("literal pattern has invalid fields");
      const auto &d = domain(head.type);
      const auto key = literal_key(head, d);
      constructor = std::find_if(
          all.begin(), all.end(), [&](const PatternConstructor &c) {
            return c.kind == PatternConstructor::Kind::literal &&
                   c.literal_key == key;
          });
    }
    if (constructor == all.end())
      throw PatternAnalysisLimit(
          "pattern constructor is absent from its declared type");
    Row next_query;
    if (head.variant) {
      if (head.payload.size() != constructor->fields.size())
        throw PatternAnalysisLimit(
            "enum pattern payload count differs from its constructor");
      next_query = head.payload;
    }
    next_query.insert(next_query.end(), query.begin() + 1, query.end());
    auto next_matrix = specialize(*constructor, matrix);
    return useful(next_matrix, next_query, depth + 1);
  }

  // A binding and a wildcard are both total at their position. Non-enum
  // domains must retain an internal Other class; an actual empty enum has no
  // values at all and is handled as uninhabited rather than as a total
  // wildcard.
  const auto &all = constructors(head.type, matrix, query);
  if (all.empty())
    return matrix.empty();
  if (matrix.empty())
    return true;
  Row tail(query.begin() + 1, query.end());
  for (const auto &constructor : all) {
    Row next_query;
    for (auto type : constructor.fields)
      next_query.push_back(wildcard(type));
    next_query.insert(next_query.end(), tail.begin(), tail.end());
    auto next_matrix = specialize(constructor, matrix);
    if (useful(next_matrix, next_query, depth + 1))
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
  const auto &d = domain(scrutinee_);
  if (d.kind == PatternDomainKind::nominal_enum && d.constructors.empty())
    return true;
  Row query{wildcard(scrutinee_)};
  return !useful(matrix_, query, 0);
}
} // namespace lang::hir
