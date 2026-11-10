// Test-only host observer for the auxiliary C++ full-matrix check.
//
// large_prepare.py makes a separate auxiliary copy of the formal C++ reference
// in which only the empty visit(const std::vector<double>&) body is replaced
// by a call to matmul_observe_cpp(values.data(), values.size()).  The formal
// C++ source, the formal C++ O2 program, and its in-program comparison are
// never changed.  This observer prints every full C element as a 16-hex-digit
// unsigned 64-bit IEEE-754 bit pattern, so +0.0 and -0.0 remain distinct; it
// never recomputes the matmul.  It depends on the host libc and is not a
// formal CLI or performance program.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static_assert(sizeof(double) == sizeof(std::uint64_t),
              "observer requires 64-bit binary64");

extern "C" void matmul_observe_cpp(const double *values, std::size_t length) {
    if (values == nullptr && length != 0) {
        std::fputs("matmul_observe_cpp: null values\n", stderr);
        std::_Exit(97);
    }
    for (std::size_t index = 0; index < length; ++index) {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &values[index], sizeof bits);
        std::printf("%016llx", static_cast<unsigned long long>(bits));
    }
    std::printf("\n");
}
