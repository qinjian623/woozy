// Vendor sources are trusted, upstream-tested code. Their assertions are
// removed in this translation unit only so the freestanding runtime has no
// __assert_fail reference; parser validation remains enabled below.
#ifndef NDEBUG
#define NDEBUG 1
#endif

#include "numbers.hpp"

// The ASCII parser is used here; SIMD paths only target char16_t. Disabling
// them also keeps freestanding builds from pulling in hosted <stdlib.h> via
// the x86 mm_malloc helpers.
#define FASTFLOAT_NO_SIMD 1
#include "vendor/fast_float/fast_float.h"
// These are the two Ryu buffered entry points. The convenience functions that
// malloc their result were removed by the vendoring patch (see PROVENANCE.md).
#include "vendor/ryu/d2s.c"
#include "vendor/ryu/f2s.c"

extern "C" [[noreturn]] void lang_native_panic_v1(uint32_t, uint64_t,
                                                 uint64_t,
                                                 uint64_t) noexcept;

namespace {

constexpr int64_t kSyntax = -22;
constexpr int64_t kRange = -34;
constexpr uint32_t kMisuse = 10;

[[noreturn]] void panic(uint32_t cause) noexcept {
  lang_native_panic_v1(cause, 0, 0, 0);
}

bool is_digit(uint8_t c) noexcept { return c >= '0' && c <= '9'; }

bool equal_ascii_ci(const uint8_t *data, const char *word,
                    uint64_t size) noexcept {
  for (uint64_t i = 0; i < size; ++i) {
    uint8_t c = data[i];
    if (c >= 'A' && c <= 'Z') c = static_cast<uint8_t>(c + ('a' - 'A'));
    if (c != static_cast<uint8_t>(word[i])) return false;
  }
  return true;
}

// Scans one signed decimal magnitude. Syntax is checked over the whole input
// before overflow is reported, so trailing junk is always a syntax error.
int64_t scan_decimal(const LangText *text, bool allow_negative, bool &negative,
                     uint64_t &magnitude, bool &overflow) noexcept {
  const uint8_t *data = text->data;
  const uint64_t size = text->bytes;
  if (size && !data) return kSyntax;
  uint64_t i = 0;
  negative = false;
  if (i < size && (data[i] == '+' || data[i] == '-')) {
    if (data[i] == '-') {
      if (!allow_negative) return kSyntax;
      negative = true;
    }
    ++i;
  }
  if (i == size) return kSyntax;
  magnitude = 0;
  overflow = false;
  for (; i < size; ++i) {
    const uint8_t c = data[i];
    if (!is_digit(c)) return kSyntax;
    const uint64_t digit = static_cast<uint64_t>(c - '0');
    if (magnitude > (~uint64_t{0} - digit) / 10) {
      overflow = true;
    } else if (!overflow) {
      magnitude = magnitude * 10 + digit;
    }
  }
  return 0;
}

enum class FloatKind : uint8_t { finite, nan, inf };

// Validates the complete token before the vendored parser sees it. This keeps
// fast_float's wider C++ grammar (hex, whitespace, nan(payload), infinity) out
// of the language ABI.
int64_t scan_float(const LangText *text, bool &negative, FloatKind &kind,
                   const uint8_t *&start, const uint8_t *&end) noexcept {
  const uint8_t *data = text->data;
  const uint64_t size = text->bytes;
  if (size && !data) return kSyntax;
  uint64_t i = 0;
  negative = false;
  if (i < size && (data[i] == '+' || data[i] == '-')) {
    negative = data[i] == '-';
    ++i;
  }
  if (size - i == 3 && equal_ascii_ci(data + i, "nan", 3)) {
    kind = FloatKind::nan;
    return 0;
  }
  if (size - i == 3 && equal_ascii_ci(data + i, "inf", 3)) {
    kind = FloatKind::inf;
    return 0;
  }
  const uint64_t body = i;
  uint64_t digits = 0;
  while (i < size && is_digit(data[i])) {
    ++i;
    ++digits;
  }
  if (i < size && data[i] == '.') {
    ++i;
    while (i < size && is_digit(data[i])) {
      ++i;
      ++digits;
    }
  }
  if (!digits) return kSyntax;
  if (i < size && (data[i] == 'e' || data[i] == 'E')) {
    ++i;
    if (i < size && (data[i] == '+' || data[i] == '-')) ++i;
    const uint64_t exponent_begin = i;
    while (i < size && is_digit(data[i])) ++i;
    if (i == exponent_begin) return kSyntax;
  }
  if (i != size) return kSyntax;
  kind = FloatKind::finite;
  start = data + (negative ? body - 1 : body);
  end = data + size;
  return 0;
}

template <typename T> struct FloatFormat;

template <> struct FloatFormat<double> {
  using Bits = uint64_t;
  static constexpr int kMantissaBits = 52;
  static constexpr int kExponentBits = 11;
  static constexpr Bits kMantissaMask = (Bits{1} << kMantissaBits) - 1;
  static constexpr Bits kExponentMask =
      ((Bits{1} << kExponentBits) - 1) << kMantissaBits;
  static constexpr Bits kSignMask = Bits{1} << (kMantissaBits + kExponentBits);

  static Bits bits(double value) noexcept {
    Bits result;
    __builtin_memcpy(&result, &value, sizeof(result));
    return result;
  }
};

template <> struct FloatFormat<float> {
  using Bits = uint32_t;
  static constexpr int kMantissaBits = 23;
  static constexpr int kExponentBits = 8;
  static constexpr Bits kMantissaMask = (Bits{1} << kMantissaBits) - 1;
  static constexpr Bits kExponentMask =
      ((Bits{1} << kExponentBits) - 1) << kMantissaBits;
  static constexpr Bits kSignMask = Bits{1} << (kMantissaBits + kExponentBits);

  static Bits bits(float value) noexcept {
    Bits result;
    __builtin_memcpy(&result, &value, sizeof(result));
    return result;
  }
};

template <typename T> bool is_nan_value(T value) noexcept {
  using F = FloatFormat<T>;
  const auto bits = F::bits(value);
  return (bits & F::kExponentMask) == F::kExponentMask &&
         (bits & F::kMantissaMask) != 0;
}

template <typename T> bool is_infinite_value(T value) noexcept {
  using F = FloatFormat<T>;
  const auto bits = F::bits(value);
  return (bits & F::kExponentMask) == F::kExponentMask &&
         (bits & F::kMantissaMask) == 0;
}

template <typename T> bool is_zero_value(T value) noexcept {
  using F = FloatFormat<T>;
  return (F::bits(value) & ~F::kSignMask) == 0;
}

template <typename T> T make_nan(bool negative) noexcept {
  using F = FloatFormat<T>;
  using Bits = typename F::Bits;
  Bits bits = F::kExponentMask |
              (Bits{1} << (F::kMantissaBits - 1));
  if (negative) bits |= F::kSignMask;
  T result;
  __builtin_memcpy(&result, &bits, sizeof(result));
  return result;
}

template <typename T> T make_infinity(bool negative) noexcept {
  using F = FloatFormat<T>;
  using Bits = typename F::Bits;
  Bits bits = F::kExponentMask;
  if (negative) bits |= F::kSignMask;
  T result;
  __builtin_memcpy(&result, &bits, sizeof(result));
  return result;
}

template <typename T>
int64_t parse_float(const LangText *text, T *out) noexcept {
  bool negative = false;
  FloatKind kind = FloatKind::finite;
  const uint8_t *start = nullptr;
  const uint8_t *end = nullptr;
  const int64_t error = scan_float(text, negative, kind, start, end);
  if (error) return error;
  if (kind == FloatKind::nan) {
    *out = make_nan<T>(negative);
    return 0;
  }
  if (kind == FloatKind::inf) {
    *out = make_infinity<T>(negative);
    return 0;
  }
  T value{};
  const auto result = fast_float::from_chars(
      reinterpret_cast<const char *>(start),
      reinterpret_cast<const char *>(end), value);
  if (result.ptr != reinterpret_cast<const char *>(end))
    return kSyntax;
  if (result.ec == fast_float::errc::invalid_argument) return kSyntax;
  if (result.ec == fast_float::errc::result_out_of_range) {
    if (is_infinite_value(value)) return kRange;
    // fast_float reports correctly rounded underflow with the same code.
    if (is_zero_value(value)) {
      *out = value;
      return 0;
    }
    return kRange;
  }
  if (result.ec != fast_float::errc()) return kSyntax;
  *out = value;
  return 0;
}

void store_ascii(LangText *out, const char *data, uint64_t size) noexcept {
  if (lang_text_from_utf8_v1(
          out, reinterpret_cast<const uint8_t *>(data), size) != 0)
    panic(kMisuse);
}

// Converts Ryu's uppercase scientific form into the language's deterministic
// general spelling. Ryu has already produced the shortest round-tripping
// digit sequence; this routine only moves the decimal point and lowercases.
uint64_t normalize_scientific(const char *source, int size,
                              char *out) noexcept {
  int i = 0;
  bool negative = false;
  if (i < size && source[i] == '-') {
    negative = true;
    ++i;
  }
  char digits[32];
  uint64_t count = 0;
  while (i < size && source[i] != 'E') {
    const char c = source[i++];
    if (c == '.') continue;
    if (c < '0' || c > '9' || count == sizeof(digits)) return 0;
    digits[count++] = c;
  }
  if (i == size || source[i] != 'E' || count == 0) return 0;
  ++i;
  bool exponent_negative = false;
  if (i < size && source[i] == '-') {
    exponent_negative = true;
    ++i;
  }
  int exponent = 0;
  for (; i < size; ++i) {
    const char c = source[i];
    if (c < '0' || c > '9') return 0;
    exponent = exponent * 10 + (c - '0');
  }
  if (exponent_negative) exponent = -exponent;
  while (count > 1 && digits[count - 1] == '0') --count;

  uint64_t pos = 0;
  if (negative) out[pos++] = '-';
  if (exponent >= -6 && exponent < 21) {
    if (exponent >= 0) {
      const uint64_t point = static_cast<uint64_t>(exponent) + 1;
      if (count <= point) {
        for (uint64_t j = 0; j < count; ++j) out[pos++] = digits[j];
        for (uint64_t j = count; j < point; ++j) out[pos++] = '0';
      } else {
        for (uint64_t j = 0; j < point; ++j) out[pos++] = digits[j];
        out[pos++] = '.';
        for (uint64_t j = point; j < count; ++j) out[pos++] = digits[j];
      }
    } else {
      out[pos++] = '0';
      out[pos++] = '.';
      for (uint64_t j = 0; j < static_cast<uint64_t>(-exponent - 1); ++j)
        out[pos++] = '0';
      for (uint64_t j = 0; j < count; ++j) out[pos++] = digits[j];
    }
  } else {
    out[pos++] = digits[0];
    if (count > 1) {
      out[pos++] = '.';
      for (uint64_t j = 1; j < count; ++j) out[pos++] = digits[j];
    }
    out[pos++] = 'e';
    const bool negative_exponent = exponent < 0;
    out[pos++] = negative_exponent ? '-' : '+';
    uint64_t magnitude = static_cast<uint64_t>(
        negative_exponent ? -exponent : exponent);
    char reverse[8];
    uint64_t used = 0;
    do {
      reverse[used++] = static_cast<char>('0' + magnitude % 10);
      magnitude /= 10;
    } while (magnitude);
    while (used) out[pos++] = reverse[--used];
  }
  return pos;
}

template <typename T> void format_float(LangText *out, T value) noexcept {
  using F = FloatFormat<T>;
  const auto bits = F::bits(value);
  if (is_nan_value(value)) {
    store_ascii(out, "nan", 3);
    return;
  }
  if (is_infinite_value(value)) {
    if (bits & F::kSignMask)
      store_ascii(out, "-inf", 4);
    else
      store_ascii(out, "inf", 3);
    return;
  }
  if (is_zero_value(value)) {
    if (bits & F::kSignMask)
      store_ascii(out, "-0", 2);
    else
      store_ascii(out, "0", 1);
    return;
  }
  char scientific[32];
  int size = 0;
  if constexpr (sizeof(T) == sizeof(double))
    size = d2s_buffered_n(static_cast<double>(value), scientific);
  else
    size = f2s_buffered_n(static_cast<float>(value), scientific);
  char normalized[64];
  const uint64_t length = normalize_scientific(scientific, size, normalized);
  if (!length) panic(kMisuse);
  store_ascii(out, normalized, length);
}

void format_unsigned_digits(char *buffer, uint64_t &begin, uint64_t value,
                            bool negative) noexcept {
  constexpr uint64_t kSize = 24;
  uint64_t pos = kSize;
  do {
    buffer[--pos] = static_cast<char>('0' + value % 10);
    value /= 10;
  } while (value);
  if (negative) buffer[--pos] = '-';
  begin = pos;
}

} // namespace

