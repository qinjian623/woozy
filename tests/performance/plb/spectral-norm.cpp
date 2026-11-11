// The Computer Language Benchmarks Game
// https://salsa.debian.org/benchmarksgame-team/benchmarksgame/
//
// Original C contributed by Sebastien Loisel
// Conversion to C++ by Jon Harrop
// OpenMP parallelize by The Anh Tran
// Add SSE by The Anh Tran
// Additional SSE optimization by Krzysztof Jakubowski
// Converted to AVX and rewritten by Bela Pecsek
//
// Portable scalar single-thread rewrite for the PLB comparison: no OpenMP,
// no intrinsics, and no N x N matrix. Three N-element std::vector<double>
// arrays hold u, v, and the intermediate vector. The denominator recurrence
// keeps the EvalA/EvalAt order of 8.cpp, so both implementations use the same
// scalar operation sequence. Automatic compiler optimization is left
// unchanged; no noalias, unrolling, or fast-math attribute is added.
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

// dst = A * src, where A(i, j) = 1 / ((i+j)*(i+j+1)/2 + i + 1).
// For a fixed row i the denominator grows by i+j, matching 8.cpp's EvalA.
void eval_atimes_u(const std::vector<double>& src, std::vector<double>& dst,
                   std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        const double fi = static_cast<double>(i);
        double denominator = fi * (fi + 1.0) * 0.5 + fi + 1.0;
        double sum = src[0] / denominator;
        for (std::size_t j = 1; j < n; ++j) {
            denominator = denominator + fi + static_cast<double>(j);
            sum += src[j] / denominator;
        }
        dst[i] = sum;
    }
}

// dst = A^T * src: the same matrix with the row and column arguments swapped.
void eval_at_times_u(const std::vector<double>& src, std::vector<double>& dst,
                     std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        const double fi = static_cast<double>(i);
        double denominator = fi * (fi + 1.0) * 0.5 + 1.0;
        double sum = src[0] / denominator;
        for (std::size_t j = 1; j < n; ++j) {
            denominator = denominator + (fi + 1.0) + static_cast<double>(j);
            sum += src[j] / denominator;
        }
        dst[i] = sum;
    }
}

double spectral_norm(std::size_t n) {
    std::vector<double> u(n, 0.0);
    std::vector<double> v(n, 0.0);
    std::vector<double> tmp(n, 0.0);

    for (std::size_t i = 0; i < n; ++i) {
        u[i] = 1.0;
    }

    for (int ite = 0; ite < 10; ++ite) {
        eval_atimes_u(u, tmp, n);
        eval_at_times_u(tmp, v, n);
        eval_atimes_u(v, tmp, n);
        eval_at_times_u(tmp, u, n);
    }

    double vbv = 0.0;
    double vv = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        vbv += u[i] * v[i];
        vv += v[i] * v[i];
    }
    return std::sqrt(vbv / vv);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: spectral-norm N\n");
        return 2;
    }

    errno = 0;
    char* end = nullptr;
    const long long value = std::strtoll(argv[1], &end, 10);
    if (end == argv[1] || *end != '\0' || errno == ERANGE || value < 1 ||
        value > 1000000) {
        std::fprintf(stderr,
                     "spectral-norm: N must be an integer between 1 and 1000000\n");
        return 2;
    }

    std::printf("%.17g\n", spectral_norm(static_cast<std::size_t>(value)));
    return 0;
}
