// No-libc static ELF smoke test for lang-gc-world/1.
//
// It is compiled -ffreestanding, linked with ld -static and the runtime's own
// allocator/syscall substrate, and uses raw clone/futex/nanosleep syscalls. Two
// real kernel threads register, mutate a shared atomic graph counter, poll, and
// are stopped/resumed by the main thread.

#include "alloc.hpp"
#include "gc_world.hpp"
#include "linux.hpp"

using lang::native::linux_x86_64::syscall6;

namespace {

constexpr long kSysWrite = 1;
constexpr long kSysNanosleep = 35;
constexpr long kSysFutex = 202;
constexpr long kSysExitGroup = 231;

constexpr int kFutexWaitPrivate = 128;

// CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD |
// CLONE_SYSVSEM | CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID
constexpr long kCloneFlags = 0x01250f00;

alignas(16) unsigned char g_stack_0[64 * 1024];
alignas(16) unsigned char g_stack_1[64 * 1024];
int32_t g_child_tid[2] = {0, 0};

uint64_t g_graph_counter = 0;
uint32_t g_ready = 0;
uint32_t g_finished = 0;
uint32_t g_stop_workers = 0;
uint32_t g_worker_errors = 0;

void write_all(const char *bytes, uint64_t size) noexcept {
  while (size) {
    const int64_t result =
        syscall6(kSysWrite, 2, reinterpret_cast<long>(bytes),
                 static_cast<long>(size), 0, 0, 0);
    if (result == -4)
      continue;
    if (result <= 0)
      return;
    bytes += result;
    size -= static_cast<uint64_t>(result);
  }
}

[[noreturn]] void fail(const char *reason) noexcept {
  write_all("GC_WORLD_STATIC_FAIL:", sizeof("GC_WORLD_STATIC_FAIL:") - 1);
  uint64_t length = 0;
  while (reason[length])
    ++length;
  write_all(reason, length);
  write_all("\n", 1);
  syscall6(kSysExitGroup, 1, 0, 0, 0, 0, 0);
  __builtin_unreachable();
}

void sleep_ms(int64_t milliseconds) noexcept {
  const long seconds = static_cast<long>(milliseconds / 1000);
  const long nanoseconds = static_cast<long>((milliseconds % 1000) * 1000000);
  struct Timespec {
    long sec;
    long nsec;
  } request{seconds, nanoseconds};
  (void)syscall6(kSysNanosleep, reinterpret_cast<long>(&request), 0, 0, 0, 0,
                 0);
}

bool wait_ready(uint32_t expected) noexcept {
  for (int attempt = 0; attempt < 5000; ++attempt) {
    if (__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE) == expected)
      return true;
    sleep_ms(1);
  }
  return __atomic_load_n(&g_ready, __ATOMIC_ACQUIRE) == expected;
}

bool wait_finished(uint32_t expected) noexcept {
  for (int attempt = 0; attempt < 5000; ++attempt) {
    if (__atomic_load_n(&g_finished, __ATOMIC_ACQUIRE) == expected)
      return true;
    sleep_ms(1);
  }
  return __atomic_load_n(&g_finished, __ATOMIC_ACQUIRE) == expected;
}

bool wait_counter_above(uint64_t value) noexcept {
  for (int attempt = 0; attempt < 5000; ++attempt) {
    if (__atomic_load_n(&g_graph_counter, __ATOMIC_ACQUIRE) > value)
      return true;
    sleep_ms(1);
  }
  return __atomic_load_n(&g_graph_counter, __ATOMIC_ACQUIRE) > value;
}

extern "C" int worker_main(void *argument) noexcept {
  const int index = static_cast<int>(reinterpret_cast<uintptr_t>(argument));
  if (!lang_gc_world_register_v1()) {
    __atomic_add_fetch(&g_worker_errors, 1u, __ATOMIC_RELAXED);
    return 1;
  }
  if (lang_gc_world_register_v1()) {
    __atomic_add_fetch(&g_worker_errors, 1u, __ATOMIC_RELAXED);
  }
  __atomic_add_fetch(&g_ready, 1u, __ATOMIC_RELEASE);
  while (!__atomic_load_n(&g_stop_workers, __ATOMIC_ACQUIRE)) {
    __atomic_add_fetch(&g_graph_counter, 1u, __ATOMIC_RELAXED);
    lang_gc_world_poll_v1();
  }
  if (!lang_gc_world_unregister_v1()) {
    __atomic_add_fetch(&g_worker_errors, 1u, __ATOMIC_RELAXED);
    return 1;
  }
  __atomic_add_fetch(&g_finished, 1u, __ATOMIC_RELEASE);
  (void)index;
  return 0;
}

