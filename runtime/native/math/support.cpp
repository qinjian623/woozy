#include "../math.hpp"

namespace {

static_assert(sizeof(int) == sizeof(int32_t),
              "frexp/ldexp ABI requires 32-bit int");

uint64_t bits_of(double value) noexcept {
  uint64_t bits;
  __builtin_memcpy(&bits, &value, sizeof(bits));
  return bits;
}

} // namespace

extern "C" double lang_math_internal_ldexp_v1(double, int) noexcept;
extern "C" double lang_math_internal_frexp_v1(double, int *) noexcept;

extern "C" double lang_math_ldexp_v1(double value,
                                      int32_t exponent) noexcept {
  return lang_math_internal_ldexp_v1(value, static_cast<int>(exponent));
}

extern "C" double lang_math_frexp_v1(double value,
                                      int32_t *exponent) noexcept {
  int internal_exponent = 0;
  const double result =
      lang_math_internal_frexp_v1(value, &internal_exponent);
  *exponent = static_cast<int32_t>(internal_exponent);
  return result;
}

extern "C" bool lang_math_isnan_v1(double value) noexcept {
  return (bits_of(value) & UINT64_C(0x7fffffffffffffff)) >
         UINT64_C(0x7ff0000000000000);
}

extern "C" bool lang_math_isinf_v1(double value) noexcept {
  return (bits_of(value) & UINT64_C(0x7fffffffffffffff)) ==
         UINT64_C(0x7ff0000000000000);
}

extern "C" bool lang_math_isfinite_v1(double value) noexcept {
  return (bits_of(value) & UINT64_C(0x7fffffffffffffff)) <
         UINT64_C(0x7ff0000000000000);
}

extern "C" bool lang_math_signbit_v1(double value) noexcept {
  return (bits_of(value) >> 63) != 0;
}
