#pragma once
#include "lang/hir/hir.hpp"
#include <gmpxx.h>

namespace lang::hir {
// Internal, source-only literal decoding shared by the checker and the
// independent HIR verifier. This header deliberately has no LLVM or Python
// dependency. It never reads producer-provided literal data.
struct LiteralError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// Parse the spelling of one integer token (without a leading sign). Underscores
// and the source radix prefixes use exactly the checker/lexer rules.
mpz_class parse_integer_spelling(std::string_view spelling);

// One exact rational-to-IEEE RNE decision for an already-known target width.
// Callers supply exact integer components; there is no host floating point
// intermediate. This is shared by decimal literal parsing and constant float
// arithmetic.
uint64_t exact_ratio_to_ieee(bool negative, const mpz_class &numerator,
                             const mpz_class &denominator, unsigned width);

// Correctly round a decimal floating token to the requested IEEE width. An
// explicit source minus is applied to the sign bit, including signed zero and
// infinity.
uint64_t ieee_float_bits(std::string_view spelling, bool negative,
                         unsigned width);

// Re-read a LiteralPattern from its real AST token range and produce the
// canonical value for the expected concrete type. Throws LiteralError when the
// source form or target type is invalid.
PatternLiteral decode_pattern_literal(const modules::Project &project,
                                      const TypeContext &types,
                                      modules::NodeRef node, TypeId expected,
                                      const Config &config);
} // namespace lang::hir
