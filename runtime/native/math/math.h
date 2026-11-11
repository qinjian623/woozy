#ifndef _MATH_H
#define _MATH_H

/*
 * Minimal freestanding <math.h> for the vendored musl C units.
 *
 * This file is only on the include path of the isolated math sources.  The
 * host oracle test continues to use the host C++ library's <cmath>/<math.h>.
 * It provides the type names, constants, predicates, and declarations used by
 * the selected musl double-precision files.  No libc function is declared or
 * called.
 */

#include <features.h>
#include <float.h>
#include <stdint.h>

typedef double double_t;
typedef float float_t;

#define NAN __builtin_nan("")
#define INFINITY __builtin_inf()
#define HUGE_VAL ((double)INFINITY)
#define HUGE_VALF ((float)INFINITY)

#define MATH_ERRNO 1
#define MATH_ERREXCEPT 2
#define math_errhandling MATH_ERREXCEPT

#define FP_NAN 0
#define FP_INFINITE 1
#define FP_ZERO 2
#define FP_SUBNORMAL 3
#define FP_NORMAL 4

static __inline uint32_t __lang_math_float_bits(float value) {
  union {
    float f;
    uint32_t i;
  } bits = {value};
  return bits.i;
}

static __inline uint64_t __lang_math_double_bits(double value) {
  union {
    double f;
    uint64_t i;
  } bits = {value};
  return bits.i;
}

#define isnan(x)                                                               \
  ((__lang_math_double_bits((double)(x)) & (UINT64_C(0x7fffffffffffffff))) >   \
   UINT64_C(0x7ff0000000000000))
#define isinf(x)                                                               \
  ((__lang_math_double_bits((double)(x)) & (UINT64_C(0x7fffffffffffffff))) ==  \
   UINT64_C(0x7ff0000000000000))
#define isfinite(x)                                                            \
  ((__lang_math_double_bits((double)(x)) & (UINT64_C(0x7fffffffffffffff))) <   \
   UINT64_C(0x7ff0000000000000))
#define signbit(x)                                                             \
  ((int)(__lang_math_double_bits((double)(x)) >> 63))
#define isnormal(x)                                                            \
  (((__lang_math_double_bits((double)(x)) + UINT64_C(0x0010000000000000)) &    \
    (UINT64_C(0x7fffffffffffffff))) >= UINT64_C(0x0020000000000000))
#define fpclassify(x)                                                          \
  (isnan(x) ? FP_NAN                                                           \
            : isinf(x)                                                         \
                  ? FP_INFINITE                                                \
                  : (((__lang_math_double_bits((double)(x)) &                  \
                       UINT64_C(0x7fffffffffffffff)) == 0)                     \
                         ? FP_ZERO                                             \
                         : (((__lang_math_double_bits((double)(x)) &           \
                              UINT64_C(0x7ff0000000000000)) == 0)              \
                                ? FP_SUBNORMAL                                 \
                                : FP_NORMAL)))

double fabs(double);
double copysign(double, double);
double floor(double);
double ceil(double);
double trunc(double);
double round(double);
double sqrt(double);
double hypot(double, double);
double fma(double, double, double);
double sin(double);
double cos(double);
double tan(double);
double asin(double);
double acos(double);
double atan(double);
double atan2(double, double);
double exp(double);
double exp2(double);
double expm1(double);
double log(double);
double log2(double);
double log10(double);
double log1p(double);
double pow(double, double);
double fmod(double, double);
double remainder(double, double);
double remquo(double, double, int *);
double ldexp(double, int);
double scalbn(double, int);
double frexp(double, int *);

#endif
