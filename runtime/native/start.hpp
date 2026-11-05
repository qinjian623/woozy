#pragma once
#include <cstdint>

namespace lang_runtime {
// Linux places argc, argv, envp and the auxiliary vector on the initial stack.
__attribute__((no_stack_protector)) inline uintptr_t startup_canary(const uintptr_t *stack) noexcept {
  if (!stack) return 0;
  const uintptr_t *p = stack + 1 + *stack + 1;
  while (*p) ++p;
  ++p;
  for (; p[0] != 0; p += 2) {
    if (p[0] != 25) continue; // AT_RANDOM
    if (!p[1]) return 0;
    auto *random = reinterpret_cast<const unsigned char *>(p[1]);
    uintptr_t canary = 0;
    for (unsigned i = 0; i < sizeof(canary); ++i)
      canary |= uintptr_t(random[i]) << (i * 8);
    return canary;
  }
  return 0;
}
}
