// Strict floating fixture oracle.  This is a test-only tool: it links to the
// externally installed LLVM 22.1.8 APFloat and is never linked into langc.
//
// Every decimal literal is parsed directly at the target precision.  Every
// arithmetic step explicitly requests round-to-nearest, ties-to-even.  No
// host float/double conversion is used to derive an f32/f64 result.

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

using namespace llvm;

namespace {
constexpr auto RM = APFloat::rmNearestTiesToEven;

template <typename Sems> APFloat parse(const Sems &semantics, StringRef text) {
  APFloat value(semantics);
  auto status = value.convertFromString(text, RM);
  if (!status) {
    errs() << "APFloat parse failed for " << text << ": "
           << toString(status.takeError()) << "\n";
    std::exit(2);
  }
  return value;
}

std::string bits(const APFloat &value) {
  APInt raw = value.bitcastToAPInt();
  SmallString<32> text;
  raw.toStringUnsigned(text, 16);
  unsigned width = APFloatBase::getSizeInBits(value.getSemantics()) / 4;
  while (text.size() < width)
    text.insert(text.begin(), '0');
  std::string result(text.begin(), text.end());
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return result;
}

std::string category(const APFloat &value) {
  if (value.isNaN())
    return "nan";
  if (value.isInfinity())
    return "infinity";
  if (value.isZero())
    return "zero";
  return "finite";
}

std::string sign(const APFloat &value) {
  if (value.isNaN())
    return "-";
  return value.isNegative() ? "negative" : "positive";
}

void require_bits(const APFloat &value, std::string_view expected,
                  std::string_view label) {
  if (bits(value) != expected) {
    errs() << "oracle input check failed: " << label << " got " << bits(value)
           << " expected " << expected << "\n";
    std::exit(3);
  }
}

void require_nan(const APFloat &value, std::string_view label) {
  if (!value.isNaN()) {
    errs() << "oracle expected NaN for " << label << " got " << bits(value)
           << "\n";
    std::exit(3);
  }
}

void row(std::string_view project, std::string_view probe, std::string_view type,
         const APFloat &value, std::string_view expression,
         std::string_view note = "") {
  std::cout << project << '\t' << probe << '\t' << type << '\t'
            << category(value) << '\t';
  // NaN payload/bit pattern is deliberately not promised or compared.
  if (value.isNaN())
    std::cout << '-';
  else
    std::cout << bits(value);
  std::cout << '\t' << sign(value) << '\t' << expression << '\t' << note
            << '\n';
}

void bool_row(std::string_view project, std::string_view probe,
              bool value, std::string_view expression) {
  std::cout << project << '\t' << probe << "\tbool\tbool\t"
            << (value ? "true" : "false") << "\t-\t" << expression << "\t\n";
}

// A decimal parsed directly at f32 must be distinguished from the forbidden
// strategy that first parses it as f64 and then converts to f32.  The expected
// row is the direct f32 result; the f64-first result is recorded in the note.
void double_rounding_row(std::string_view project, std::string_view probe,
                         StringRef decimal, std::string_view direct_bits,
                         std::string_view f64_first_bits) {
  APFloat direct = parse(APFloat::IEEEsingle(), decimal);
  require_bits(direct, direct_bits, "direct f32 parse");
  APFloat via_f64 = parse(APFloat::IEEEdouble(), decimal);
  const std::string f64_bits = bits(via_f64);
  bool lost = false;
  via_f64.convert(APFloat::IEEEsingle(), RM, &lost);
  require_bits(via_f64, f64_first_bits, "f64-first wrong strategy");
  if (bits(direct) == bits(via_f64)) {
    errs() << "double-rounding trap did not differ for " << decimal << "\n";
    std::exit(3);
  }
  row(project, probe, "f32", direct,
      std::string("direct f32 literal; f64-first=") +
          std::string(f64_first_bits) + "; f64-intermediate=" + f64_bits +
          " (wrong strategy)",
      "direct f32 parse is the only expected value");
}

bool eq(const APFloat &left, const APFloat &right) {
  return left.compare(right) == APFloatBase::cmpEqual;
}
bool ne(const APFloat &left, const APFloat &right) {
  return left.compare(right) != APFloatBase::cmpEqual;
}
bool lt(const APFloat &left, const APFloat &right) {
  return left.compare(right) == APFloatBase::cmpLessThan;
}
bool le(const APFloat &left, const APFloat &right) {
  auto result = left.compare(right);
  return result == APFloatBase::cmpLessThan ||
         result == APFloatBase::cmpEqual;
}
bool gt(const APFloat &left, const APFloat &right) {
  return left.compare(right) == APFloatBase::cmpGreaterThan;
}
bool ge(const APFloat &left, const APFloat &right) {
  auto result = left.compare(right);
  return result == APFloatBase::cmpGreaterThan ||
         result == APFloatBase::cmpEqual;
}
} // namespace

