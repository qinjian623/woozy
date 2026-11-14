#pragma once
#include <cstdint>

namespace lang_runtime {
// Private TCB slots used only by the runtime's own FS base. Slot 1 caches the
// current kernel tid and slot 2 marks that the slots are owned by this runtime;
// external pthreads and hosted libc TLS never match this cookie.
inline constexpr uintptr_t kTcbTidCacheCookie = uintptr_t{0x575a544944763031ULL};
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
