// Test-only reference oracle for the self-hosted number module.
//
// This program is never linked into or called by the compiler. It exists so
// the acceptance run can compare the Woozy implementation against an
// independent exact implementation built on GMP/MPFR: MPFR parses decimal
// literals and rounds exact rationals/integers once at the destination
// precision, and GMP holds the intermediate integers exactly.
//
// Protocol (stdin, one request per line, answers on stdout in the same order):
//   L <width> <negative01> <spelling>          decimal spelling -> IEEE bits
//   R <width> <negative01> <num> <den>         num/den -> IEEE bits
//   N <width> <value>                         integer -> IEEE bits
//   C <from_width> <to_width> <bits>          already-rounded IEEE -> IEEE
// Invalid requests answer "error"; the harness only sends requests whose
// arguments are in the domain of the frozen API.
#include <gmp.h>
#include <mpfr.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {
uint64_t double_bits(double value) {
  uint64_t out = 0;
  std::memcpy(&out, &value, sizeof(out));
  return out;
}
uint32_t float_bits(float value) {
  uint32_t out = 0;
  std::memcpy(&out, &value, sizeof(out));
  return out;
}
uint64_t infinity_bits(unsigned width) {
  return width == 32 ? 0x7f800000ULL : 0x7ff0000000000000ULL;
}
uint64_t nan_bits(unsigned width) {
  return width == 32 ? 0x7fc00000ULL : 0x7ff8000000000000ULL;
}
bool float_width(unsigned width) { return width == 32 || width == 64; }

uint64_t encode_mpfr(const mpfr_t value, unsigned width, bool negative) {
  uint64_t bits = width == 32 ? uint64_t(float_bits(mpfr_get_flt(value, MPFR_RNDN)))
                              : double_bits(mpfr_get_d(value, MPFR_RNDN));
  if (negative) bits |= uint64_t(1) << (width - 1);
  return bits;
}

struct Decoded {
  bool finite = true;
  bool infinity = false;
  bool nan = false;
  bool negative = false;
  mpz_t significand;
  long shift = 0;
  Decoded() { mpz_init(significand); }
  ~Decoded() { mpz_clear(significand); }
};

void decode(uint64_t bits, unsigned width, Decoded &out) {
  const unsigned fraction_bits = width == 32 ? 23 : 52;
  const unsigned exponent_bits = width == 32 ? 8 : 11;
  const long bias = width == 32 ? 127 : 1023;
  out.negative = ((bits >> (width - 1)) & 1) != 0;
  const uint64_t exponent_mask = (uint64_t(1) << exponent_bits) - 1;
  const uint64_t fraction_mask = (uint64_t(1) << fraction_bits) - 1;
  const uint64_t exponent = (bits >> fraction_bits) & exponent_mask;
  const uint64_t fraction = bits & fraction_mask;
  if (exponent == exponent_mask) {
    out.finite = false;
    out.nan = fraction != 0;
    out.infinity = fraction == 0;
    return;
  }
  out.shift = exponent == 0
                  ? long(1 - bias - long(fraction_bits))
                  : long(exponent) - bias - long(fraction_bits);
  if (exponent == 0) {
    mpz_set_ui(out.significand, fraction);
  } else {
    mpz_set_ui(out.significand,
               (uint64_t(1) << fraction_bits) | fraction);
  }
}

// Exact decoded magnitude as a positive rational.
void decoded_value(const Decoded &value, mpq_t out) {
  mpq_set_z(out, value.significand);
  if (value.shift > 0)
    mpq_mul_2exp(out, out, static_cast<mp_bitcnt_t>(value.shift));
  else if (value.shift < 0)
    mpq_div_2exp(out, out, static_cast<mp_bitcnt_t>(-value.shift));
}

