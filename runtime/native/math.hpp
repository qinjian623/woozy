#pragma once
#include <cstdint>

// lang-native-math/1. Double-precision IEEE math ABI, implemented from the
// selected musl double sources without libm/libc/errno/fenv/global rounding
// dependencies.  Default round-to-nearest-even is assumed.  Source-level f32
// helpers may cast through double; this ABI does not claim a separately
// correctly-rounded f32 implementation.
//
// NaN arguments/domain errors produce NaN, poles and overflow produce the
// appropriate infinity, signed zero is preserved, and subnormals are handled
// by the upstream algorithms.
extern "C" {
double lang_math_fabs_v1(double value) noexcept;
double lang_math_copysign_v1(double value, double sign) noexcept;
double lang_math_floor_v1(double value) noexcept;
double lang_math_ceil_v1(double value) noexcept;
double lang_math_trunc_v1(double value) noexcept;
double lang_math_round_v1(double value) noexcept;
double lang_math_sqrt_v1(double value) noexcept;
double lang_math_hypot_v1(double x, double y) noexcept;
double lang_math_fma_v1(double x, double y, double z) noexcept;
double lang_math_sin_v1(double value) noexcept;
double lang_math_cos_v1(double value) noexcept;
double lang_math_tan_v1(double value) noexcept;
double lang_math_asin_v1(double value) noexcept;
double lang_math_acos_v1(double value) noexcept;
double lang_math_atan_v1(double value) noexcept;
double lang_math_atan2_v1(double y, double x) noexcept;
double lang_math_exp_v1(double value) noexcept;
double lang_math_exp2_v1(double value) noexcept;
double lang_math_expm1_v1(double value) noexcept;
double lang_math_log_v1(double value) noexcept;
double lang_math_log2_v1(double value) noexcept;
double lang_math_log10_v1(double value) noexcept;
double lang_math_log1p_v1(double value) noexcept;
double lang_math_pow_v1(double base, double exponent) noexcept;
double lang_math_fmod_v1(double x, double y) noexcept;
double lang_math_remainder_v1(double x, double y) noexcept;
double lang_math_ldexp_v1(double value, int32_t exponent) noexcept;
double lang_math_frexp_v1(double value, int32_t *exponent) noexcept;
bool lang_math_isnan_v1(double value) noexcept;
bool lang_math_isinf_v1(double value) noexcept;
bool lang_math_isfinite_v1(double value) noexcept;
bool lang_math_signbit_v1(double value) noexcept;
}
