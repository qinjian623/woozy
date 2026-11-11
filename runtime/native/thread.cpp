#include "thread.hpp"
#include <cstddef>
#include <cstdint>

extern "C" [[noreturn]] void lang_native_panic_v1(uint32_t, uint64_t, uint64_t,
                                                 uint64_t) noexcept;

#if defined(__x86_64__) && defined(__linux__)
namespace {
constexpr uint64_t kPage = 4096;
constexpr uint64_t kStackBytes = 1024 * 1024;
constexpr uint64_t kStackMappingBytes = kPage + kStackBytes + kPage;
constexpr int64_t kEintr = -4, kAgain = -11, kDeadlock = -35;
static_assert(sizeof(LangHandle) == 24 && offsetof(LangHandle, refs) == 0 &&
              offsetof(LangHandle, kind) == 8 && offsetof(LangHandle, destroy) == 16);

long syscall0(long number) noexcept {
  long result;
  __asm__ volatile("syscall" : "=a"(result) : "a"(number)
                   : "rcx", "r11", "memory", "cc");
  return result;
}
long syscall2(long number, long first, long second) noexcept {
  long result;
  __asm__ volatile("syscall" : "=a"(result)
                   : "a"(number), "D"(first), "S"(second)
                   : "rcx", "r11", "memory", "cc");
  return result;
}
long syscall3(long number, long first, long second, long third) noexcept {
  long result;
  __asm__ volatile("syscall" : "=a"(result)
                   : "a"(number), "D"(first), "S"(second), "d"(third)
                   : "rcx", "r11", "memory", "cc");
  return result;
}
void *mapping(uint64_t bytes, long protection) noexcept {
  long result;
  register long fourth __asm__("r10") = 0x22; // MAP_PRIVATE | MAP_ANONYMOUS
  register long fifth __asm__("r8") = -1;
  register long sixth __asm__("r9") = 0;
  __asm__ volatile("syscall" : "=a"(result)
                   : "a"(9L), "D"(0L), "S"(bytes), "d"(protection),
                     "r"(fourth), "r"(fifth), "r"(sixth)
                   : "rcx", "r11", "memory", "cc");
  if (result < 0 && result >= -4095)
    lang_native_panic_v1(5, 0, 0, 0);
  return reinterpret_cast<void *>(result);
}
void unmap(void *address, uint64_t bytes) noexcept {
  if (syscall2(11, reinterpret_cast<long>(address), bytes) < 0)
    lang_handle_panic_v1();
}
long futex(uint32_t *word, int operation, uint32_t value) noexcept {
  long result;
  register long fourth __asm__("r10") = 0;
  register long fifth __asm__("r8") = 0;
  register long sixth __asm__("r9") = 0;
  __asm__ volatile("syscall" : "=a"(result)
                   : "a"(202L), "D"(word), "S"(operation), "d"(value),
                     "r"(fourth), "r"(fifth), "r"(sixth)
                   : "rcx", "r11", "memory", "cc");
  return result;
}
void block_signals() noexcept {
  const uint64_t all = ~uint64_t{0};
  long result;
  register long fourth __asm__("r10") = 8; // kernel sigset size
  __asm__ volatile("syscall" : "=a"(result)
                   : "a"(14L), "D"(2L), "S"(&all), "d"(0L), "r"(fourth)
                   : "rcx", "r11", "memory", "cc");
  if (result < 0) lang_handle_panic_v1();
}
struct alignas(16) ThreadState {
  LangHandle handle;
  uint64_t mapping_bytes;
  void *stack_mapping;
  LangThreadEntry entry;
  LangValueOp context_drop;
  int64_t error, result;
  uint32_t done;
  int32_t tid;
};
static_assert(sizeof(ThreadState) % 16 == 0);
void *context(ThreadState *state) noexcept {
  return reinterpret_cast<unsigned char *>(state) + sizeof(ThreadState);
}
void destroy_thread(LangHandle *handle) noexcept {
  auto *state = reinterpret_cast<ThreadState *>(handle);
  unmap(state, state->mapping_bytes);
}
void validate_handle(const LangHandle *handle) noexcept {
  if (!handle || !handle->destroy || handle->kind < LangHandleKind::thread ||
      handle->kind > LangHandleKind::atomic_i64)
    lang_handle_panic_v1();
}
ThreadState *thread(LangHandle *handle) noexcept {
  validate_handle(handle);
  if (handle->kind != LangHandleKind::thread || handle->destroy != destroy_thread)
    lang_handle_panic_v1();
  return reinterpret_cast<ThreadState *>(handle);
}
} // namespace