std::string answer_ratio(unsigned width, bool negative, const char *num,
                         const char *den) {
  mpz_t n, d;
  mpz_inits(n, d, nullptr);
  if (mpz_set_str(n, num, 10) != 0 || mpz_set_str(d, den, 10) != 0) {
    mpz_clears(n, d, nullptr);
    return "error";
  }
  if (mpz_sgn(n) < 0 || mpz_sgn(d) <= 0) {
    mpz_clears(n, d, nullptr);
    return "error";
  }
  mpq_t q;
  mpq_init(q);
  mpq_set_num(q, n);
  mpq_set_den(q, d);
  mpq_canonicalize(q);
  const mpfr_prec_t precision =
      static_cast<mpfr_prec_t>(mpz_sizeinbase(n, 2) +
                               mpz_sizeinbase(d, 2) + 320);
  mpfr_t value;
  mpfr_init2(value, precision);
  mpfr_set_q(value, q, MPFR_RNDN);
  const uint64_t bits = encode_mpfr(value, width, negative);
  mpfr_clear(value);
  mpq_clear(q);
  mpz_clears(n, d, nullptr);
  return std::to_string(bits);
}

std::string answer_literal(unsigned width, bool negative, const char *spelling) {
  const std::string text(spelling);
  const mpfr_prec_t precision =
      static_cast<mpfr_prec_t>(8 * text.size() + 320);
  mpfr_t value;
  mpfr_init2(value, precision);
  if (mpfr_set_str(value, text.c_str(), 10, MPFR_RNDN) != 0) {
    mpfr_clear(value);
    return "error";
  }
  const uint64_t bits = encode_mpfr(value, width, negative);
  mpfr_clear(value);
  return std::to_string(bits);
}

std::string answer_integer(unsigned width, const char *spelling) {
  mpz_t value;
  mpz_init(value);
  if (mpz_set_str(value, spelling, 10) != 0) {
    mpz_clear(value);
    return "error";
  }
  const mpfr_prec_t precision =
      static_cast<mpfr_prec_t>(mpz_sizeinbase(value, 2) + 320);
  mpfr_t rounded;
  mpfr_init2(rounded, precision);
  mpfr_set_z(rounded, value, MPFR_RNDN);
  const uint64_t bits = encode_mpfr(rounded, width, mpz_sgn(value) < 0);
  mpfr_clear(rounded);
  mpz_clear(value);
  return std::to_string(bits);
}

std::string answer_convert(unsigned from_width, unsigned to_width,
                           uint64_t bits) {
  if (from_width == to_width) return std::to_string(bits);
  Decoded source;
  decode(bits, from_width, source);
  if (source.nan) return std::to_string(nan_bits(to_width));
  if (source.infinity) {
    const uint64_t sign = source.negative ? (uint64_t(1) << (to_width - 1)) : 0;
    return std::to_string(sign | infinity_bits(to_width));
  }
  if (mpz_sgn(source.significand) == 0) {
    const uint64_t sign = source.negative ? (uint64_t(1) << (to_width - 1)) : 0;
    return std::to_string(sign);
  }
  mpq_t exact;
  mpq_init(exact);
  decoded_value(source, exact);
  const mpfr_prec_t precision =
      static_cast<mpfr_prec_t>(mpz_sizeinbase(mpq_numref(exact), 2) + 320);
  mpfr_t rounded;
  mpfr_init2(rounded, precision);
  mpfr_set_q(rounded, exact, MPFR_RNDN);
  const uint64_t result = encode_mpfr(rounded, to_width, source.negative);
  mpfr_clear(rounded);
  mpq_clear(exact);
  return std::to_string(result);
}

