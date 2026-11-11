// The Computer Language Benchmarks Game
// https://salsa.debian.org/benchmarksgame-team/benchmarksgame/
//
// Contributed by Andrei Simion (with patch from Vincent Yu)
// Based on "fannkuch-redux C++ g++ #5", contributed by Dave Compton,
// which in turn was based on the C program by Jeremy Zerfasm
// which in turn was based on the Ada program by Jonathan Parker and
// Georg Bauhaus which in turn was based on code by Dave Fladebo,
// Eckehard Berns, Heiner Marxen, Hongwei Xi, and The Anh Tran and
// also the Java program by Oleg Mazurov.
//
// Portable scalar single-thread rewrite for the PLB comparison: no SIMD,
// no OpenMP, and no block/thread partitioning. The three working arrays are
// sized from the runtime N, and the count-array permutation walk plus prefix
// flips keep the upstream order. The preparation-only visit hook is dropped.
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

struct FannkuchResult {
    int64_t checksum;
    int32_t maximum;
};

FannkuchResult fannkuch(int32_t n) {
    // The only three working arrays for one complete fannkuch call.
    std::vector<int32_t> perm(static_cast<std::size_t>(n), 0);
    std::vector<int32_t> perm1(static_cast<std::size_t>(n), 0);
    std::vector<int32_t> count(static_cast<std::size_t>(n), 0);

    int32_t max_flips = 0;
    int64_t checksum = 0;
    uint64_t permutation_index = 0;
    int32_t r = n;

    for (int32_t i = 0; i < n; ++i) {
        perm1[static_cast<std::size_t>(i)] = i;
    }

    for (;;) {
        while (r != 1) {
            count[static_cast<std::size_t>(r - 1)] = r;
            --r;
        }

        for (int32_t i = 0; i < n; ++i) {
            perm[static_cast<std::size_t>(i)] =
                perm1[static_cast<std::size_t>(i)];
        }

        int32_t flips = 0;
        while (perm[0] != 0) {
            const int32_t k = perm[0] + 1;
            int32_t left = 0;
            int32_t right = k - 1;
            while (left < right) {
                const int32_t saved = perm[static_cast<std::size_t>(left)];
                perm[static_cast<std::size_t>(left)] =
                    perm[static_cast<std::size_t>(right)];
                perm[static_cast<std::size_t>(right)] = saved;
                ++left;
                --right;
            }
            ++flips;
        }

        if (flips > max_flips) {
            max_flips = flips;
        }
        if ((permutation_index % 2) == 0) {
            checksum += static_cast<int64_t>(flips);
        } else {
            checksum -= static_cast<int64_t>(flips);
        }
        ++permutation_index;

        for (;;) {
            if (r == n) {
                break;
            }
            const int32_t perm0 = perm1[0];
            int32_t i = 0;
            while (i < r) {
                const int32_t j = i + 1;
                perm1[static_cast<std::size_t>(i)] =
                    perm1[static_cast<std::size_t>(j)];
                i = j;
            }
            perm1[static_cast<std::size_t>(r)] = perm0;
            --count[static_cast<std::size_t>(r)];
            if (count[static_cast<std::size_t>(r)] > 0) {
                break;
            }
            ++r;
        }
        if (r == n) {
            break;
        }
    }

    return FannkuchResult{checksum, max_flips};
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: fannkuch-redux N\n");
        return 2;
    }

    errno = 0;
    char* end = nullptr;
    const long long value = std::strtoll(argv[1], &end, 10);
    if (end == argv[1] || *end != '\0' || errno == ERANGE || value < 1 ||
        value > 16) {
        std::fprintf(stderr,
                     "fannkuch-redux: N must be an integer between 1 and 16\n");
        return 2;
    }

    const int32_t n = static_cast<int32_t>(value);
    const FannkuchResult result = fannkuch(n);
    std::printf("%lld\nPfannkuchen(%d) = %d\n",
                static_cast<long long>(result.checksum), static_cast<int>(n),
                static_cast<int>(result.maximum));
    return 0;
}
