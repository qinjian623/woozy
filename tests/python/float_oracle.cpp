// Test-only LLVM 22.1.8 oracle. Never linked into woozyc or its runtime.
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Config/llvm-config.h"
#include <algorithm>
#include <cctype>
#include <iostream>
#include <string>

using namespace llvm;
int main(int argc, char **argv) {
  if (argc == 2 && std::string(argv[1]) == "--version") {
    std::cout << LLVM_VERSION_STRING << '\n';
    return 0;
  }
  unsigned width;
  std::string op, left, right;
  while (std::cin >> width >> op >> left >> right) {
    if (width != 32 && width != 64) return 2;
    const auto &sem = width == 32 ? APFloat::IEEEsingle() : APFloat::IEEEdouble();
    APFloat a(sem, APInt(width, left, 16)), b(sem, APInt(width, right, 16));
    auto rm = APFloat::rmNearestTiesToEven;
    if (op == "add") a.add(b, rm);
    else if (op == "sub") a.subtract(b, rm);
    else if (op == "mul") a.multiply(b, rm);
    else if (op == "div") a.divide(b, rm);
    else if (op == "neg") a.changeSign();
    else {
      auto cmp = a.compare(b);
      bool result;
      if (op == "eq") result = cmp == APFloat::cmpEqual;
      else if (op == "ne") result = cmp != APFloat::cmpEqual;
      else if (op == "lt") result = cmp == APFloat::cmpLessThan;
      else if (op == "le") result = cmp == APFloat::cmpLessThan || cmp == APFloat::cmpEqual;
      else if (op == "gt") result = cmp == APFloat::cmpGreaterThan;
      else if (op == "ge") result = cmp == APFloat::cmpGreaterThan || cmp == APFloat::cmpEqual;
      else return 3;
      std::cout << "bool\t" << (result ? "true" : "false") << "\t-\n";
      continue;
    }
    if (a.isNaN()) {
      std::cout << "nan\t-\t-\n";
      continue;
    }
    SmallString<32> raw;
    a.bitcastToAPInt().toStringUnsigned(raw, 16);
    std::string bits(raw.begin(), raw.end());
    std::transform(bits.begin(), bits.end(), bits.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    bits.insert(0, width / 4 - bits.size(), '0');
    std::cout << (a.isInfinity() ? "infinity" : a.isZero() ? "zero" : "finite")
              << '\t' << bits << '\t' << (a.isNegative() ? "negative" : "positive") << '\n';
  }
  return std::cin.eof() ? 0 : 4;
}
