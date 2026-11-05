// PERF-META width=32 seed=0x6d2b79f5 iterations=30000000 expected=468995906
#include <cstdint>

static uint32_t mix32(uint32_t x) {
    uint32_t y = x * UINT32_C(1664525) + UINT32_C(1013904223);
    y = y ^ (y >> 13);
    y = y * UINT32_C(2654435761);
    y = y ^ (y << 7);
    return y;
}

static uint32_t accumulate(uint32_t seed, uint32_t count) {
    uint32_t state = seed;
    uint32_t acc = 0;
    for (uint32_t i = 0; i < count; ++i) {
        state = mix32(state);
        if ((state & UINT32_C(65535)) < UINT32_C(32768)) {
            acc = acc + state;
        } else {
            acc = acc ^ state;
        }
        if ((state & UINT32_C(7)) == 0) {
            acc = acc + i;
        }
    }
    return acc ^ state;
}

int main() {
    const uint32_t actual = accumulate(UINT32_C(0x6d2b79f5), UINT32_C(30000000));
    const uint32_t expected = UINT32_C(468995906);
    return actual == expected ? 0 : 1;
}
