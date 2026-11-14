#include "start.hpp"
#include <asm/prctl.h>
#include <asm/unistd.h>
#include <cstddef>

extern "C" int main();
extern "C" uintptr_t __stack_chk_guard;
extern "C" void lang_process_init_v1(uint64_t, const char *const *,
                                      const char *const *) noexcept __attribute__((weak));

#if defined(__x86_64__) && defined(__linux__)
alignas(64) static uintptr_t initial_tcb[8];

__attribute__((no_stack_protector, noreturn)) static void startup_failure() noexcept {
  static constexpr char message[] = "LANG_NATIVE_STARTUP_ERROR\n";
  size_t written = 0;
  while (written < sizeof(message) - 1) {
    long result;
    __asm__ volatile("syscall" : "=a"(result)
      : "a"(__NR_write), "D"(2), "S"(message + written), "d"(sizeof(message) - 1 - written)
      : "rcx", "r11", "memory");
    if (result == -4) continue;
    if (result <= 0) break;
    written += static_cast<size_t>(result);
  }
  __asm__ volatile("syscall" : : "a"(__NR_exit_group), "D"(127) : "rcx", "r11", "memory");
  __builtin_trap();
}

// Private hidden hooks used by gc_world/thread. They are deliberately not part
// of the public _v1 ABI. A hosted link has no start.cpp and therefore leaves
// its weak references null.
extern "C" __attribute__((no_stack_protector, visibility("hidden")))
uintptr_t lang_native_cached_tid() noexcept {
  uintptr_t cookie;
  __asm__ volatile("movq %%fs:0x10, %0" : "=r"(cookie));
  if (cookie != lang_runtime::kTcbTidCacheCookie) {
    // Never write a foreign pthread/libc TLS block. The caller still gets the
    // raw tid so a core program can use externally-created threads.
    long tid;
    __asm__ volatile("syscall" : "=a"(tid) : "a"(__NR_gettid)
                     : "rcx", "r11", "memory");
    return tid > 0 ? static_cast<uintptr_t>(tid) : 0;
  }
  uintptr_t cached;
  __asm__ volatile("movq %%fs:0x8, %0" : "=r"(cached));
  if (cached > 0 && cached <= 0x7fffffffU) return cached;
  long tid;
  __asm__ volatile("syscall" : "=a"(tid) : "a"(__NR_gettid)
                   : "rcx", "r11", "memory");
  if (tid <= 0) return 0;
  const uintptr_t value = static_cast<uintptr_t>(tid);
  __asm__ volatile("movq %0, %%fs:0x8" : : "r"(value) : "memory");
  return value;
}

extern "C" __attribute__((no_stack_protector, visibility("hidden")))
void lang_native_clear_tid_cache() noexcept {
  uintptr_t cookie;
  __asm__ volatile("movq %%fs:0x10, %0" : "=r"(cookie));
  if (cookie != lang_runtime::kTcbTidCacheCookie) return;
  __asm__ volatile("movq $0, %%fs:0x8" ::: "memory");
}

extern "C" __attribute__((no_stack_protector, visibility("hidden")))
void lang_init_tcb(const uintptr_t *stack) noexcept {
  const uintptr_t canary = lang_runtime::startup_canary(stack);
  if (!canary) startup_failure();
  initial_tcb[0] = reinterpret_cast<uintptr_t>(initial_tcb);
  initial_tcb[1] = 0;
  initial_tcb[2] = lang_runtime::kTcbTidCacheCookie;
  initial_tcb[5] = canary; // x86_64 stack protector reads FS:0x28.
  __stack_chk_guard = canary;
  long result;
  __asm__ volatile("syscall" : "=a"(result)
    : "a"(__NR_arch_prctl), "D"(ARCH_SET_FS), "S"(initial_tcb)
    : "rcx", "r11", "memory");
  if (result != 0) startup_failure();
  if (lang_process_init_v1) {
    auto *argv = reinterpret_cast<const char *const *>(stack + 1);
    lang_process_init_v1(*stack, argv, argv + *stack + 1);
  }
}

extern "C" __attribute__((naked, no_stack_protector, visibility("default"))) void _start() {
  __asm__ volatile(
    "xorl %%ebp, %%ebp\n\t"
    "cld\n\t"
    "movq %%rsp, %%rdi\n\t"
    "andq $-16, %%rsp\n\t"
    "call lang_init_tcb\n\t"
    "call main\n\t"
    "movl %%eax, %%edi\n\t"
    "movl $231, %%eax\n\t"
    "syscall\n\t"
    "movl $60, %%eax\n\t"
    "syscall\n\t"
    "ud2\n\t"
    ::: "memory");
}
#endif
