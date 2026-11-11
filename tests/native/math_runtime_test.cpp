// Host acceptance harness for runtime/native/math/.
//
// The runtime math objects are freestanding and expose only lang_math_* names;
// this harness uses the host C++ library as the independent oracle.  Build the
// static mode with -DLANG_MATH_FREESTANDING and link the runtime objects plus
// start.cpp for the no-libc ELF smoke.
//
// Host build:
//   <cxx> -std=c++20 -O2 -I runtime/native tests/native/math_runtime_test.cpp
//         <compiled math C objects> runtime/native/math/support.cpp
//         -o math_runtime_test

#include "../../runtime/native/math.hpp"

#ifdef LANG_MATH_FREESTANDING

namespace {
uint64_t bits_of(double value) {
  uint64_t bits;
  __builtin_memcpy(&bits, &value, sizeof(bits));
  return bits;
}
} // namespace

extern "C" int main() {
  if (bits_of(lang_math_sqrt_v1(2.0)) != UINT64_C(0x3ff6a09e667f3bcd))
    return 1;
  if (bits_of(lang_math_sin_v1(1.0)) != UINT64_C(0x3feaed548f090cee))
    return 2;
  if (bits_of(lang_math_cos_v1(1.0)) != UINT64_C(0x3fe14a280fb5068c))
    return 3;
  if (bits_of(lang_math_atan_v1(1.0)) != UINT64_C(0x3fe921fb54442d18))
    return 4;
  if (bits_of(lang_math_exp_v1(1.0)) != UINT64_C(0x4005bf0a8b145769))
    return 5;
  if (bits_of(lang_math_log_v1(2.0)) != UINT64_C(0x3fe62e42fefa39ef))
    return 6;
  if (lang_math_pow_v1(2.0, 10.0) != 1024.0) return 7;
  if (lang_math_hypot_v1(3.0, 4.0) != 5.0) return 8;
  if (lang_math_fma_v1(1.0 + 0x1p-52, 1.0 - 0x1p-52, -1.0) !=
      -0x1p-104) return 9;
  const double maximum = 0x1.fffffffffffffp+1023;
  if (lang_math_fma_v1(maximum, 2.0, -maximum) != maximum) return 10;
  if (lang_math_fma_v1(0x1p-537, 0x1p-537, 0.0) != 0x1p-1074) return 11;
  if (lang_math_ldexp_v1(0.5, 4) != 8.0) return 12;
  int32_t exponent = 0;
  const double mantissa = lang_math_frexp_v1(8.0, &exponent);
  if (mantissa != 0.5 || exponent != 4) return 13;
  if (!lang_math_isnan_v1(__builtin_nan(""))) return 14;
  if (!lang_math_isinf_v1(__builtin_inf())) return 15;
  if (!lang_math_isfinite_v1(1.0)) return 16;
  if (!lang_math_signbit_v1(-0.0)) return 17;
  if (lang_math_fabs_v1(-2.5) != 2.5) return 18;
  if (lang_math_copysign_v1(2.5, -1.0) != -2.5) return 19;
  if (lang_math_floor_v1(1.9) != 1.0) return 20;
  if (lang_math_ceil_v1(1.1) != 2.0) return 21;
  if (lang_math_trunc_v1(-1.9) != -1.0) return 22;
  if (lang_math_round_v1(2.5) != 3.0) return 23;
  if (bits_of(lang_math_tan_v1(1.0)) != UINT64_C(0x3ff8eb245cbee3a6))
    return 24;
  if (bits_of(lang_math_asin_v1(0.5)) != UINT64_C(0x3fe0c152382d7366))
    return 25;
  if (bits_of(lang_math_acos_v1(0.5)) != UINT64_C(0x3ff0c152382d7366))
    return 26;
  if (lang_math_atan2_v1(1.0, 1.0) != lang_math_atan_v1(1.0)) return 27;
  if (lang_math_exp2_v1(10.0) != 1024.0) return 28;
  if (bits_of(lang_math_expm1_v1(1.0)) != UINT64_C(0x3ffb7e151628aed2))
    return 29;
  if (lang_math_log2_v1(8.0) != 3.0) return 30;
  if (lang_math_log10_v1(100.0) != 2.0) return 31;
  if (bits_of(lang_math_log1p_v1(1.0)) != UINT64_C(0x3fe62e42fefa39ef))
    return 32;
  if (lang_math_fmod_v1(10.0, 3.0) != 1.0) return 33;
  if (lang_math_remainder_v1(10.0, 3.0) != 1.0) return 34;
  return 0;
}

