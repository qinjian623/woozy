#include "start.hpp"

#include <cstddef>
#include <cstdint>

// The hosted runtime deliberately relies on the platform C runtime:
// crt1.o/libc call __libc_start_main, which runs .init_array and then the
// program's ordinary main.  This file only forwards argc/argv/envp to the
// existing native runtime.  It does not define _start, set FS, write
// __stack_chk_guard, or install a private TCB.
// This strong dependency also extracts the hosted process/fork object from
// the runtime archive, even when the program only calls allocator/Vec APIs.
extern "C" void lang_process_init_v1(uint64_t, const char *const *,
                                     const char *const *) noexcept;

// Exported by glibc's dynamic loader and still points at the initial process
// stack during constructor execution.  It is weak so the object can also be
// inspected outside a glibc link; the explicit helper remains usable then.
extern "C" void *__libc_stack_end __attribute__((weak));
// libc's environment vector, used only as a fallback when the stack pointer is
// unavailable.  It is intentionally not used to obtain argv.
extern "C" char **environ __attribute__((weak));

namespace {
void publish(const char *const *stack) noexcept {
  if (!stack) return;
  const uint64_t argc = *reinterpret_cast<const uint64_t *>(stack);
  const auto *argv = reinterpret_cast<const char *const *>(stack + 1);
  const auto *environment = argv + argc + 1;
  lang_process_init_v1(argc, argv, environment);
}

void publish_fallback() noexcept {
  const auto *environment =
      &environ && environ ? const_cast<const char *const *>(environ) : nullptr;
  lang_process_init_v1(0, nullptr, environment);
}
} // namespace

extern "C" void lang_hosted_init_v1(int argc, char **argv,
                                    char **environment) noexcept {
  lang_process_init_v1(static_cast<uint64_t>(argc),
                       const_cast<const char *const *>(argv),
                       const_cast<const char *const *>(environment));
}

// Kept for the native startup helper name.  It has no TCB/FS side effects:
// hosted threads and the main thread use the C library's own TLS and stack
// protector state.
extern "C" void lang_init_tcb(const uintptr_t *initial_stack) noexcept {
  if (!initial_stack) return;
  publish(reinterpret_cast<const char *const *>(initial_stack));
}

// The generated program defines a normal main with no parameters.  This
// constructor is linked in by the hosted link mode and captures the vectors
// before main calls the language entry.
extern "C" __attribute__((constructor(101), used, visibility("default")))
void lang_hosted_startup_v1() noexcept {
  if (&__libc_stack_end && __libc_stack_end)
    publish(reinterpret_cast<const char *const *>(__libc_stack_end));
  else
    publish_fallback();
}