extern "C" int64_t lang_num_parse_i64_v1(const LangText *text,
                                          int64_t *out) noexcept {
  if (!text || !out) panic(kMisuse);
  bool negative = false;
  bool overflow = false;
  uint64_t magnitude = 0;
  const int64_t error =
      scan_decimal(text, true, negative, magnitude, overflow);
  if (error) return error;
  if (overflow) return kRange;
  if (!negative) {
    if (magnitude > static_cast<uint64_t>(INT64_MAX)) return kRange;
    *out = static_cast<int64_t>(magnitude);
    return 0;
  }
  const uint64_t limit = static_cast<uint64_t>(INT64_MAX) + 1;
  if (magnitude > limit) return kRange;
  *out = magnitude == limit ? INT64_MIN : -static_cast<int64_t>(magnitude);
  return 0;
}

extern "C" int64_t lang_num_parse_u64_v1(const LangText *text,
                                          uint64_t *out) noexcept {
  if (!text || !out) panic(kMisuse);
  bool negative = false;
  bool overflow = false;
  uint64_t magnitude = 0;
  const int64_t error =
      scan_decimal(text, false, negative, magnitude, overflow);
  if (error) return error;
  if (overflow) return kRange;
  *out = magnitude;
  return 0;
}

extern "C" int64_t lang_num_parse_f64_v1(const LangText *text,
                                          double *out) noexcept {
  if (!text || !out) panic(kMisuse);
  return parse_float(text, out);
}