#else

#include <bit>
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace {

int failures = 0;
int shown = 0;

uint64_t next_random() {
  static uint64_t state = UINT64_C(0x9e3779b97f4a7c15);
  state ^= state << 13;
  state ^= state >> 7;
  state ^= state << 17;
  return state;
}

uint64_t bits_of(double value) { return std::bit_cast<uint64_t>(value); }

double from_bits(uint64_t bits) { return std::bit_cast<double>(bits); }

double random_finite() {
  uint64_t raw = next_random();
  raw &= ~(UINT64_C(0x7ff) << 52);
  raw |= (next_random() % 2047) << 52;
  return from_bits(raw);
}

double random_positive() {
  for (;;) {
    uint64_t raw = next_random() & ~(UINT64_C(1) << 63);
    raw &= ~(UINT64_C(0x7ff) << 52);
    raw |= (next_random() % 2047) << 52;
    const double value = from_bits(raw);
    if (std::isfinite(value) && value > 0) return value;
  }
}

double random_unit() {
  return static_cast<double>(next_random() >> 11) * 0x1p-53 * 2.0 - 1.0;
}

double random_01() {
  return static_cast<double>(next_random() >> 11) * 0x1p-53;
}

double random_uniform(double low, double high) {
  return low + random_01() * (high - low);
}

uint64_t ordered_bits(double value) {
  const uint64_t bits = bits_of(value);
  return (bits >> 63) ? ~bits : (bits | (UINT64_C(1) << 63));
}

uint64_t ulp_distance(double left, double right) {
  if (std::isnan(left) && std::isnan(right)) return 0;
  if (std::isnan(left) || std::isnan(right)) return UINT64_MAX;
  const uint64_t a = ordered_bits(left);
  const uint64_t b = ordered_bits(right);
  return a > b ? a - b : b - a;
}

void fail(const char *name, double actual, double expected, uint64_t distance,
          uint64_t tolerance) {
  ++failures;
  if (shown++ < 20)
    std::fprintf(stderr,
                 "FAIL %s: actual=%a (%016llx) expected=%a (%016llx) "
                 "ulp=%llu tolerance=%llu\n",
                 name, actual,
                 static_cast<unsigned long long>(bits_of(actual)), expected,
                 static_cast<unsigned long long>(bits_of(expected)),
                 static_cast<unsigned long long>(distance),
                 static_cast<unsigned long long>(tolerance));
}

void expect_ulp(const char *name, double actual, double expected,
                uint64_t tolerance) {
  const uint64_t distance = ulp_distance(actual, expected);
  if (distance > tolerance) fail(name, actual, expected, distance, tolerance);
}

using Unary = double (*)(double);

void random_unary(const char *name, Unary ours, Unary host, double (*gen)(),
                  uint64_t tolerance) {
  for (int i = 0; i < 10000; ++i) {
    const double input = gen();
    expect_ulp(name, ours(input), host(input), tolerance);
  }
}

void exact_unary_cases() {
  const double values[] = {
      0.0, -0.0, 0.5, -0.5, 1.5, -1.5, 2.5, -2.5, 1.0, -1.0,
      3.0, -3.0, std::numeric_limits<double>::min(),
      std::numeric_limits<double>::denorm_min(),
      std::numeric_limits<double>::max(),
      std::numeric_limits<double>::infinity(),
      -std::numeric_limits<double>::infinity(),
      std::numeric_limits<double>::quiet_NaN(), 0x1p52 + 0.5,
      -0x1p52 - 0.5};
  for (double x : values) {
    expect_ulp("fabs", lang_math_fabs_v1(x), std::fabs(x), 0);
    expect_ulp("floor", lang_math_floor_v1(x), std::floor(x), 0);
    expect_ulp("ceil", lang_math_ceil_v1(x), std::ceil(x), 0);
    expect_ulp("trunc", lang_math_trunc_v1(x), std::trunc(x), 0);
    expect_ulp("round", lang_math_round_v1(x), std::round(x), 0);
  }
  for (double x : values) {
    expect_ulp("copysign+", lang_math_copysign_v1(x, 1.0),
               std::copysign(x, 1.0), 0);
    expect_ulp("copysign-", lang_math_copysign_v1(x, -1.0),
               std::copysign(x, -1.0), 0);
  }
  const double nan = std::numeric_limits<double>::quiet_NaN();
  expect_ulp("copysign-nan", lang_math_copysign_v1(nan, -1.0),
             std::copysign(nan, -1.0), 0);

  random_unary("fabs", lang_math_fabs_v1, std::fabs, random_finite, 0);
  random_unary("floor", lang_math_floor_v1, std::floor, random_finite, 0);
  random_unary("ceil", lang_math_ceil_v1, std::ceil, random_finite, 0);
  random_unary("trunc", lang_math_trunc_v1, std::trunc, random_finite, 0);
  random_unary("round", lang_math_round_v1, std::round, random_finite, 0);
}

void sqrt_and_hypot_cases() {
  const double values[] = {0.0, -0.0, 1.0, 2.0, 4.0,
                           std::numeric_limits<double>::min(),
                           std::numeric_limits<double>::denorm_min(),
                           std::numeric_limits<double>::max(),
                           std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN(), -1.0};
  for (double x : values)
    expect_ulp("sqrt", lang_math_sqrt_v1(x), std::sqrt(x), 0);
  for (int i = 0; i < 10000; ++i) {
    const double x = random_positive();
    expect_ulp("sqrt", lang_math_sqrt_v1(x), std::sqrt(x), 0);
  }

  const double hypot_edges[][2] = {
      {3.0, 4.0}, {0.0, -0.0}, {-0.0, -0.0},
      {std::numeric_limits<double>::infinity(),
       std::numeric_limits<double>::quiet_NaN()},
      {std::numeric_limits<double>::quiet_NaN(),
       std::numeric_limits<double>::infinity()},
      {std::numeric_limits<double>::quiet_NaN(), 1.0},
      {std::numeric_limits<double>::max(),
       std::numeric_limits<double>::max()},
      {std::numeric_limits<double>::denorm_min(),
       std::numeric_limits<double>::denorm_min()}};
  for (const auto &item : hypot_edges)
    expect_ulp("hypot", lang_math_hypot_v1(item[0], item[1]),
               std::hypot(item[0], item[1]), 2);
  for (int i = 0; i < 10000; ++i) {
    const double x = random_finite();
    const double y = random_finite();
    expect_ulp("hypot", lang_math_hypot_v1(x, y), std::hypot(x, y), 2);
  }
}

void fma_cases() {
  const double maximum = std::numeric_limits<double>::max();
  const double minimum = std::numeric_limits<double>::denorm_min();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  const double cases[][3] = {
      {1.0, 2.0, 3.0},
      {0.0, 0.0, -0.0},
      {1.0 + 0x1p-52, 1.0 - 0x1p-52, -1.0},
      {0x1p-537, 0x1p-537, 0.0},
      {0x1p-538, 0x1p-537, 0.0},
      {3.0 * 0x1p-538, 0x1p-537, 0.0},
      {maximum, 2.0, -maximum},
      {maximum, maximum, -maximum},
      {1e308, 1e-308, -1.0},
      {inf, 0.0, nan},
      {inf, 1.0, -inf},
      {0.0, inf, 1.0},
      {1.0, inf, -inf},
      {nan, 1.0, 2.0},
      {minimum, 1.0, minimum},
      {minimum, 0.5, 0.0}};
  for (const auto &item : cases)
    expect_ulp("fma", lang_math_fma_v1(item[0], item[1], item[2]),
               std::fma(item[0], item[1], item[2]), 0);
  for (int i = 0; i < 10000; ++i) {
    const double x = random_finite();
    const double y = random_finite();
    const double z = random_finite();
    expect_ulp("fma", lang_math_fma_v1(x, y, z), std::fma(x, y, z), 0);
  }
}

void trigonometric_cases() {
  const double values[] = {
      0.0, -0.0, std::numeric_limits<double>::denorm_min(),
      std::numeric_limits<double>::min(), 0x1p-27,
      0x1.921fb54442d18p-1, 1.0, 2.0, 1e6, 0x1p20, 0x1p100,
      0x1p500, 0x1p1000, std::numeric_limits<double>::max(),
      -std::numeric_limits<double>::max(),
      std::numeric_limits<double>::infinity(),
      -std::numeric_limits<double>::infinity(),
      std::numeric_limits<double>::quiet_NaN()};
  for (double x : values) {
    expect_ulp("sin", lang_math_sin_v1(x), std::sin(x), 2);
    expect_ulp("cos", lang_math_cos_v1(x), std::cos(x), 2);
    expect_ulp("tan", lang_math_tan_v1(x), std::tan(x), 2);
  }
  random_unary("sin", lang_math_sin_v1, std::sin, random_finite, 2);
  random_unary("cos", lang_math_cos_v1, std::cos, random_finite, 2);
  random_unary("tan", lang_math_tan_v1, std::tan, random_finite, 2);
}

void inverse_trigonometric_cases() {
  const double unit[] = {-1.0, -0.5, -0.0, 0.0, 0.5, 1.0,
                         std::nextafter(1.0, 0.0),
                         -std::nextafter(1.0, 0.0), 1.0000000000000002,
                         -1.0000000000000002,
                         std::numeric_limits<double>::denorm_min(),
                         std::numeric_limits<double>::infinity(),
                         std::numeric_limits<double>::quiet_NaN()};
  for (double x : unit) {
    expect_ulp("asin", lang_math_asin_v1(x), std::asin(x), 2);
    expect_ulp("acos", lang_math_acos_v1(x), std::acos(x), 2);
  }
  for (int i = 0; i < 10000; ++i) {
    const double x = random_unit();
    expect_ulp("asin", lang_math_asin_v1(x), std::asin(x), 2);
    expect_ulp("acos", lang_math_acos_v1(x), std::acos(x), 2);
  }
  const double atan_values[] = {
      0.0, -0.0, 1.0, -1.0, 1e300, -1e300,
      std::numeric_limits<double>::max(),
      -std::numeric_limits<double>::max(),
      std::numeric_limits<double>::infinity(),
      -std::numeric_limits<double>::infinity(),
      std::numeric_limits<double>::quiet_NaN()};
  for (double x : atan_values)
    expect_ulp("atan", lang_math_atan_v1(x), std::atan(x), 2);
  random_unary("atan", lang_math_atan_v1, std::atan, random_finite, 2);

  const double specials[] = {
      0.0, -0.0, 1.0, -1.0, 2.0, -2.0, 0x1p1000, -0x1p1000,
      std::numeric_limits<double>::infinity(),
      -std::numeric_limits<double>::infinity(),
      std::numeric_limits<double>::quiet_NaN()};
  for (double y : specials)
    for (double x : specials)
      expect_ulp("atan2", lang_math_atan2_v1(y, x), std::atan2(y, x), 2);
  for (int i = 0; i < 10000; ++i) {
    const double y = random_finite();
    const double x = random_finite();
    expect_ulp("atan2", lang_math_atan2_v1(y, x), std::atan2(y, x), 2);
  }
}

void exponential_cases() {
  const double exp_values[] = {
      -std::numeric_limits<double>::infinity(), -800.0, -745.2, -708.0,
      -1.0, -0.0, 0.0, 1.0, 709.0, 710.0,
      std::numeric_limits<double>::infinity(),
      std::numeric_limits<double>::quiet_NaN(),
      std::numeric_limits<double>::denorm_min()};
  for (double x : exp_values)
    expect_ulp("exp", lang_math_exp_v1(x), std::exp(x), 1);
  for (int i = 0; i < 10000; ++i) {
    const double x = random_uniform(-800.0, 800.0);
    expect_ulp("exp", lang_math_exp_v1(x), std::exp(x), 1);
  }
  const double exp2_values[] = {-1100.0, -1075.0, -1074.0, -1.0, -0.0, 0.0,
                                1.0, 1023.0, 1024.0, 1100.0,
                                std::numeric_limits<double>::infinity(),
                                std::numeric_limits<double>::quiet_NaN()};
  for (double x : exp2_values)
    expect_ulp("exp2", lang_math_exp2_v1(x), std::exp2(x), 1);
  for (int i = 0; i < 10000; ++i) {
    const double x = random_uniform(-1100.0, 1100.0);
    expect_ulp("exp2", lang_math_exp2_v1(x), std::exp2(x), 1);
  }
  const double expm1_values[] = {-800.0, -1.0, -0.0, 0.0, 1.0, 709.0, 710.0,
                                 std::numeric_limits<double>::infinity(),
                                 -std::numeric_limits<double>::infinity(),
                                 std::numeric_limits<double>::quiet_NaN(),
                                 std::numeric_limits<double>::denorm_min()};
  for (double x : expm1_values)
    expect_ulp("expm1", lang_math_expm1_v1(x), std::expm1(x), 1);
  for (int i = 0; i < 10000; ++i) {
    const double x = random_uniform(-800.0, 710.0);
    expect_ulp("expm1", lang_math_expm1_v1(x), std::expm1(x), 1);
  }
}

void logarithmic_cases() {
  const double positive[] = {
      0.0, -0.0, std::numeric_limits<double>::denorm_min(),
      std::numeric_limits<double>::min(), 0.5, 1.0, 2.0,
      std::numeric_limits<double>::max(),
      std::numeric_limits<double>::infinity(), -1.0,
      -std::numeric_limits<double>::infinity(),
      std::numeric_limits<double>::quiet_NaN()};
  for (double x : positive) {
    expect_ulp("log", lang_math_log_v1(x), std::log(x), 1);
    expect_ulp("log2", lang_math_log2_v1(x), std::log2(x), 1);
    expect_ulp("log10", lang_math_log10_v1(x), std::log10(x), 2);
  }
  for (int i = 0; i < 10000; ++i) {
    const double x = random_positive();
    expect_ulp("log", lang_math_log_v1(x), std::log(x), 1);
    expect_ulp("log2", lang_math_log2_v1(x), std::log2(x), 1);
    expect_ulp("log10", lang_math_log10_v1(x), std::log10(x), 2);
  }

  const double log1p_values[] = {
      -1.0, std::nextafter(-1.0, 0.0), -2.0, -0.5, -0.0, 0.0,
      std::numeric_limits<double>::denorm_min(), 1.0,
      std::numeric_limits<double>::max(),
      std::numeric_limits<double>::infinity(),
      -std::numeric_limits<double>::infinity(),
      std::numeric_limits<double>::quiet_NaN()};
  for (double x : log1p_values)
    expect_ulp("log1p", lang_math_log1p_v1(x), std::log1p(x), 1);
  for (int i = 0; i < 10000; ++i) {
    double x = random_positive() - 1.0;
    if (!(x > -1.0)) x = -0.9999999999999999;
    expect_ulp("log1p", lang_math_log1p_v1(x), std::log1p(x), 1);
  }
}

void power_cases() {
  const double maximum = std::numeric_limits<double>::max();
  const double inf = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double cases[][2] = {
      {0.0, 0.0},      {0.0, 1.0},     {0.0, -1.0},   {-0.0, 3.0},
      {-0.0, -3.0},    {-0.0, 2.0},    {-2.0, 3.0},   {-2.0, 2.0},
      {-2.0, 0.5},     {2.0, 10.0},    {2.0, -1.0},   {1.0, inf},
      {0.5, inf},      {2.0, inf},     {inf, 0.0},    {inf, -1.0},
      {nan, 0.0},      {nan, 1.0},     {1.0, nan},    {maximum, 2.0},
      {0x1p-1000, 2.0}, {0x1p-1000, -2.0}};
  for (const auto &item : cases)
    expect_ulp("pow", lang_math_pow_v1(item[0], item[1]),
               std::pow(item[0], item[1]), 1);
  for (int i = 0; i < 5000; ++i) {
    const double base = random_positive();
    const double exponent = random_uniform(-2000.0, 2000.0);
    expect_ulp("pow", lang_math_pow_v1(base, exponent),
               std::pow(base, exponent), 1);
  }
  for (int i = 0; i < 5000; ++i) {
    const double base = random_uniform(0.5, 2.0);
    const double exponent = random_uniform(-1000.0, 1000.0);
    expect_ulp("pow", lang_math_pow_v1(base, exponent),
               std::pow(base, exponent), 1);
  }
  for (int i = 0; i < 2000; ++i) {
    const double base = -random_positive();
    const double exponent = static_cast<double>(
        static_cast<int32_t>(next_random() % 201) - 100);
    expect_ulp("pow-int", lang_math_pow_v1(base, exponent),
               std::pow(base, exponent), 1);
  }
}

void remainder_cases() {
  const double edges[][2] = {
      {5.0, 2.0}, {-5.0, 2.0}, {5.0, -2.0}, {-5.0, -2.0},
      {0.0, 2.0}, {-0.0, 2.0}, {1.0, 0.0}, {1.0, -0.0},
      {std::numeric_limits<double>::infinity(), 2.0},
      {1.0, std::numeric_limits<double>::infinity()},
      {std::numeric_limits<double>::max(), 1.0},
      {std::numeric_limits<double>::denorm_min(),
       std::numeric_limits<double>::min()}};
  for (const auto &item : edges) {
    expect_ulp("fmod", lang_math_fmod_v1(item[0], item[1]),
               std::fmod(item[0], item[1]), 0);
    expect_ulp("remainder", lang_math_remainder_v1(item[0], item[1]),
               std::remainder(item[0], item[1]), 0);
  }
  for (int i = 0; i < 10000; ++i) {
    const double x = random_finite();
    double y = random_finite();
    if (y == 0) y = 1;
    expect_ulp("fmod", lang_math_fmod_v1(x, y), std::fmod(x, y), 0);
    expect_ulp("remainder", lang_math_remainder_v1(x, y),
               std::remainder(x, y), 0);
  }
}

void ldexp_frexp_cases() {
  const double values[] = {
      0.0, -0.0, 1.0, -1.0, std::numeric_limits<double>::denorm_min(),
      std::numeric_limits<double>::min(),
      std::numeric_limits<double>::max(),
      std::numeric_limits<double>::infinity(),
      -std::numeric_limits<double>::infinity(),
      std::numeric_limits<double>::quiet_NaN()};
  const int32_t exponents[] = {-2000, -1075, -1074, -1022, -1, 0, 1,
                               1023, 1024, 2000};
  for (double x : values)
    for (int32_t e : exponents)
      expect_ulp("ldexp", lang_math_ldexp_v1(x, e), std::ldexp(x, e), 0);
  for (int i = 0; i < 10000; ++i) {
    const double x = random_finite();
    const int32_t e = static_cast<int32_t>(
        static_cast<int64_t>(next_random() % 4001) - 2000);
    expect_ulp("ldexp", lang_math_ldexp_v1(x, e), std::ldexp(x, e), 0);
  }

  for (double x : values) {
    int32_t actual_exponent = 0;
    int expected_exponent = 0;
    const double actual = lang_math_frexp_v1(x, &actual_exponent);
    const double expected = std::frexp(x, &expected_exponent);
    expect_ulp("frexp", actual, expected, 0);
    if (std::isfinite(x)) {
      if (actual_exponent != expected_exponent) {
        std::fprintf(stderr, "FAIL frexp exponent: actual=%d expected=%d\n",
                     actual_exponent, expected_exponent);
        ++failures;
      }
    }
  }
  for (int i = 0; i < 10000; ++i) {
    const double x = random_finite();
    int32_t actual_exponent = 0;
    int expected_exponent = 0;
    const double actual = lang_math_frexp_v1(x, &actual_exponent);
    const double expected = std::frexp(x, &expected_exponent);
    expect_ulp("frexp", actual, expected, 0);
    if (actual_exponent != expected_exponent) {
      std::fprintf(stderr, "FAIL frexp exponent: actual=%d expected=%d\n",
                   actual_exponent, expected_exponent);
      ++failures;
    }
  }
}

void predicate_cases() {
  const double values[] = {
      0.0, -0.0, 1.0, -1.0,
      std::numeric_limits<double>::denorm_min(),
      -std::numeric_limits<double>::denorm_min(),
      std::numeric_limits<double>::min(),
      std::numeric_limits<double>::max(),
      std::numeric_limits<double>::infinity(),
      -std::numeric_limits<double>::infinity(),
      std::numeric_limits<double>::quiet_NaN(),
      -std::numeric_limits<double>::quiet_NaN()};
  for (double x : values) {
    if (lang_math_isnan_v1(x) != std::isnan(x)) {
      std::fprintf(stderr, "FAIL isnan\n");
      ++failures;
    }
    if (lang_math_isinf_v1(x) != std::isinf(x)) {
      std::fprintf(stderr, "FAIL isinf\n");
      ++failures;
    }
    if (lang_math_isfinite_v1(x) != std::isfinite(x)) {
      std::fprintf(stderr, "FAIL isfinite\n");
      ++failures;
    }
    if (lang_math_signbit_v1(x) != std::signbit(x)) {
      std::fprintf(stderr, "FAIL signbit\n");
      ++failures;
    }
  }
}

} // namespace

int main() {
  std::fesetround(FE_TONEAREST);
  exact_unary_cases();
  sqrt_and_hypot_cases();
  fma_cases();
  trigonometric_cases();
  inverse_trigonometric_cases();
  exponential_cases();
  logarithmic_cases();
  power_cases();
  remainder_cases();
  ldexp_frexp_cases();
  predicate_cases();
  if (failures) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::printf("math_runtime_test: ok\n");
  return 0;
}

#endif
