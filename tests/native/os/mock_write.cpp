#include <stdint.h>

extern "C" int64_t __real_lang_os_write_v1(int32_t, const void *, uint64_t) noexcept;

namespace {
constexpr uint8_t vector_bytes[] = {'A', 0, 'B', 'C', '\n'};
constexpr uint8_t text_bytes[] = {'t', 'e', 'x', 't', 0, '!', '\n'};
constexpr uint8_t marker[] = {'M', 'O', 'C', 'K', '-', 'O', 'K', '\n'};
#if MOCK_MODE == 0
constexpr int64_t vector_steps[] = {-4, 2, 1, 2};
constexpr int64_t text_steps[] = {-4, 3, -4, 4};
#elif MOCK_MODE == 1
constexpr int64_t vector_steps[] = {-4, 2, 0};
constexpr int64_t text_steps[] = {-4, 3, 0};
#elif MOCK_MODE == 2
constexpr int64_t vector_steps[] = {-4, 1, 2, -11};
constexpr int64_t text_steps[] = {2, -4, -11};
#else
#error MOCK_MODE must be 0, 1 or 2
#endif

struct State { uint64_t step, completed; const uint8_t *base; } states[2];

[[noreturn]] void fail(int status) noexcept {
  __asm__ volatile("syscall" : : "a"(231L), "D"(status) : "rcx", "r11", "memory");
  __builtin_trap();
}

bool equal(const uint8_t *left, const uint8_t *right, uint64_t count) noexcept {
  for (uint64_t i = 0; i < count; ++i) if (left[i] != right[i]) return false;
  return true;
}
}

// Only this test binary simulates return values; positive bytes are really written.
extern "C" int64_t __wrap_lang_os_write_v1(int32_t fd, const void *pointer,
                                           uint64_t count) noexcept {
  if (fd != 1 && fd != 2) return __real_lang_os_write_v1(fd, pointer, count);
  auto *bytes = static_cast<const uint8_t *>(pointer);
  constexpr uint64_t vector_calls = sizeof(vector_steps) / sizeof(*vector_steps);
  constexpr uint64_t text_calls = sizeof(text_steps) / sizeof(*text_steps);
  if (fd == 2 && count == sizeof(marker) && equal(bytes, marker, sizeof(marker))) {
    if (states[0].step != vector_calls || states[1].step != text_calls) fail(206);
    return __real_lang_os_write_v1(fd, pointer, count);
  }
  State &state = states[fd - 1];
  const auto *expected = fd == 1 ? vector_bytes : text_bytes;
  const uint64_t length = fd == 1 ? sizeof(vector_bytes) : sizeof(text_bytes);
  const auto *steps = fd == 1 ? vector_steps : text_steps;
  if (state.step >= (fd == 1 ? vector_calls : text_calls)) fail(201);
  if (count != length - state.completed) fail(202);
  if (!state.step) state.base = bytes;
  if (bytes != state.base + state.completed) fail(203);
  if (!equal(bytes, expected + state.completed, count)) fail(204);
  const int64_t result = steps[state.step++];
  if (result > 0) {
    if (uint64_t(result) > count || __real_lang_os_write_v1(fd, pointer, uint64_t(result)) != result)
      fail(205);
    state.completed += uint64_t(result);
  }
  return result;
}
