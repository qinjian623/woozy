#ifndef LANG_MATH_PREFIX_H
#define LANG_MATH_PREFIX_H

/* Forced include for every vendored musl math C translation unit.  It gives
 * the isolated copies a private namespace so they can never interpose on the
 * host test's std::sin/std::pow/etc.  The public ABI names are lang_math_*;
 * internal kernels and data also get a lang_math_internal_* name. */

/* Select the non-FMA table variants even if the target compiler advertises
 * hardware FMA.  The runtime build uses -ffp-contract=off and the fma
 * implementation must be the explicit vendored algorithm, not an implicit
 * contraction in log/pow/exp. */
#undef __FP_FAST_FMA
#undef __FP_FAST_FMAF
#undef __FP_FAST_FMAL

/* Public ABI. */
#define fabs lang_math_fabs_v1
#define copysign lang_math_copysign_v1
#define floor lang_math_floor_v1
#define ceil lang_math_ceil_v1
#define trunc lang_math_trunc_v1
#define round lang_math_round_v1
#define sqrt lang_math_sqrt_v1
#define hypot lang_math_hypot_v1
#define fma lang_math_fma_v1
#define sin lang_math_sin_v1
#define cos lang_math_cos_v1
#define tan lang_math_tan_v1
#define asin lang_math_asin_v1
#define acos lang_math_acos_v1
#define atan lang_math_atan_v1
#define atan2 lang_math_atan2_v1
#define exp lang_math_exp_v1
#define exp2 lang_math_exp2_v1
#define expm1 lang_math_expm1_v1
#define log lang_math_log_v1
#define log2 lang_math_log2_v1
#define log10 lang_math_log10_v1
#define log1p lang_math_log1p_v1
#define pow lang_math_pow_v1
#define fmod lang_math_fmod_v1
#define remainder lang_math_remainder_v1

/* Implemented as exact-ABI wrappers in support.cpp. */
#define ldexp lang_math_internal_ldexp_v1
#define frexp lang_math_internal_frexp_v1

/* Internal helpers and kernels. */
#define scalbn lang_math_internal_scalbn_v1
#define remquo lang_math_internal_remquo_v1
#define __rem_pio2 lang_math_internal_rem_pio2_v1
#define __rem_pio2_large lang_math_internal_rem_pio2_large_v1
#define __sin lang_math_internal_sin_kernel_v1
#define __cos lang_math_internal_cos_kernel_v1
#define __tan lang_math_internal_tan_kernel_v1
#define __math_invalid lang_math_internal_math_invalid_v1
#define __math_uflow lang_math_internal_math_uflow_v1
#define __math_oflow lang_math_internal_math_oflow_v1
#define __math_divzero lang_math_internal_math_divzero_v1
#define __math_xflow lang_math_internal_math_xflow_v1

/* Data symbols shared by exp/exp2/pow/log/log2. */
#define __exp_data lang_math_internal_exp_data_v1
#define __log_data lang_math_internal_log_data_v1
#define __log2_data lang_math_internal_log2_data_v1
#define __pow_log_data lang_math_internal_pow_log_data_v1
#define __rsqrt_tab lang_math_internal_rsqrt_tab_v1

#endif