// x86_64 raw clone. The parent gets the child tid; the child pops worker and
// argument from its new stack, runs worker_main, then exits only that thread.
extern "C" __attribute__((naked, no_stack_protector, noinline))
long raw_clone(long flags, void *child_stack, int32_t *child_tid) noexcept {
  __asm__ volatile(
      "movq %%rdx, %%r10\n\t" // child_tid pointer in the fourth syscall arg
      "xorl %%edx, %%edx\n\t" // parent_tid pointer
      "xorl %%r8d, %%r8d\n\t" // TLS pointer: no TLS in this runtime
      "movl $56, %%eax\n\t"
      "syscall\n\t"
      "testq %%rax, %%rax\n\t"
      "jz 1f\n\t"
      "ret\n\t"
      "1: xorl %%ebp, %%ebp\n\t"
      "cld\n\t"
      "popq %%rax\n\t" // worker_main
      "popq %%rdi\n\t" // argument
      "call *%%rax\n\t"
      "movl %%eax, %%edi\n\t"
      "movl $60, %%eax\n\t" // SYS_exit: terminate this thread only
      "syscall\n\t"
      "ud2\n\t"
      ::: "rax", "rcx", "r11", "rdi", "rsi", "rdx", "r8", "r10", "memory",
      "cc");
}

bool start_raw_thread(int index) noexcept {
  unsigned char *stack = index == 0 ? g_stack_0 : g_stack_1;
  const uint64_t stack_size =
      index == 0 ? sizeof(g_stack_0) : sizeof(g_stack_1);
  const uintptr_t top = (reinterpret_cast<uintptr_t>(stack) + stack_size) &
                        ~static_cast<uintptr_t>(15);
  auto *sp = reinterpret_cast<uintptr_t *>(top);
  *--sp = static_cast<uintptr_t>(index);
  *--sp = reinterpret_cast<uintptr_t>(&worker_main);
  const long result = raw_clone(kCloneFlags, sp, &g_child_tid[index]);
  return result > 0;
}

void join_raw_thread(int index) noexcept {
  int32_t *word = &g_child_tid[index];
  for (;;) {
    const int32_t value = __atomic_load_n(word, __ATOMIC_ACQUIRE);
    if (value == 0)
      return;
    (void)syscall6(kSysFutex, reinterpret_cast<long>(word), kFutexWaitPrivate,
                   value, 0, 0, 0);
  }
}

} // namespace

extern "C" int gc_static_main() noexcept {
  if (lang_gc_world_poll_required_v1())
    fail("idle-request-bit-set");

  if (!lang_gc_world_register_v1())
    fail("first-register");
  if (lang_gc_world_register_v1())
    fail("second-register-not-idempotent");
  if (lang_gc_world_request_pending_v1() != 0)
    fail("pending-helper");

  lang_gc_world_enter_safe_v1();
  lang_gc_world_enter_safe_v1();
  lang_gc_world_leave_safe_v1();
  lang_gc_world_leave_safe_v1();

  if (lang_gc_world_stop_v1(-2) != -22)
    fail("invalid-timeout");
  if (lang_gc_world_stop_v1(0) != 0)
    fail("empty-stop");
  if (lang_gc_world_stop_v1(0) != -16)
    fail("self-reentry");
  lang_gc_world_resume_v1();

  if (!start_raw_thread(0) || !start_raw_thread(1))
    fail("clone");
  if (!wait_ready(2))
    fail("workers-not-ready");

  sleep_ms(20);
  const uint64_t before_stop =
      __atomic_load_n(&g_graph_counter, __ATOMIC_RELAXED);
  if (before_stop == 0)
    fail("no-mutation-before-stop");

  if (lang_gc_world_stop_v1(-1) != 0)
    fail("stop");
  const uint64_t stopped =
      __atomic_load_n(&g_graph_counter, __ATOMIC_ACQUIRE);
  sleep_ms(20);
  if (__atomic_load_n(&g_graph_counter, __ATOMIC_ACQUIRE) != stopped)
    fail("counter-moved-during-stop");

  lang_gc_world_resume_v1();
  if (!wait_counter_above(stopped))
    fail("counter-did-not-resume");

  __atomic_store_n(&g_stop_workers, 1u, __ATOMIC_RELEASE);
  if (!wait_finished(2))
    fail("workers-did-not-finish");
  join_raw_thread(0);
  join_raw_thread(1);
  if (__atomic_load_n(&g_worker_errors, __ATOMIC_ACQUIRE) != 0)
    fail("worker-error");
  if (!lang_gc_world_unregister_v1())
    fail("unregister");
  if (lang_gc_world_unregister_v1())
    fail("unregister-not-idempotent");

  // Re-register the same tid after the nodes above were freed.
  if (!lang_gc_world_register_v1())
    fail("re-register");
  if (!lang_gc_world_unregister_v1())
    fail("re-unregister");

  write_all("GC_WORLD_STATIC_OK\n", sizeof("GC_WORLD_STATIC_OK\n") - 1);
  return 0;
}

extern "C" __attribute__((naked, no_stack_protector, noreturn)) void
_start() noexcept {
  __asm__ volatile(
      "xorl %%ebp, %%ebp\n\t"
      "andq $-16, %%rsp\n\t"
      "call gc_static_main\n\t"
      "movl %%eax, %%edi\n\t"
      "movl $231, %%eax\n\t" // SYS_exit_group
      "syscall\n\t"
      "movl $60, %%eax\n\t"
      "syscall\n\t"
      "ud2\n\t"
      ::: "memory");
}
