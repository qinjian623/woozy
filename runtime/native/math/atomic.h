#ifndef LANG_MATH_ATOMIC_H
#define LANG_MATH_ATOMIC_H

#include <stdint.h>

/* fma.c only needs musl's a_clz_64 helper.  Use the compiler intrinsic instead
 * of pulling in the architecture atomic headers. */
static __inline int a_clz_64(uint64_t value) {
  return value ? __builtin_clzll(value) : 64;
}

#endif
