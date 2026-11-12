// Freestanding fixture: linked with -nostdlib against the real runtime archive.
// It exercises the timeout ABI without libc or C++ runtime support.
#include "../../../runtime/native/sync.hpp"
#include "../../../runtime/native/thread.hpp"

namespace {
struct Gate {
  uint32_t open;
};
int64_t entry(void *pointer) noexcept {
  auto *gate = *static_cast<Gate **>(pointer);
  while (!__atomic_load_n(&gate->open, __ATOMIC_ACQUIRE))
    __asm__ volatile("pause" ::: "memory");
  return 23;
}
} // namespace

extern "C" int main() {
  Gate gate{0};
  Gate *argument = &gate;
  auto *thread = lang_thread_spawn_v1(entry, &argument, sizeof(argument),
                                      nullptr, nullptr);
  if (lang_thread_error_v1(thread) != 0) return 1;
  if (lang_thread_wait_for_v1(thread, -2) != -22) return 2;
  if (lang_thread_wait_for_v1(thread, 0) != -110) return 3;
  if (lang_thread_wait_for_v1(thread, 1000000) != -110) return 4;
  __atomic_store_n(&gate.open, 1u, __ATOMIC_RELEASE);
  if (lang_thread_wait_for_v1(thread, 1000000000) != 0) return 5;
  if (lang_thread_result_v1(thread) != 23) return 6;
  lang_handle_release_v1(thread);

  auto *mutex = lang_mutex_new_v1();
  auto *condition = lang_condvar_new_v1();
  lang_mutex_lock_v1(mutex);
  if (lang_condvar_wait_for_v1(condition, mutex, 0)) return 7;
  if (lang_condvar_wait_for_v1(condition, mutex, 1000000)) return 8;
  lang_mutex_unlock_v1(mutex);
  lang_handle_release_v1(condition);
  lang_handle_release_v1(mutex);
  return 0;
}
