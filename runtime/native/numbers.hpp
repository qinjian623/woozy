#pragma once
#include "text.hpp"
#include <cstdint>

// lang-native-numbers/1. Inputs must be one complete ASCII number token.
// Parsing returns 0, -22 (syntax), or -34 (range); `out` is written only on
// success, plus the documented successful underflow to signed zero.
//
// Integer grammar: [+-]?[0-9]+ for i64; +?[0-9]+ for u64. No whitespace,
// NUL, non-ASCII, or trailing bytes. Float grammar: [+-]?(nan|inf) matched
// case-insensitively as exact three-letter tokens, or a decimal mantissa with
// optional point and optional e/E exponent. At least one mantissa digit is
// required; "1." and ".5" are accepted. Underflow to +/-0 is success;
// overflow to infinity is -34.
//
// Float formatting uses Ryu's shortest round-tripping digits. Let `digits` be
// those digits after removing the point and trailing zeros, and let `e` be the
// decimal exponent of the first digit. The canonical spelling is:
//   -6 <= e < 21, e >= 0: fixed point after e+1 digits, zero-padded right;
//   -6 <= e < 21, e < 0:  "0." + (-e-1) zeros + digits;
//   otherwise:            digits[0] ['.' digits[1..]] "e" [+|-] abs(e),
//                         with no zero padding and '+' for positive exponents.
// A leading '-' is emitted for negative values. There are no trailing
// fraction zeros or trailing ".0". Examples: 1.5 -> "1.5", 1.0 -> "1",
// 1e-7 -> "1e-7", 1e-6 -> "0.000001", 1e20 -> 21 fixed digits,
// 1e21 -> "1e+21". Specials are lowercase "nan", "inf", "-inf"; negative
// zero is "-0".
extern "C" {
int64_t lang_num_parse_i64_v1(const LangText *text, int64_t *out) noexcept;
int64_t lang_num_parse_u64_v1(const LangText *text, uint64_t *out) noexcept;
int64_t lang_num_parse_f64_v1(const LangText *text, double *out) noexcept;
int64_t lang_num_parse_f32_v1(const LangText *text, float *out) noexcept;
void lang_num_format_i64_v1(LangText *out, int64_t value) noexcept;
void lang_num_format_u64_v1(LangText *out, uint64_t value) noexcept;
void lang_num_format_f64_v1(LangText *out, double value) noexcept;
void lang_num_format_f32_v1(LangText *out, float value) noexcept;
}