int main() {
  std::cout << "# Strict floating oracle, LLVM " << LLVM_VERSION_STRING
            << " APFloat, all operations rmNearestTiesToEven\n";
  std::cout << "# project\tprobe\ttype\tclass\tbits\tsign\texpression\tnote\n";

  const auto &f32 = APFloat::IEEEsingle();
  const auto &f64 = APFloat::IEEEdouble();
  const APFloat one32 = parse(f32, "1.0");
  const APFloat two32 = parse(f32, "2.0");
  const APFloat one64 = parse(f64, "1.0");
  const APFloat two64 = parse(f64, "2.0");
  const APFloat one_and_half32 = parse(f32, "1.5");
  const APFloat one_and_half64 = parse(f64, "1.5");
  const APFloat zero32 = APFloat::getZero(f32);
  const APFloat zero64 = APFloat::getZero(f64);

  // RNE ties-to-even, f32.
  {
    const auto project = "f32-round-ties-even";
    APFloat half = parse(f32, "0.000000059604644775390625"); // 2^-24
    APFloat down = one32;
    down.add(half, RM);
    row(project, "tie_down_add", "f32", down, "1.0 + 2^-24", "");

    APFloat three_halves = parse(f32, "0.000000178813934326171875"); // 3*2^-24
    APFloat up = one32;
    up.add(three_halves, RM);
    row(project, "tie_up_add", "f32", up, "1.0 + 3*2^-24", "");

    APFloat literal_down = parse(f32, "1.000000059604644775390625");
    row(project, "tie_down_literal", "f32", literal_down,
        "literal 1+2^-24", "");
    APFloat literal_up = parse(f32, "1.000000178813934326171875");
    row(project, "tie_up_literal", "f32", literal_up,
        "literal 1+3*2^-24", "");

    // These two decimals are not exactly representable as f64.  Direct f32
    // parsing rounds both to 3f800001; f64-first parsing rounds them to the
    // adjacent f32 ties and then rounds to 3f800000 / 3f800002 respectively.
    double_rounding_row(
        project, "literal_above_lower_midpoint",
        "1.0000000596046447753906250000000000000000001", "3f800001",
        "3f800000");
    double_rounding_row(
        project, "literal_below_upper_midpoint",
        "1.0000001788139343261718749999999999999999999", "3f800001",
        "3f800002");
  }

  // RNE ties-to-even, f64.
  {
    const auto project = "f64-round-ties-even";
    APFloat half =
        parse(f64, "0.00000000000000011102230246251565404236316680908203125");
    APFloat down = one64;
    down.add(half, RM);
    row(project, "tie_down_add", "f64", down, "1.0 + 2^-53", "");

    APFloat three_halves = parse(
        f64,
        "0.00000000000000033306690738754696212708950042724609375");
    APFloat up = one64;
    up.add(three_halves, RM);
    row(project, "tie_up_add", "f64", up, "1.0 + 3*2^-53", "");

    APFloat literal_down = parse(
        f64, "1.00000000000000011102230246251565404236316680908203125");
    row(project, "tie_down_literal", "f64", literal_down,
        "literal 1+2^-53", "");
    APFloat literal_up = parse(
        f64, "1.00000000000000033306690738754696212708950042724609375");
    row(project, "tie_up_literal", "f64", literal_up,
        "literal 1+3*2^-53", "");
  }

  // Overflow to infinity.
  {
    const auto project = "f32-overflow";
    APFloat max_finite =
        parse(f32, "3.40282346638528859811704183484516925440e38");
    require_bits(max_finite, "7f7fffff", "f32 max finite literal");
    APFloat positive = max_finite;
    positive.multiply(two32, RM);
    row(project, "positive_overflow", "f32", positive, "f32::MAX * 2.0", "");

    APFloat negative = max_finite;
    negative.changeSign();
    negative.multiply(two32, RM);
    row(project, "negative_overflow", "f32", negative, "(-f32::MAX) * 2.0", "");
  }
  {
    const auto project = "f64-overflow";
    APFloat max_finite = parse(f64, "1.7976931348623157e308");
    require_bits(max_finite, "7fefffffffffffff", "f64 max finite literal");
    APFloat positive = max_finite;
    positive.multiply(two64, RM);
    row(project, "positive_overflow", "f64", positive, "f64::MAX * 2.0", "");

    APFloat negative = max_finite;
    negative.changeSign();
    negative.multiply(two64, RM);
    row(project, "negative_overflow", "f64", negative, "(-f64::MAX) * 2.0", "");
  }

  // Subnormal results, ties in the subnormal range, and underflow to zero.
  {
    const auto project = "f32-subnormal";
    APFloat min_normal = parse(
        f32,
        "1.175494350822287507968736537222245677818665556772087521508751706278"
        "4172594547271728515625e-38");
    require_bits(min_normal, "00800000", "f32 min normal literal");
    APFloat min_subnormal = parse(
        f32,
        "1.401298464324817070923729583289916131280261941876515771757068283889"
        "79108268586060148663818836212158203125e-45");
    require_bits(min_subnormal, "00000001", "f32 min subnormal literal");

    APFloat halved_normal = min_normal;
    halved_normal.divide(two32, RM);
    row(project, "min_normal_halved", "f32", halved_normal,
        "f32::MIN_POSITIVE / 2.0", "");

    APFloat halved_subnormal = min_subnormal;
    halved_subnormal.divide(two32, RM);
    row(project, "min_subnormal_halved", "f32", halved_subnormal,
        "min_subnormal / 2.0", "exact half of min subnormal -> +0");

    APFloat tied = min_subnormal;
    tied.multiply(one_and_half32, RM);
    row(project, "subnormal_tie", "f32", tied, "min_subnormal * 1.5",
        "ties-to-even in subnormal range");

    APFloat negative_underflow = min_subnormal;
    negative_underflow.changeSign();
    negative_underflow.divide(two32, RM);
    row(project, "negative_underflow", "f32", negative_underflow,
        "(-min_subnormal) / 2.0", "exact half -> -0");
  }
  {
    const auto project = "f64-subnormal";
    APFloat min_normal = parse(f64, "2.2250738585072014e-308");
    require_bits(min_normal, "0010000000000000", "f64 min normal literal");
    APFloat min_subnormal = parse(f64, "4.9406564584124654e-324");
    require_bits(min_subnormal, "0000000000000001",
                 "f64 min subnormal literal");

    APFloat halved_normal = min_normal;
    halved_normal.divide(two64, RM);
    row(project, "min_normal_halved", "f64", halved_normal,
        "f64::MIN_POSITIVE / 2.0", "");

    APFloat halved_subnormal = min_subnormal;
    halved_subnormal.divide(two64, RM);
    row(project, "min_subnormal_halved", "f64", halved_subnormal,
        "min_subnormal / 2.0", "exact half of min subnormal -> +0");

    APFloat tied = min_subnormal;
    tied.multiply(one_and_half64, RM);
    row(project, "subnormal_tie", "f64", tied, "min_subnormal * 1.5",
        "ties-to-even in subnormal range");

    APFloat negative_underflow = min_subnormal;
    negative_underflow.changeSign();
    negative_underflow.divide(two64, RM);
    row(project, "negative_underflow", "f64", negative_underflow,
        "(-min_subnormal) / 2.0", "exact half -> -0");
  }

  // Signed zero, division by zero, NaN classification and comparisons, f32.
  {
    const auto project = "f32-zero-div-nan";
    APFloat positive_zero = APFloat::getZero(f32);
    row(project, "positive_zero", "f32", positive_zero, "0.0", "");
    APFloat negative_zero = positive_zero;
    negative_zero.changeSign();
    row(project, "negative_zero", "f32", negative_zero, "-0.0", "");

    APFloat opposite_sum = positive_zero;
    opposite_sum.add(negative_zero, RM);
    row(project, "opposite_zero_sum", "f32", opposite_sum, "+0.0 + -0.0",
        "RNE exact cancellation -> +0");
    APFloat negative_sum = negative_zero;
    negative_sum.add(negative_zero, RM);
    row(project, "negative_zero_sum", "f32", negative_sum, "-0.0 + -0.0", "");
    APFloat negative_subtract = negative_zero;
    negative_subtract.subtract(positive_zero, RM);
    row(project, "negative_zero_subtract", "f32", negative_subtract,
        "-0.0 - +0.0", "");
    APFloat negative_product = negative_zero;
    negative_product.multiply(one32, RM);
    row(project, "negative_zero_product", "f32", negative_product,
        "-0.0 * 1.0", "");
    APFloat negative_quotient = negative_zero;
    negative_quotient.divide(one32, RM);
    row(project, "negative_zero_quotient", "f32", negative_quotient,
        "-0.0 / 1.0", "");

    APFloat positive_infinity = one32;
    positive_infinity.divide(zero32, RM);
    row(project, "positive_infinity", "f32", positive_infinity, "1.0 / 0.0", "");
    APFloat negative_one = one32;
    negative_one.changeSign();
    APFloat negative_infinity_numerator = negative_one;
    negative_infinity_numerator.divide(zero32, RM);
    row(project, "negative_infinity_numerator", "f32",
        negative_infinity_numerator, "(-1.0) / 0.0", "");
    APFloat negative_infinity_divisor = one32;
    negative_infinity_divisor.divide(negative_zero, RM);
    row(project, "negative_infinity_divisor", "f32",
        negative_infinity_divisor, "1.0 / -0.0", "");

    APFloat nan_zero = zero32;
    nan_zero.divide(zero32, RM);
    require_nan(nan_zero, "f32 0/0");
    row(project, "nan_zero_over_zero", "f32", nan_zero, "0.0 / 0.0",
        "class only; payload not compared");

    APFloat nan_inf = positive_infinity;
    nan_inf.subtract(positive_infinity, RM);
    require_nan(nan_inf, "f32 Inf-Inf");
    row(project, "nan_inf_minus_inf", "f32", nan_inf, "Inf - Inf",
        "class only; payload not compared");

    const APFloat nan = nan_zero;
    bool_row(project, "nan_equal_self", eq(nan, nan), "NaN == NaN");
    bool_row(project, "nan_not_equal_self", ne(nan, nan), "NaN != NaN");
    bool_row(project, "nan_less_than_one", lt(nan, one32), "NaN < 1.0");
    bool_row(project, "one_less_than_nan", lt(one32, nan), "1.0 < NaN");
    bool_row(project, "nan_less_equal_one", le(nan, one32), "NaN <= 1.0");
    bool_row(project, "nan_greater_equal_one", ge(nan, one32), "NaN >= 1.0");
    bool_row(project, "nan_greater_than_one", gt(nan, one32), "NaN > 1.0");
    bool_row(project, "nan_equal_zero", eq(nan, zero32), "NaN == 0.0");

    // True ordered comparisons and signed-zero equality controls.  Without
    // these, an implementation that always returns false would pass all NaN
    // false probes above.
    bool_row(project, "finite_less_than", lt(one32, two32), "1.0 < 2.0");
    bool_row(project, "finite_less_equal", le(one32, one32), "1.0 <= 1.0");
    bool_row(project, "finite_greater_than", gt(two32, one32), "2.0 > 1.0");
    bool_row(project, "finite_greater_equal", ge(one32, one32), "1.0 >= 1.0");
    bool_row(project, "positive_zero_equal_negative_zero",
             eq(positive_zero, negative_zero), "+0.0 == -0.0");
    bool_row(project, "positive_zero_not_equal_negative_zero",
             ne(positive_zero, negative_zero), "+0.0 != -0.0");
  }

  // Signed zero, division by zero, NaN classification and comparisons, f64.
  {
    const auto project = "f64-zero-div-nan";
    APFloat positive_zero = APFloat::getZero(f64);
    row(project, "positive_zero", "f64", positive_zero, "0.0", "");
    APFloat negative_zero = positive_zero;
    negative_zero.changeSign();
    row(project, "negative_zero", "f64", negative_zero, "-0.0", "");

    APFloat opposite_sum = positive_zero;
    opposite_sum.add(negative_zero, RM);
    row(project, "opposite_zero_sum", "f64", opposite_sum, "+0.0 + -0.0",
        "RNE exact cancellation -> +0");
    APFloat negative_sum = negative_zero;
    negative_sum.add(negative_zero, RM);
    row(project, "negative_zero_sum", "f64", negative_sum, "-0.0 + -0.0", "");
    APFloat negative_subtract = negative_zero;
    negative_subtract.subtract(positive_zero, RM);
    row(project, "negative_zero_subtract", "f64", negative_subtract,
        "-0.0 - +0.0", "");
    APFloat negative_product = negative_zero;
    negative_product.multiply(one64, RM);
    row(project, "negative_zero_product", "f64", negative_product,
        "-0.0 * 1.0", "");
    APFloat negative_quotient = negative_zero;
    negative_quotient.divide(one64, RM);
    row(project, "negative_zero_quotient", "f64", negative_quotient,
        "-0.0 / 1.0", "");

    APFloat positive_infinity = one64;
    positive_infinity.divide(zero64, RM);
    row(project, "positive_infinity", "f64", positive_infinity, "1.0 / 0.0", "");
    APFloat negative_one = one64;
    negative_one.changeSign();
    APFloat negative_infinity_numerator = negative_one;
    negative_infinity_numerator.divide(zero64, RM);
    row(project, "negative_infinity_numerator", "f64",
        negative_infinity_numerator, "(-1.0) / 0.0", "");
    APFloat negative_infinity_divisor = one64;
    negative_infinity_divisor.divide(negative_zero, RM);
    row(project, "negative_infinity_divisor", "f64",
        negative_infinity_divisor, "1.0 / -0.0", "");

    APFloat nan_zero = zero64;
    nan_zero.divide(zero64, RM);
    require_nan(nan_zero, "f64 0/0");
    row(project, "nan_zero_over_zero", "f64", nan_zero, "0.0 / 0.0",
        "class only; payload not compared");

    APFloat nan_inf = positive_infinity;
    nan_inf.subtract(positive_infinity, RM);
    require_nan(nan_inf, "f64 Inf-Inf");
    row(project, "nan_inf_minus_inf", "f64", nan_inf, "Inf - Inf",
        "class only; payload not compared");

    const APFloat nan = nan_zero;
    bool_row(project, "nan_equal_self", eq(nan, nan), "NaN == NaN");
    bool_row(project, "nan_not_equal_self", ne(nan, nan), "NaN != NaN");
    bool_row(project, "nan_less_than_one", lt(nan, one64), "NaN < 1.0");
    bool_row(project, "one_less_than_nan", lt(one64, nan), "1.0 < NaN");
    bool_row(project, "nan_less_equal_one", le(nan, one64), "NaN <= 1.0");
    bool_row(project, "nan_greater_equal_one", ge(nan, one64), "NaN >= 1.0");
    bool_row(project, "nan_greater_than_one", gt(nan, one64), "NaN > 1.0");
    bool_row(project, "nan_equal_zero", eq(nan, zero64), "NaN == 0.0");

    bool_row(project, "finite_less_than", lt(one64, two64), "1.0 < 2.0");
    bool_row(project, "finite_less_equal", le(one64, one64), "1.0 <= 1.0");
    bool_row(project, "finite_greater_than", gt(two64, one64), "2.0 > 1.0");
    bool_row(project, "finite_greater_equal", ge(one64, one64), "1.0 >= 1.0");
    bool_row(project, "positive_zero_equal_negative_zero",
             eq(positive_zero, negative_zero), "+0.0 == -0.0");
    bool_row(project, "positive_zero_not_equal_negative_zero",
             ne(positive_zero, negative_zero), "+0.0 != -0.0");
  }

  // No implicit fused multiply-add.  The expected value is the separately
  // rounded product followed by a separately rounded add.  The note records
  // what a contracted FMA would have produced, which must not be observed.
  {
    const auto project = "f32-no-fma";
    APFloat a = parse(f32, "1.00000011920928955078125");   // 1 + 2^-23
    APFloat c = parse(f32, "-1.0000002384185791015625");  // -(1 + 2^-22)
    require_bits(a, "3f800001", "f32 no-fma multiplicand");
    require_bits(c, "bf800002", "f32 no-fma addend");
    APFloat product = a;
    product.multiply(a, RM);
    APFloat separate = product;
    separate.add(c, RM);
    APFloat fused = a;
    fused.fusedMultiplyAdd(a, c, RM);
    row(project, "separate_mul_add", "f32", separate, "a * b + c",
        std::string("separate RNE; fused-if-contracted=") + bits(fused));
  }
  {
    const auto project = "f64-no-fma";
    APFloat a = parse(
        f64,
        "1.0000000000000002220446049250313080847263336181640625"); // 1+2^-52
    APFloat c = parse(
        f64,
        "-1.000000000000000444089209850062616169452667236328125"); // -(1+2^-51)
    require_bits(a, "3ff0000000000001", "f64 no-fma multiplicand");
    require_bits(c, "bff0000000000002", "f64 no-fma addend");
    APFloat product = a;
    product.multiply(a, RM);
    APFloat separate = product;
    separate.add(c, RM);
    APFloat fused = a;
    fused.fusedMultiplyAdd(a, c, RM);
    row(project, "separate_mul_add", "f64", separate, "a * b + c",
        std::string("separate RNE; fused-if-contracted=") + bits(fused));
  }

  return 0;
}
