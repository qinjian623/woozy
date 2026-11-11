// The Computer Language Shootout
// http://shootout.alioth.debian.org/
// Precedent C entry modified by bearophile for speed and size, 31 Jan 2006
// Converted to C++ by Paul Kitchin
//
// Single-threaded comparison reference for nsieve.wz.  Upstream nsieve/1.cpp
// at commit ad18b203dd1769724f4eea94fc3ac1e99f6593e0 uses std::vector<bool>,
// whose bits are packed one per bit.  To keep the memory traffic comparable
// with the Woozy program, which packs bits in Vec[u8], this file uses an
// explicit std::vector<std::uint8_t> bitset with the same packing rule
// (word = value >> 3, bit = 1u << (value & 7)).
//
// Same sieve as upstream: composite test for value = 2 .. max-1, marking
// multiples starting at 2 * value.  The storage is reused across the three
// rounds and byte-cleared each round, matching the Woozy vec.resize + clear.
// Neither this file nor nsieve.wz enables fast-math or noalias.
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

const std::int64_t kBase = 10000;

// Packs one bit per value; only the first (max + 7) / 8 bytes are cleared and
// read, so unused high bits of the last byte are never observed.
std::uint64_t sieve(std::vector<std::uint8_t> &flags, std::int64_t max) {
    const std::int64_t bytes = (max + 7) / 8;
    if (static_cast<std::int64_t>(flags.size()) < bytes) {
        flags.resize(static_cast<std::size_t>(bytes));
    }
    for (std::int64_t index = 0; index < bytes; ++index) {
        flags[static_cast<std::size_t>(index)] = 0;
    }

    std::uint64_t count = 0;
    for (std::int64_t value = 2; value < max; ++value) {
        const std::int64_t word = value >> 3;
        const std::uint8_t mask = static_cast<std::uint8_t>(1u << (value & 7));
        if ((flags[static_cast<std::size_t>(word)] & mask) == 0) {
            ++count;
            for (std::int64_t multiple = value * 2; multiple < max;
                 multiple += value) {
                const std::int64_t target = multiple >> 3;
                const std::uint8_t bit =
                    static_cast<std::uint8_t>(1u << (multiple & 7));
                flags[static_cast<std::size_t>(target)] = static_cast<std::uint8_t>(
                    flags[static_cast<std::size_t>(target)] | bit);
            }
        }
    }
    return count;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: nsieve <n>\n");
        return 2;
    }
    errno = 0;
    char *end = nullptr;
    const long long parsed = std::strtoll(argv[1], &end, 10);
    // The three upstream rounds shift 10000 by n, n-1 and n-2; upstream
    // computes them in unsigned int, so n >= 2 is the well-defined domain.
    if (errno != 0 || end == argv[1] || *end != '\0' || parsed < 2 || parsed > 20) {
        std::fprintf(stderr, "nsieve: n must be an integer between 2 and 20\n");
        return 2;
    }
    const std::int64_t count = static_cast<std::int64_t>(parsed);

    std::vector<std::uint8_t> flags;
    for (std::int64_t i = 0; i < 3; ++i) {
        const std::int64_t max = kBase << (count - i);
        const std::uint64_t primes = sieve(flags, max);
        std::printf("Primes up to %lld %llu\n", static_cast<long long>(max),
                    static_cast<unsigned long long>(primes));
    }
    return 0;
}
