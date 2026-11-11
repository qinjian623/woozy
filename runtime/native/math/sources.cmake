# Manifest for the isolated musl double-precision math sources.
#
# Root enables C, includes this file from the parent CMakeLists, and calls
#   enable_language(C)
#   include(${LANG_ROOT}/runtime/native/math/sources.cmake)
#   lang_native_math_sources(lang_native_runtime)
# The C sources are compiled as C with the isolated include path and forced
# rename prefix; support.cpp is compiled as C++ with the parent target's
# C++20 settings.  No vendor include directory is added to the parent target.

set(LANG_MATH_VENDOR_DIR "${CMAKE_CURRENT_LIST_DIR}/vendor")

set(LANG_MATH_C_SOURCES
  ${LANG_MATH_VENDOR_DIR}/fabs.c
  ${LANG_MATH_VENDOR_DIR}/copysign.c
  ${LANG_MATH_VENDOR_DIR}/floor.c
  ${LANG_MATH_VENDOR_DIR}/ceil.c
  ${LANG_MATH_VENDOR_DIR}/trunc.c
  ${LANG_MATH_VENDOR_DIR}/round.c
  ${LANG_MATH_VENDOR_DIR}/sqrt.c
  ${LANG_MATH_VENDOR_DIR}/hypot.c
  ${LANG_MATH_VENDOR_DIR}/fma.c
  ${LANG_MATH_VENDOR_DIR}/sin.c
  ${LANG_MATH_VENDOR_DIR}/cos.c
  ${LANG_MATH_VENDOR_DIR}/tan.c
  ${LANG_MATH_VENDOR_DIR}/asin.c
  ${LANG_MATH_VENDOR_DIR}/acos.c
  ${LANG_MATH_VENDOR_DIR}/atan.c
  ${LANG_MATH_VENDOR_DIR}/atan2.c
  ${LANG_MATH_VENDOR_DIR}/exp.c
  ${LANG_MATH_VENDOR_DIR}/exp2.c
  ${LANG_MATH_VENDOR_DIR}/expm1.c
  ${LANG_MATH_VENDOR_DIR}/log.c
  ${LANG_MATH_VENDOR_DIR}/log2.c
  ${LANG_MATH_VENDOR_DIR}/log10.c
  ${LANG_MATH_VENDOR_DIR}/log1p.c
  ${LANG_MATH_VENDOR_DIR}/pow.c
  ${LANG_MATH_VENDOR_DIR}/fmod.c
  ${LANG_MATH_VENDOR_DIR}/remainder.c
  ${LANG_MATH_VENDOR_DIR}/remquo.c
  ${LANG_MATH_VENDOR_DIR}/ldexp.c
  ${LANG_MATH_VENDOR_DIR}/scalbn.c
  ${LANG_MATH_VENDOR_DIR}/frexp.c
  ${LANG_MATH_VENDOR_DIR}/__sin.c
  ${LANG_MATH_VENDOR_DIR}/__cos.c
  ${LANG_MATH_VENDOR_DIR}/__tan.c
  ${LANG_MATH_VENDOR_DIR}/__rem_pio2.c
  ${LANG_MATH_VENDOR_DIR}/__rem_pio2_large.c
  ${LANG_MATH_VENDOR_DIR}/__math_invalid.c
  ${LANG_MATH_VENDOR_DIR}/__math_uflow.c
  ${LANG_MATH_VENDOR_DIR}/__math_oflow.c
  ${LANG_MATH_VENDOR_DIR}/__math_divzero.c
  ${LANG_MATH_VENDOR_DIR}/__math_xflow.c
  ${LANG_MATH_VENDOR_DIR}/exp_data.c
  ${LANG_MATH_VENDOR_DIR}/log_data.c
  ${LANG_MATH_VENDOR_DIR}/log2_data.c
  ${LANG_MATH_VENDOR_DIR}/pow_data.c
  ${LANG_MATH_VENDOR_DIR}/sqrt_data.c)

set(LANG_MATH_CXX_SOURCES
  ${CMAKE_CURRENT_LIST_DIR}/support.cpp)

# These flags are deliberately source-scoped.  In particular, -std=c99,
# -ffreestanding, the prefix header, and the isolated -I path must not affect
# the parent C++ target or the host oracle test.
set(LANG_MATH_C_FLAGS
  -std=c99
  -ffreestanding
  -fno-builtin
  -fno-common
  -fno-strict-aliasing
  -fno-math-errno
  -fno-trapping-math
  -fno-fast-math
  -ffp-contract=off
  -Wno-parentheses
  -Wno-shift-op-parentheses
  -Wno-unknown-pragmas
  -I${CMAKE_CURRENT_LIST_DIR}
  -include
  ${CMAKE_CURRENT_LIST_DIR}/math_prefix.h)

function(lang_native_math_sources target)
  target_sources(${target} PRIVATE
    ${LANG_MATH_CXX_SOURCES}
    ${LANG_MATH_C_SOURCES})
  set_source_files_properties(${LANG_MATH_C_SOURCES} PROPERTIES
    LANGUAGE C
    COMPILE_OPTIONS "${LANG_MATH_C_FLAGS}")
endfunction()