bool self_check() {
  struct LiteralCheck {
    unsigned width;
    bool negative;
    const char *spelling;
    uint64_t expected;
  };
  const LiteralCheck literals[] = {
      {64, false, "0.1", 4591870180066957722ULL},
      {64, false, "5e-324", 1ULL},
      {64, false, "1e-323", 2ULL},
      {64, false, "2.4703282292062327e-324", 0ULL},
      {64, false, "1.7976931348623157e308", 0x7fefffffffffffffULL},
      {64, false, "1e400", 0x7ff0000000000000ULL},
      {64, true, "0.0", 0x8000000000000000ULL},
      {32, false, "1e-45", 1ULL},
      {32, false, "1.1754943508222875e-38", 0x00800000ULL},
      {32, false, "3.4028235e38", 0x7f7fffffULL},
      {32, false, "3.4028236e38", 0x7f800000ULL},
  };
  for (const auto &check : literals) {
    const std::string got =
        answer_literal(check.width, check.negative, check.spelling);
    if (got != std::to_string(check.expected)) {
      std::fprintf(stderr, "oracle literal self-check failed: %s -> %s\n",
                   check.spelling, got.c_str());
      return false;
    }
  }
  struct RatioCheck {
    unsigned width;
    bool negative;
    const char *num;
    const char *den;
    uint64_t expected;
  };
  const RatioCheck ratios[] = {
      {64, false, "1", "3", 4599676419421066581ULL},
      {64, false, "1", "1", 0x3ff0000000000000ULL},
      {32, false, "1", "3", 0x3eaaaaabULL},
  };
  for (const auto &check : ratios) {
    const std::string got =
        answer_ratio(check.width, check.negative, check.num, check.den);
    if (got != std::to_string(check.expected)) {
      std::fprintf(stderr, "oracle ratio self-check failed: %s/%s -> %s\n",
                   check.num, check.den, got.c_str());
      return false;
    }
  }
  // 2^53 + 1 is exactly between two binary64 values; ties-to-even keeps 2^53.
  mpz_t odd;
  mpz_init(odd);
  mpz_ui_pow_ui(odd, 2, 53);
  mpz_add_ui(odd, odd, 1);
  const size_t needed = mpz_sizeinbase(odd, 10) + 2;
  std::vector<char> text(needed, '\0');
  mpz_get_str(text.data(), 10, odd);
  const std::string rounded = answer_integer(64, text.data());
  mpz_clear(odd);
  if (rounded != std::to_string(0x4340000000000000ULL)) {
    std::fprintf(stderr, "oracle integer self-check failed: %s\n",
                 rounded.c_str());
    return false;
  }
  // 2^-1074 is the least binary64 subnormal, 2^-1075 ties to even zero.
  mpz_t pow;
  mpz_init(pow);
  mpz_ui_pow_ui(pow, 2, 1074);
  std::vector<char> den(mpz_sizeinbase(pow, 10) + 2, '\0');
  mpz_get_str(den.data(), 10, pow);
  mpz_clear(pow);
  if (answer_ratio(64, false, "1", den.data()) != "1") {
    std::fprintf(stderr, "oracle subnormal self-check failed\n");
    return false;
  }
  mpz_t tie;
  mpz_init(tie);
  mpz_ui_pow_ui(tie, 2, 1075);
  std::vector<char> tie_den(mpz_sizeinbase(tie, 10) + 2, '\0');
  mpz_get_str(tie_den.data(), 10, tie);
  mpz_clear(tie);
  if (answer_ratio(64, false, "1", tie_den.data()) != "0") {
    std::fprintf(stderr, "oracle subnormal tie self-check failed\n");
    return false;
  }
  return true;
}
} // namespace

int main() {
  if (!self_check()) return 1;
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) continue;
    std::istringstream stream(line);
    std::string op;
    stream >> op;
    std::string result = "error";
    if (op == "L") {
      unsigned width = 0;
      int negative = 0;
      std::string spelling;
      stream >> width >> negative >> spelling;
      if (float_width(width) && !spelling.empty())
        result = answer_literal(width, negative != 0, spelling.c_str());
    } else if (op == "R") {
      unsigned width = 0;
      int negative = 0;
      std::string num, den;
      stream >> width >> negative >> num >> den;
      if (float_width(width))
        result = answer_ratio(width, negative != 0, num.c_str(), den.c_str());
    } else if (op == "N") {
      unsigned width = 0;
      std::string value;
      stream >> width >> value;
      if (float_width(width))
        result = answer_integer(width, value.c_str());
    } else if (op == "C") {
      unsigned from_width = 0, to_width = 0;
      std::string bits;
      stream >> from_width >> to_width >> bits;
      if (float_width(from_width) && float_width(to_width)) {
        try {
          const uint64_t pattern = std::stoull(bits);
          result = answer_convert(from_width, to_width, pattern);
        } catch (...) {
          result = "error";
        }
      }
    }
    std::cout << result << "\n";
  }
  return 0;
}
