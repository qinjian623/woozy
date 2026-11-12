// No-libc static smoke test for the private callback enter/leave ABI.

#include "gc_world.hpp"

#include <cstdint>

namespace {

constexpr long kSysWrite = 1;
constexpr long kSysExitGroup = 231;
constexpr int kEintr = -4;

void write_all(const char *bytes, uint64_t size) noexcept {
  while (size) {
    long result;
    __asm__ volatile("syscall" : "=a"(result)
      : "a"(kSysWrite), "D"(2), "S"(bytes), "d"(size)
      : "rcx", "r11", "memory");
    if (result == kEintr)
      continue;
    if (result <= 0)
      return;
    bytes += result;
    size -= static_cast<uint64_t>(result);
  }
}

[[noreturn]] void fail(const char *reason) noexcept {
  write_all("GC_WORLD_CALLBACK_STATIC_FAIL:", sizeof("GC_WORLD_CALLBACK_STATIC_FAIL:") - 1);
  uint64_t length = 0;
  while (reason[length])
    ++length;
  write_all(reason, length);
  write_all("\n", 1);
  __asm__ volatile("syscall" : : "a"(kSysExitGroup), "D"(1)
                   : "rcx", "r11", "memory");
  __builtin_unreachable();
}

} // namespace

extern "C" int gc_callback_static_main() noexcept {
  if (!lang_gc_world_register_v1())
    fail("world-register");
  if (!lang_gc_world_can_collect_v1())
    fail("can-collect");

  lang_gc_world_enter_safe_v1();
  lang_gc_world_enter_safe_v1();
  LangGcWorldCallbackToken token{};
  lang_gc_world_callback_enter_v1(&token);
  if (token.created_registration != 0)
    fail("unexpected-created-registration");
  if (token.saved_safe_depth != 2)
    fail("saved-safe-depth");
  lang_gc_world_callback_leave_v1(&token);
  if (lang_gc_world_can_collect_v1())
    fail("safe-depth-not-restored");
  lang_gc_world_leave_safe_v1();
  if (lang_gc_world_can_collect_v1())
    fail("safe-depth-not-restored-2");
  lang_gc_world_leave_safe_v1();
  if (!lang_gc_world_can_collect_v1())
    fail("not-active-after-safe");

  LangGcWorldCallbackToken active{};
  lang_gc_world_callback_enter_v1(&active);
  if (active.saved_safe_depth != 0)
    fail("active-saved-depth");
  if (lang_gc_world_stop_v1(0) != -16)
    fail("callback-became-collector");
  if (lang_gc_world_poll_required_v1())
    fail("callback-busy-set-request");
  lang_gc_world_callback_leave_v1(&active);

  if (lang_gc_world_stop_v1(0) != 0)
    fail("stop");
  lang_gc_world_resume_v1();
  if (!lang_gc_world_unregister_v1())
    fail("unregister");

  write_all("GC_WORLD_CALLBACK_STATIC_OK\n",
            sizeof("GC_WORLD_CALLBACK_STATIC_OK\n") - 1);
  return 0;
}

extern "C" __attribute__((naked, no_stack_protector, noreturn)) void
_start() noexcept {
  __asm__ volatile(
      "xorl %%ebp, %%ebp\n\t"
      "andq $-16, %%rsp\n\t"
      "call gc_callback_static_main\n\t"
      "movl %%eax, %%edi\n\t"
      "movl $231, %%eax\n\t"
      "syscall\n\t"
      "movl $60, %%eax\n\t"
      "syscall\n\t"
      "ud2\n\t"
      ::: "memory");
}
