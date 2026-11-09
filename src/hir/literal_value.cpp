#include "literal_value.hpp"
#include "lang/support/source.hpp"
#include <algorithm>
#include <limits>
#include <string>

namespace lang::hir {
namespace {
bool integer_kind(TypeKind kind) {
  return kind == TypeKind::SignedInt || kind == TypeKind::UnsignedInt ||
         kind == TypeKind::Isize || kind == TypeKind::Usize;
}
bool signed_kind(TypeKind kind) {
  return kind == TypeKind::SignedInt || kind == TypeKind::Isize;
}

[[noreturn]] void bad(const std::string &message) {
  throw LiteralError(message);
}


std::string canonical_integer(const mpz_class &value) {
  return value.get_str();
}
} // namespace

uint64_t exact_ratio_to_ieee(bool negative, const mpz_class &numerator,
                     const mpz_class &denominator, unsigned width) {
  const uint64_t sign = uint64_t(negative) << (width - 1);
  const uint64_t infinity =
      width == 32 ? 0x7f800000ULL : 0x7ff0000000000000ULL;
  const int precision = width == 32 ? 24 : 53;
  const int bias = width == 32 ? 127 : 1023;
  if (numerator == 0)
    return sign;
  long e = long(mpz_sizeinbase(numerator.get_mpz_t(), 2)) -
           long(mpz_sizeinbase(denominator.get_mpz_t(), 2));
  if (e >= 0 ? numerator < (denominator << e)
             : (numerator << -e) < denominator)
    --e;
  if (e > bias)
    return sign | infinity;
  long scale = precision - 1 - std::max(e, long(1 - bias));
  mpz_class a = numerator, b = denominator, q, rem;
  if (scale >= 0)
    a <<= scale;
  else
    b <<= -scale;
  mpz_fdiv_qr(q.get_mpz_t(), rem.get_mpz_t(), a.get_mpz_t(), b.get_mpz_t());
  if (2 * rem > b || (2 * rem == b && mpz_odd_p(q.get_mpz_t())))
    ++q;
  if (q >= (mpz_class(1) << precision)) {
    q >>= 1;
    ++e;
  }
  if (e > bias)
    return sign | infinity;
  bool normal = q >= (mpz_class(1) << (precision - 1));
  uint64_t fraction = q.get_ui();
  if (normal)
    fraction -= uint64_t(1) << (precision - 1);
  return sign | fraction |
         (normal ? uint64_t(std::max(e, long(1 - bias)) + bias)
                       << (precision - 1)
                 : 0);
}


mpz_class parse_integer_spelling(std::string_view spelling) {
  std::string s(spelling);
  s.erase(std::remove(s.begin(), s.end(), '_'), s.end());
  if (s.empty())
    bad("empty integer literal");
  int base = 10;
  if (s.size() > 2 && s[0] == '0') {
    base = s[1] == 'x' || s[1] == 'X'   ? 16
           : s[1] == 'b' || s[1] == 'B' ? 2
           : s[1] == 'o' || s[1] == 'O' ? 8
                                        : 10;
    if (base != 10)
      s.erase(0, 2);
  }
  mpz_class value;
  if (s.empty() || value.set_str(s, base) != 0)
    bad("invalid integer literal spelling");
  return value;
}

uint64_t ieee_float_bits(std::string_view spelling, bool negative,
                         unsigned width) {
  if (width != 32 && width != 64)
    bad("floating literal has unsupported IEEE width");
  const uint64_t sign = uint64_t(negative) << (width - 1);
  const uint64_t infinity =
      width == 32 ? 0x7f800000ULL : 0x7ff0000000000000ULL;
  std::string s(spelling);
  s.erase(std::remove(s.begin(), s.end(), '_'), s.end());
  auto ep = s.find_first_of("eE");
  mpz_class decimal_exponent = 0;
  if (ep != std::string::npos) {
    const auto es = s.substr(ep + 1);
    if (es.empty() ||
        decimal_exponent.set_str(es[0] == '+' ? es.substr(1) : es, 10) != 0)
      bad("invalid floating literal exponent");
    s.resize(ep);
  }
  auto dot = s.find('.');
  if (dot != std::string::npos) {
    decimal_exponent -= s.size() - dot - 1;
    s.erase(dot, 1);
  }
  auto first = s.find_first_not_of('0');
  if (first == std::string::npos)
    return sign;
  s.erase(0, first);
  auto last = s.find_last_not_of('0');
  decimal_exponent += s.size() - last - 1;
  s.resize(last + 1);
  mpz_class magnitude = decimal_exponent + s.size() - 1;
  // Beyond these decimal magnitudes no f32/f64 rounding boundary remains.
  if (magnitude > 400)
    return sign | infinity;
  if (magnitude < -400)
    return sign;
  const long exponent = decimal_exponent.get_si();
  mpz_class numerator;
  numerator.set_str(s, 10);
  if (numerator == 0)
    return sign;
  mpz_class denominator = 1, power;
  mpz_ui_pow_ui(
      power.get_mpz_t(), 10,
      static_cast<unsigned long>(exponent < 0 ? -exponent : exponent));
  if (exponent >= 0)
    numerator *= power;
  else
    denominator = power;
  return exact_ratio_to_ieee(negative, numerator, denominator, width);
}

PatternLiteral decode_pattern_literal(const modules::Project &project,
                                      const TypeContext &types,
                                      modules::NodeRef node, TypeId expected,
                                      const Config &config) {
  const Type *target = types.find(expected);
  if (!target)
    bad("literal pattern type is missing");
  if (node.module.value >= project.modules.size())
    bad("literal pattern module is invalid");
  const auto &module = project.modules[node.module.value];
  if (module.id != node.module || node.file >= module.files.size() ||
      !module.files[node.file])
    bad("literal pattern file is invalid");
  const auto &file = *module.files[node.file];
  if (node.node.value >= file.nodes.size())
    bad("literal pattern source is invalid");
  const auto &syntax = file.nodes[node.node.value];
  if (syntax.kind != AstKind::LiteralPattern || syntax.recovered)
    bad("literal pattern source kind is not a literal");
  size_t first = syntax.token_begin;
  if (first >= syntax.token_end || syntax.token_end > file.tokens.size())
    bad("literal pattern token range is invalid");
  bool negative = false;
  if (file.tokens[first].spelling == "-") {
    negative = true;
    ++first;
  }
  if (first + 1 != syntax.token_end)
    bad("literal pattern has an unexpected token sequence");
  const auto &token = file.tokens[first];

  if (target->kind == TypeKind::Bool) {
    if (negative)
      bad("boolean literal pattern cannot be negative");
    if (token.spelling == "true")
      return PatternLiteral{BoolLiteral{true}};
    if (token.spelling == "false")
      return PatternLiteral{BoolLiteral{false}};
    bad("boolean literal pattern requires true or false");
  }

  if (integer_kind(target->kind)) {
    if (token.kind != TokenKind::integer)
      bad("integer literal pattern requires an integer token");
    mpz_class value = parse_integer_spelling(token.spelling);
    if (negative)
      value = -value;
    const unsigned width = bit_width(*target, config);
    if (width == 0 || width > 64)
      bad("integer literal pattern has an invalid target width");
    const mpz_class modulus = mpz_class(1) << width;
    const bool is_signed = signed_kind(target->kind);
    const mpz_class high =
        is_signed ? mpz_class(modulus >> 1) : modulus;
    const mpz_class low = is_signed ? mpz_class(-high) : mpz_class(0);
    if (value < low || value >= high)
      bad("integer constant is outside the target type range");
    mpz_class normalized;
    mpz_mod(normalized.get_mpz_t(), value.get_mpz_t(), modulus.get_mpz_t());
    uint64_t bits = 0;
    size_t count = 0;
    mpz_export(&bits, &count, -1, sizeof(bits), 0, 0,
               normalized.get_mpz_t());
    return PatternLiteral{IntegerLiteral{canonical_integer(value), bits}};
  }

  if (target->kind == TypeKind::Float) {
    if (token.kind != TokenKind::floating)
      bad("floating literal pattern requires a floating token");
    if (target->bits != 32 && target->bits != 64)
      bad("floating literal pattern has an invalid target width");
    return PatternLiteral{
        FloatLiteral{ieee_float_bits(token.spelling, negative, target->bits)}};
  }

  if (target->kind == TypeKind::String) {
    if (negative)
      bad("string literal pattern cannot be negative");
    if (token.kind != TokenKind::string_literal || !token.string_value)
      bad("string literal pattern requires a decoded string token");
    try {
      validate_utf8(*token.string_value);
    } catch (const SourceError &) {
      bad("invalid UTF-8 string literal");
    }
    return PatternLiteral{StringLiteral{*token.string_value}};
  }

  bad("literal pattern requires bool, integer, float, or string");
}
} // namespace lang::hir
