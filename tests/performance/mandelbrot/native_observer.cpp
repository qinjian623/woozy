// Test-only Native observer for the empty Source visit(Vec[u8]) function.
//
// prepare.py identifies the exact visit SymbolId and its real one-pointer LLVM
// argument, injects a call to mandel_observe in the pre-optimization IR, runs
// the LLVM verifier, then links this file at O0 and O2.  The observer reads the
// actual LangVec through the recorded runtime C ABI and prints the complete
// bitmap as lowercase hex.  It never recomputes the Mandelbrot algorithm and is
// not a formal CLI program: it depends on the host libc and is not used for any
// performance conclusion.
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "vec.hpp"

extern "C" void mandel_observe(LangVec *bytes) {
    if (bytes == nullptr) {
        std::fputs("mandel_observe: null Vec\n", stderr);
        std::_Exit(97);
    }
    const uint64_t length = lang_vec_length_v1(bytes);
    for (uint64_t index = 0; index < length; ++index) {
        const auto *element =
            static_cast<const uint8_t *>(lang_vec_at_v1(bytes, index));
        if (element == nullptr) {
            std::fputs("mandel_observe: null element\n", stderr);
            std::_Exit(98);
        }
        std::printf("%02x", static_cast<unsigned>(*element));
    }
    std::printf("\n");
}
