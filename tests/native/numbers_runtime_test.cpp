// Host acceptance harness for runtime/native/numbers.cpp.
//
// The runtime object under test is freestanding and has no libc/libstdc++
// dependency. This harness uses the host C++ library only as an independent
// oracle for decimal conversion and for diagnostics.
//
// Build (the test report contains the exact GCC/Clang commands):
//   <cxx> -std=c++20 -O2 -I runtime/native tests/native/numbers_runtime_test.cpp
//         runtime/native/numbers.cpp runtime/native/text.cpp
//         runtime/native/alloc.cpp runtime/native/panic.cpp
//         runtime/native/freestanding.cpp runtime/native/thread.cpp
//         -o numbers_runtime_test

#include "../../runtime/native/numbers.hpp"

#ifdef LANG_NUMBERS_FREESTANDING
// Build with -DLANG_NUMBERS_FREESTANDING for the no-libc static ELF smoke.
// The host-oracle branch below is intentionally excluded in that mode.
extern "C" void lang_handle_release_v1(LangHandle *) noexcept;

namespace {
LangText literal(const char *data, uint64_t size) {
  return {reinterpret_cast<const uint8_t *>(data), size, size, nullptr};
}
bool equal(const LangText &text, const char *expected, uint64_t size) {
  if (text.bytes != size) return false;
  for (uint64_t i = 0; i < size; ++i)
    if (text.data[i] != static_cast<uint8_t>(expected[i])) return false;
  return true;
}
uint64_t f64_bits(double value) {
  uint64_t bits;
  __builtin_memcpy(&bits, &value, sizeof(bits));
  return bits;
}
uint32_t f32_bits(float value) {
  uint32_t bits;
  __builtin_memcpy(&bits, &value, sizeof(bits));
  return bits;
}
bool check_format_f64(double value, const char *expected, uint64_t size) {
  LangText out{};
  lang_num_format_f64_v1(&out, value);
  const bool ok = equal(out, expected, size);
  if (out.owner) lang_handle_release_v1(out.owner);
  return ok;
}
bool check_format_f32(float value, const char *expected, uint64_t size) {
  LangText out{};
  lang_num_format_f32_v1(&out, value);
  const bool ok = equal(out, expected, size);
  if (out.owner) lang_handle_release_v1(out.owner);
  return ok;
}
} // namespace

extern "C" int main() {
  int64_t i64 = 0;
  uint64_t u64 = 0;
  double f64 = 0;
  float f32 = 0;
  const LangText i64_min = literal("-9223372036854775808", 20);
  const LangText u64_max = literal("18446744073709551615", 20);
  const LangText one_point_five = literal("1.5", 3);
  const LangText overflow = literal("1e309", 5);
  const LangText underflow = literal("-1e-5000", 8);
  const LangText tiny32 = literal("1e-100", 6);
  if (lang_num_parse_i64_v1(&i64_min, &i64) != 0 || i64 != INT64_MIN)
    return 1;
  if (lang_num_parse_u64_v1(&u64_max, &u64) != 0 || u64 != UINT64_MAX)
    return 2;
  if (lang_num_parse_f64_v1(&one_point_five, &f64) != 0 ||
      f64_bits(f64) != 0x3ff8000000000000ULL)
    return 3;
  if (lang_num_parse_f64_v1(&overflow, &f64) != -34) return 4;
  if (lang_num_parse_f64_v1(&underflow, &f64) != 0 ||
      f64_bits(f64) != 0x8000000000000000ULL)
    return 5;
  if (lang_num_parse_f32_v1(&tiny32, &f32) != 0 || f32_bits(f32) != 0)
    return 6;
  if (!check_format_f64(1.0, "1", 1)) return 7;
  if (!check_format_f64(1e-7, "1e-7", 4)) return 8;
  if (!check_format_f64(1e21, "1e+21", 5)) return 9;
  if (!check_format_f64(-0.0, "-0", 2)) return 10;
  if (!check_format_f64(1.0 / 0.0, "inf", 3)) return 11;
  if (!check_format_f32(1.5f, "1.5", 3)) return 12;
  if (!check_format_f32(-1.0f / 0.0f, "-inf", 4)) return 13;
  LangText iout{};
  lang_num_format_i64_v1(&iout, INT64_MIN);
  const bool iok = equal(iout, "-9223372036854775808", 20);
  if (iout.owner) lang_handle_release_v1(iout.owner);
  if (!iok) return 14;
  return 0;
}

#else

#include <bit>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

