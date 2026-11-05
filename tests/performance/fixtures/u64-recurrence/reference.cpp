// PERF-META width=64 seed=0x123456789abcdef0 iterations=30000000 expected=16012610758795272003
#include <cstdint>

static uint64_t mix(uint64_t x) {
    uint64_t y = x;
    y = y ^ (y >> 12);
    y = y ^ (y << 25);
    y = y ^ (y >> 27);
    return y * UINT64_C(2685821657736338717);
}

static uint64_t advance(uint64_t seed, uint64_t count) {
    uint64_t state = seed;
    for (uint64_t i = 0; i < count; ++i) {
        state = mix(state) + state * UINT64_C(6364136223846793005) + i;
    }
    return state;
}

int main() {
    const uint64_t actual = advance(UINT64_C(0x123456789abcdef0), UINT64_C(30000000));
    const uint64_t expected = UINT64_C(16012610758795272003);
    return actual == expected ? 0 : 1;
}
