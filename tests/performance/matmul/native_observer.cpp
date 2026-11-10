// Test-only Native observer for the empty Source visit(Vec[f64]) function.
//
// prepare.py identifies the exact visit SymbolId and its real one-pointer LLVM
// argument, injects a call to matmul_observe in the pre-optimization IR, runs
// the LLVM verifier, then links this file at O0 and O2.  The observer reads the
// actual LangVec[f64] through the recorded runtime C ABI and prints every full
// element as a 16-hex-digit unsigned 64-bit IEEE-754 bit pattern.  It never
// recomputes the matmul and is not a formal CLI program: it depends on the host
// libc and is not used for any performance conclusion.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "vec.hpp"

static_assert(sizeof(double) == sizeof(uint64_t),
              "observer requires 64-bit binary64");

extern "C" void matmul_observe(LangVec *values) {
    if (values == nullptr) {
        std::fputs("matmul_observe: null Vec\n", stderr);
        std::_Exit(97);
    }
    const uint64_t length = lang_vec_length_v1(values);
    for (uint64_t index = 0; index < length; ++index) {
        const auto *element =
            static_cast<const uint8_t *>(lang_vec_at_v1(values, index));
        if (element == nullptr) {
            std::fputs("matmul_observe: null element\n", stderr);
            std::_Exit(98);
        }
        double value = 0.0;
        std::memcpy(&value, element, sizeof value);
        uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof bits);
        std::printf("%016llx", static_cast<unsigned long long>(bits));
    }
    std::printf("\n");
}