extern "C" int64_t lang_num_parse_f32_v1(const LangText *text,
                                          float *out) noexcept {
  if (!text || !out) panic(kMisuse);
  return parse_float(text, out);
}

extern "C" void lang_num_format_i64_v1(LangText *out,
                                        int64_t value) noexcept {
  if (!out) panic(kMisuse);
  const bool negative = value < 0;
  const uint64_t magnitude =
      negative ? ~static_cast<uint64_t>(value) + 1
               : static_cast<uint64_t>(value);
  char buffer[24];
  uint64_t begin = 0;
  format_unsigned_digits(buffer, begin, magnitude, negative);
  store_ascii(out, buffer + begin, sizeof(buffer) - begin);
}

extern "C" void lang_num_format_u64_v1(LangText *out,
                                        uint64_t value) noexcept {
  if (!out) panic(kMisuse);
  char buffer[24];
  uint64_t begin = 0;
  format_unsigned_digits(buffer, begin, value, false);
  store_ascii(out, buffer + begin, sizeof(buffer) - begin);
}

extern "C" void lang_num_format_f64_v1(LangText *out,
                                        double value) noexcept {
  if (!out) panic(kMisuse);
  format_float(out, value);
}

extern "C" void lang_num_format_f32_v1(LangText *out, float value) noexcept {
  if (!out) panic(kMisuse);
  format_float(out, value);
}