namespace {

int failures = 0;

void check(bool ok, const char *expression, int line) {
  if (!ok) {
    std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
    ++failures;
  }
}

void check_text(std::string_view actual, std::string_view expected, int line) {
  if (actual != expected) {
    std::fprintf(stderr, "FAIL line %d: got '%.*s' expected '%.*s'\n", line,
                 static_cast<int>(actual.size()), actual.data(),
                 static_cast<int>(expected.size()), expected.data());
    ++failures;
  }
}

#define CHECK(x) check((x), #x, __LINE__)
#define CHECK_TEXT(actual, expected) check_text((actual), (expected), __LINE__)

struct Text {
  LangText value{};
  explicit Text(std::string_view bytes) {
    const auto *data = reinterpret_cast<const uint8_t *>(bytes.data());
    if (lang_text_from_utf8_v1(&value, data, bytes.size()) != 0) {
      std::fprintf(stderr, "could not construct test text\n");
      std::exit(2);
    }
  }
  ~Text() {
    if (value.owner) lang_handle_release_v1(value.owner);
  }
  Text(const Text &) = delete;
  Text &operator=(const Text &) = delete;
};

std::string take(LangText &text) {
  std::string result(reinterpret_cast<const char *>(text.data), text.bytes);
  if (text.owner) lang_handle_release_v1(text.owner);
  text = {};
  return result;
}

std::string format_i64(int64_t value) {
  LangText out{};
  lang_num_format_i64_v1(&out, value);
  return take(out);
}
std::string format_u64(uint64_t value) {
  LangText out{};
  lang_num_format_u64_v1(&out, value);
  return take(out);
}
std::string format_f64(double value) {
  LangText out{};
  lang_num_format_f64_v1(&out, value);
  return take(out);
}
std::string format_f32(float value) {
  LangText out{};
  lang_num_format_f32_v1(&out, value);
  return take(out);
}

template <typename T> T from_bits(uint64_t bits) {
  if constexpr (sizeof(T) == 8) {
    return std::bit_cast<T>(bits);
  } else {
    return std::bit_cast<T>(static_cast<uint32_t>(bits));
  }
}

template <typename T> uint64_t bits_of(T value) {
  if constexpr (sizeof(T) == 8)
    return std::bit_cast<uint64_t>(value);
  else
    return std::bit_cast<uint32_t>(value);
}

template <typename T>
int64_t parse_number(std::string_view text, T &value) {
  Text input(text);
  if constexpr (std::is_same_v<T, int64_t>)
    return lang_num_parse_i64_v1(&input.value, &value);
  else if constexpr (std::is_same_v<T, uint64_t>)
    return lang_num_parse_u64_v1(&input.value, &value);
  else if constexpr (std::is_same_v<T, double>)
    return lang_num_parse_f64_v1(&input.value, &value);
  else
    return lang_num_parse_f32_v1(&input.value, &value);
}

template <typename T>
int oracle_parse(std::string_view text, T &value) {
  // The host's from_chars is the primary independent rounding oracle. For the
  // range cases, strtod/strtof distinguishes overflow (ERANGE + infinity)
  // from underflow (ERANGE + zero), which the runtime ABI treats differently.
  T parsed{};
  auto result = std::from_chars(text.data(), text.data() + text.size(), parsed,
                                std::chars_format::general);
  if (result.ec == std::errc::invalid_argument) return -22;
  if (result.ptr != text.data() + text.size()) return -22;
  if (result.ec == std::errc()) {
    value = parsed;
    return 0;
  }
  if (result.ec != std::errc::result_out_of_range) return -34;

  const std::string owned(text);
  char *end = nullptr;
  errno = 0;
  if constexpr (sizeof(T) == 8)
    value = std::strtod(owned.c_str(), &end);
  else
    value = std::strtof(owned.c_str(), &end);
  if (end != owned.c_str() + owned.size()) return -22;
  if (errno == ERANGE && std::isinf(value)) return -34;
  return 0;
}

template <typename T> void check_decimal(std::string_view text) {
  T actual = from_bits<T>(0x123456789abcdefULL);
  const uint64_t before = bits_of(actual);
  T expected{};
  const int64_t actual_code = parse_number(text, actual);
  const int expected_code = oracle_parse(text, expected);
  if (actual_code != expected_code) {
    static int shown = 0;
    if (shown++ < 20)
      std::fprintf(stderr, "decimal mismatch '%.*s': actual %lld expected %d\n",
                   static_cast<int>(text.size()), text.data(),
                   static_cast<long long>(actual_code), expected_code);
  }
  CHECK(actual_code == expected_code);
  if (actual_code == 0) {
    CHECK(bits_of(actual) == bits_of(expected));
  } else {
    CHECK(bits_of(actual) == before);
  }
}

void integer_tests() {
  struct Case {
    const char *text;
    int64_t expected_code;
    int64_t expected_value;
  };
  const Case i64_cases[] = {
      {"0", 0, 0},
      {"-0", 0, 0},
      {"+0", 0, 0},
      {"9223372036854775807", 0, std::numeric_limits<int64_t>::max()},
      {"-9223372036854775808", 0, std::numeric_limits<int64_t>::min()},
      {"+00000000000000000001", 0, 1},
      {"-00000000000000000001", 0, -1},
      {"9223372036854775808", -34, 0},
      {"-9223372036854775809", -34, 0},
      {"18446744073709551616", -34, 0},
      {"999999999999999999999999999999999999999", -34, 0},
      {"", -22, 0},
      {"+", -22, 0},
      {"-", -22, 0},
      {" 1", -22, 0},
      {"1 ", -22, 0},
      {"1x", -22, 0},
      {"++1", -22, 0},
      {"--1", -22, 0},
      {"1.0", -22, 0},
      {"0x10", -22, 0},
  };
  for (const auto &item : i64_cases) {
    int64_t value = 0x123456789abcdefLL;
    CHECK(parse_number(item.text, value) == item.expected_code);
    if (item.expected_code == 0)
      CHECK(value == item.expected_value);
    else
      CHECK(value == 0x123456789abcdefLL);
  }

  const Case u64_cases[] = {
      {"0", 0, 0},
      {"+0", 0, 0},
      {"18446744073709551615", 0,
       static_cast<int64_t>(std::numeric_limits<uint64_t>::max())},
      {"+18446744073709551615", 0,
       static_cast<int64_t>(std::numeric_limits<uint64_t>::max())},
      {"18446744073709551616", -34, 0},
      {"-0", -22, 0},
      {"-1", -22, 0},
      {"+", -22, 0},
      {"", -22, 0},
      {"1 ", -22, 0},
      {"1e3", -22, 0},
  };
  for (const auto &item : u64_cases) {
    uint64_t value = 0x123456789abcdefULL;
    CHECK(parse_number(item.text, value) == item.expected_code);
    if (item.expected_code == 0)
      CHECK(value == static_cast<uint64_t>(item.expected_value));
    else
      CHECK(value == 0x123456789abcdefULL);
  }

  const uint8_t nul[] = {'1', 0, '2'};
  const LangText nul_text{nul, sizeof(nul), sizeof(nul), nullptr};
  uint64_t uvalue = 7;
  CHECK(lang_num_parse_u64_v1(&nul_text, &uvalue) == -22);
  CHECK(uvalue == 7);

  const uint8_t non_ascii[] = {0xc3, 0xa9}; // valid UTF-8 "é", not a number
  const LangText non_ascii_text{non_ascii, sizeof(non_ascii), 1, nullptr};
  int64_t ivalue = 7;
  CHECK(lang_num_parse_i64_v1(&non_ascii_text, &ivalue) == -22);
  CHECK(ivalue == 7);

  CHECK_TEXT(format_i64(0), "0");
  CHECK_TEXT(format_i64(-1), "-1");
  CHECK_TEXT(format_i64(std::numeric_limits<int64_t>::max()),
             "9223372036854775807");
  CHECK_TEXT(format_i64(std::numeric_limits<int64_t>::min()),
             "-9223372036854775808");
  CHECK_TEXT(format_u64(0), "0");
  CHECK_TEXT(format_u64(std::numeric_limits<uint64_t>::max()),
             "18446744073709551615");
}

void float_syntax_tests() {
  const char *decimal_f64[] = {
      "0", "0.0", "-0", "-0.0", "1", "-1", "1.5", "-1.5", ".5", "-.5", "1.",
      "1e0", "1E+0", "1e-0", "1.25e3", "-1.25e-3", "123400", "0.000001",
      "1e-7", "1e20", "1e21", "1e-400", "-1e-400", "1e-999999999999999999",
      "4.9406564584124654e-324", "2.4703282292062327e-324",
      "9007199254740993", "9007199254740995", "1.7976931348623157e308",
  };
  for (const char *text : decimal_f64) check_decimal<double>(text);

  const char *decimal_f32[] = {
      "0", "-0.0", "1", "1.5", "-1.5", ".5", "1.", "1e0", "1E+0",
      "1e-7", "1e-6", "1e20", "1e21", "1e-50", "-1e-50", "1e39",
      "16777217", "16777219", "1.17549435e-38", "1.40129846e-45",
      "3.4028235e38", "3.4028236e38", "7.006492321624085e-46",
  };
  for (const char *text : decimal_f32) check_decimal<float>(text);

  const char *invalid[] = {
      "", ".", "+", "-", "e1", "1e", "1e+", "1e-", "1.2.3", "0x1p2",
      " 1", "1 ", "infinity", "-infinity", "nan(0)", "nanx", "inff", "in",
      "1e 2", "12,3",
  };
  for (const char *text : invalid) {
    double d = 123.456;
    float f = 123.456f;
    CHECK(parse_number(text, d) == -22);
    CHECK(parse_number(text, f) == -22);
    CHECK(d == 123.456);
    CHECK(f == 123.456f);
  }

  const char *overflow_f64[] = {"1e309", "-1e309",
                                "1.7976931348623159e308",
                                "1e999999999999999999999"};
  for (const char *text : overflow_f64) {
    double d = 123.456;
    CHECK(parse_number(text, d) == -34);
    CHECK(d == 123.456);
  }
  const char *overflow_f32[] = {"1e39", "-1e39", "3.5e38", "-3.5e38",
                                "3.4028236e38", "1e309"};
  for (const char *text : overflow_f32) {
    float f = 123.456f;
    CHECK(parse_number(text, f) == -34);
    CHECK(f == 123.456f);
  }

  double underflow = 1;
  CHECK(parse_number("1e-5000", underflow) == 0);
  CHECK(bits_of(underflow) == 0);
  underflow = 1;
  CHECK(parse_number("-1e-5000", underflow) == 0);
  CHECK(bits_of(underflow) == (uint64_t{1} << 63));
  float underflow32 = 1;
  CHECK(parse_number("-1e-100", underflow32) == 0);
  CHECK(bits_of(underflow32) == (uint32_t{1} << 31));
}

void special_tests() {
  const char *nan_tokens[] = {"nan", "NaN", "nAn", "+nan", "-nan"};
  for (const char *text : nan_tokens) {
    double d = 1;
    float f = 1;
    CHECK(parse_number(text, d) == 0);
    CHECK(parse_number(text, f) == 0);
    CHECK(std::isnan(d));
    CHECK(std::isnan(f));
  }
  CHECK_TEXT(format_f64(std::numeric_limits<double>::quiet_NaN()), "nan");
  CHECK_TEXT(format_f64(-std::numeric_limits<double>::quiet_NaN()), "nan");
  CHECK_TEXT(format_f32(std::numeric_limits<float>::quiet_NaN()), "nan");

  const char *inf_tokens[] = {"inf", "INF", "iNf", "+inf", "Inf"};
  for (const char *text : inf_tokens) {
    double d = 0;
    float f = 0;
    CHECK(parse_number(text, d) == 0);
    CHECK(parse_number(text, f) == 0);
    CHECK(std::isinf(d) && d > 0);
    CHECK(std::isinf(f) && f > 0);
  }
  double negative_inf = 0;
  float negative_inf32 = 0;
  CHECK(parse_number("-inf", negative_inf) == 0);
  CHECK(parse_number("-INF", negative_inf32) == 0);
  CHECK(std::isinf(negative_inf) && negative_inf < 0);
  CHECK(std::isinf(negative_inf32) && negative_inf32 < 0);

  CHECK_TEXT(format_f64(std::numeric_limits<double>::infinity()), "inf");
  CHECK_TEXT(format_f64(-std::numeric_limits<double>::infinity()), "-inf");
  CHECK_TEXT(format_f32(std::numeric_limits<float>::infinity()), "inf");
  CHECK_TEXT(format_f32(-std::numeric_limits<float>::infinity()), "-inf");
  CHECK_TEXT(format_f64(0.0), "0");
  CHECK_TEXT(format_f64(-0.0), "-0");
  CHECK_TEXT(format_f32(0.0f), "0");
  CHECK_TEXT(format_f32(-0.0f), "-0");
}

void format_canonical_tests() {
  struct F64Case {
    double value;
    const char *expected;
  };
  const F64Case cases[] = {
      {1.5, "1.5"},
      {1.0, "1"},
      {1e-7, "1e-7"},
      {1e-6, "0.000001"},
      {1e20, "100000000000000000000"},
      {1e21, "1e+21"},
      {-1e21, "-1e+21"},
      {1.234e5, "123400"},
      {1e-5, "0.00001"},
      {std::numeric_limits<double>::denorm_min(), "5e-324"},
  };
  for (const auto &item : cases)
    CHECK_TEXT(format_f64(item.value), item.expected);

  struct F32Case {
    float value;
    const char *expected;
  };
  const F32Case cases32[] = {
      {1.5f, "1.5"},
      {1.0f, "1"},
      {1e-7f, "1e-7"},
      {1e-6f, "0.000001"},
      {1e20f, "100000000000000000000"},
      {1e21f, "1e+21"},
      {-1e21f, "-1e+21"},
      {1.234e5f, "123400"},
      {std::numeric_limits<float>::denorm_min(), "1e-45"},
  };
  for (const auto &item : cases32)
    CHECK_TEXT(format_f32(item.value), item.expected);
}

template <typename T> void check_round_trip(uint64_t raw) {
  LangText output{};
  if constexpr (sizeof(T) == 8)
    lang_num_format_f64_v1(&output, std::bit_cast<double>(raw));
  else
    lang_num_format_f32_v1(&output,
                           std::bit_cast<float>(static_cast<uint32_t>(raw)));
  const std::string spelling =
      std::string(reinterpret_cast<const char *>(output.data), output.bytes);
  if (output.owner) lang_handle_release_v1(output.owner);

  T parsed = from_bits<T>(0);
  CHECK(parse_number(spelling, parsed) == 0);
  CHECK(bits_of(parsed) == raw);

  T oracle{};
  CHECK(oracle_parse(spelling, oracle) == 0);
  CHECK(bits_of(oracle) == raw);
}

void random_bits_and_decimals() {
  uint64_t state = 0x8a5cd789635d2dffULL;
  auto next = [&]() {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  };

  for (int i = 0; i < 10000; ++i) {
    uint64_t raw = next();
    raw &= ~(uint64_t{0x7ff} << 52);
    raw |= (next() % 2047) << 52;
    check_round_trip<double>(raw);
  }
  for (int i = 0; i < 10000; ++i) {
    uint32_t raw = static_cast<uint32_t>(next());
    raw &= ~(uint32_t{0xff} << 23);
    raw |= static_cast<uint32_t>(next() % 255) << 23;
    check_round_trip<float>(raw);
  }

  for (int i = 0; i < 4000; ++i) {
    const int length = 1 + static_cast<int>(next() % 45);
    const int point = static_cast<int>(next() % (length + 1));
    std::string text;
    for (int j = 0; j < length; ++j) {
      if (j == point) text.push_back('.');
      const int digit = j == 0 ? 1 + static_cast<int>(next() % 9)
                               : static_cast<int>(next() % 10);
      text.push_back(static_cast<char>('0' + digit));
    }
    if (point == length) text.push_back('.');
    if (next() & 1) {
      text.push_back('e');
      if (next() & 1) text.push_back('-');
      text += std::to_string(next() % 700);
    }
    check_decimal<double>(text);
  }

  for (int i = 0; i < 4000; ++i) {
    const int length = 1 + static_cast<int>(next() % 30);
    const int point = static_cast<int>(next() % (length + 1));
    std::string text;
    for (int j = 0; j < length; ++j) {
      if (j == point) text.push_back('.');
      const int digit = j == 0 ? 1 + static_cast<int>(next() % 9)
                               : static_cast<int>(next() % 10);
      text.push_back(static_cast<char>('0' + digit));
    }
    if (point == length) text.push_back('.');
    if (next() & 1) {
      text.push_back('e');
      if (next() & 1) text.push_back('-');
      text += std::to_string(next() % 90);
    }
    check_decimal<float>(text);
  }

  const std::string long_integer(500, '9');
  check_decimal<double>(long_integer);
  check_decimal<float>(long_integer);
  const std::string long_fraction =
      "0." + std::string(600, '0') + "123456789e600";
  check_decimal<double>(long_fraction);
  check_decimal<float>(long_fraction);
  const std::string long_mantissa =
      "12345678901234567890123456789012345678901234567890e-25";
  check_decimal<double>(long_mantissa);
  check_decimal<float>(long_mantissa);
  const std::string zeros(200, '0');
  const std::string long_exponent = "1e+" + zeros + "1";
  const std::string long_negative_exponent = "1e-" + zeros + "1";
  const std::string long_overflow_exponent = "1e+" + zeros + "309";
  check_decimal<double>(long_exponent);
  check_decimal<float>(long_exponent);
  check_decimal<double>(long_negative_exponent);
  check_decimal<float>(long_negative_exponent);
  check_decimal<double>(long_overflow_exponent);
  check_decimal<float>(long_overflow_exponent);
}

} // namespace

int main() {
  integer_tests();
  float_syntax_tests();
  special_tests();
  format_canonical_tests();
  random_bits_and_decimals();
  if (failures) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::printf("numbers_runtime_test: ok\n");
  return 0;
}

#endif