extern "C" [[noreturn]] void lang_handle_panic_v1() noexcept {
  lang_native_panic_v1(10, 0, 0, 0);
}
extern "C" void lang_handle_retain_v1(LangHandle *handle) noexcept {
  validate_handle(handle);
  uint64_t refs = __atomic_load_n(&handle->refs, __ATOMIC_RELAXED);
  for (;;) {
    if (!refs || refs == ~uint64_t{0}) lang_handle_panic_v1();
    if (__atomic_compare_exchange_n(&handle->refs, &refs, refs + 1, false,
                                    __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return;
  }
}
extern "C" void lang_handle_release_v1(LangHandle *handle) noexcept {
  validate_handle(handle);
  uint64_t refs = __atomic_load_n(&handle->refs, __ATOMIC_RELAXED);
  for (;;) {
    if (!refs) lang_handle_panic_v1();
    if (__atomic_compare_exchange_n(&handle->refs, &refs, refs - 1, false,
                                    __ATOMIC_RELEASE, __ATOMIC_RELAXED)) break;
  }
  if (refs == 1) {
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    handle->destroy(handle);
  }
}

// Signals are blocked before entering. After munmap, neither the stack nor
// FS points at live memory; only registers and the exit syscall may be used.
extern "C" __attribute__((naked, no_stack_protector, noinline, noreturn))
void lang_thread_unmap_exit_v1(void *, uint64_t) noexcept {
  __asm__ volatile(
      "movl $11, %%eax\n\t"
      "syscall\n\t"
      "testq %%rax, %%rax\n\t"
      "js 1f\n\t"
      "xorl %%edi, %%edi\n\t"
      "movl $60, %%eax\n\t"
      "syscall\n\t"
      "ud2\n\t"
      "1: subq $8, %%rsp\n\t"
      "call lang_handle_panic_v1\n\t"
      "ud2\n\t"
      ::: "rax", "rcx", "r11", "rdi", "memory", "cc");
}
extern "C" [[noreturn]] __attribute__((noinline))
void lang_thread_run_v1(ThreadState *state) noexcept {
  const int64_t result = state->entry(context(state));
  if (state->context_drop) state->context_drop(context(state));
  block_signals();
  void *stack_mapping = state->stack_mapping;
  state->result = result;
  __atomic_store_n(&state->done, 1, __ATOMIC_RELEASE);
  if (futex(&state->done, 129, 0x7fffffff) < 0) lang_handle_panic_v1();
  lang_handle_release_v1(&state->handle);
  lang_thread_unmap_exit_v1(stack_mapping, kStackMappingBytes);
}

// x86_64 raw clone: flags, child stack, parent_tid, child_tid, TLS. The
// child stack already contains its entry function and argument.
extern "C" __attribute__((naked, no_stack_protector, noinline))
long lang_thread_clone_start_v1(void *, void *, void *) noexcept {
  __asm__ volatile(
      "movq %%rdx, %%r8\n\t"
      "xorl %%edx, %%edx\n\t"
      "xorl %%r10d, %%r10d\n\t"
      "movl $0xd0f00, %%edi\n\t"
      "movl $56, %%eax\n\t"
      "syscall\n\t"
      "testq %%rax, %%rax\n\t"
      "jz 1f\n\t"
      "ret\n\t"
      "1: xorl %%ebp, %%ebp\n\t"
      "cld\n\t"
      "popq %%rax\n\t"
      "popq %%rdi\n\t"
      "call *%%rax\n\t"
      "ud2\n\t"
      ::: "rax", "rcx", "r11", "rdi", "rdx", "r8", "r10", "memory", "cc");
}
extern "C" LangHandle *lang_thread_spawn_v1(LangThreadEntry entry,
                                            const void *argument, uint64_t size,
                                            LangValueOp copy,
                                            LangValueOp drop) noexcept {
  if (!entry || (size && !argument)) lang_handle_panic_v1();
  if (size > ~uint64_t{0} - sizeof(ThreadState) - kPage)
    lang_native_panic_v1(6, 0, 0, 0);
  const uint64_t bytes = (sizeof(ThreadState) + size + kPage - 1) & ~(kPage - 1);
  auto *state = static_cast<ThreadState *>(mapping(bytes, 3));
  *state = {{2, LangHandleKind::thread, destroy_thread}, bytes, nullptr,
            entry, drop, 0, 0, 0, 0};
  void *stack_mapping = mapping(kStackMappingBytes, 0);
  auto *stack = static_cast<unsigned char *>(stack_mapping) + kPage;
  if (syscall3(10, reinterpret_cast<long>(stack), kStackBytes + kPage, 3) < 0)
    lang_native_panic_v1(5, 0, 0, 0);
  auto *tcb = reinterpret_cast<uintptr_t *>(stack + kStackBytes);
  tcb[0] = reinterpret_cast<uintptr_t>(tcb);
  __asm__ volatile("movq %%fs:0x28, %0" : "=r"(tcb[5]));
  state->stack_mapping = stack_mapping;
  auto *destination = static_cast<volatile unsigned char *>(context(state));
  const auto *source = static_cast<const volatile unsigned char *>(argument);
  for (uint64_t i = 0; i < size; ++i) destination[i] = source[i];
  if (copy) copy(context(state));
  auto *top = tcb;
  *--top = reinterpret_cast<uintptr_t>(state);
  *--top = reinterpret_cast<uintptr_t>(&lang_thread_run_v1);
  const long created = lang_thread_clone_start_v1(state, top, tcb);
  if (created < 0) {
    if (drop) drop(context(state));
    unmap(stack_mapping, kStackMappingBytes);
    state->error = created;
    state->done = 1;
    lang_handle_release_v1(&state->handle); // no runner was created
  } else {
    __atomic_store_n(&state->tid, static_cast<int32_t>(created), __ATOMIC_RELEASE);
  }
  return &state->handle;
}
extern "C" int64_t lang_thread_error_v1(LangHandle *handle) noexcept {
  return thread(handle)->error;
}
extern "C" int64_t lang_thread_wait_v1(LangHandle *handle) noexcept {
  auto *state = thread(handle);
  if (state->error) return state->error;
  for (;;) {
    if (__atomic_load_n(&state->done, __ATOMIC_ACQUIRE)) return 0;
    const long current = syscall0(186);
    if (current < 0) return current;
    if (current == __atomic_load_n(&state->tid, __ATOMIC_ACQUIRE)) {
      if (__atomic_load_n(&state->done, __ATOMIC_ACQUIRE)) return 0;
      return kDeadlock;
    }
    const long result = futex(&state->done, 128, 0);
    if (result < 0 && result != kAgain && result != kEintr) return result;
  }
}
extern "C" int64_t lang_thread_result_v1(LangHandle *handle) noexcept {
  auto *state = thread(handle);
  if (state->error || !__atomic_load_n(&state->done, __ATOMIC_ACQUIRE))
    lang_handle_panic_v1();
  return state->result;
}
#endif
