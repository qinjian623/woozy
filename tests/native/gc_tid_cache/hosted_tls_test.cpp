// Hosted fallback check: gc_world.cpp has no start.cpp in the hosted archive,
// so its private tid-cache hooks stay unresolved/empty. Registration must use
// the raw gettid path and must not write the C library's FS words (glibc keeps
// its DTV and self pointer at FS:0x8 and FS:0x10 on x86_64).

#include "gc_world.hpp"

#include <cstdint>
#include <cstdio>
#include <pthread.h>

namespace {

uintptr_t fs_word_8() noexcept {
  uintptr_t value;
  __asm__ volatile("movq %%fs:0x8, %0" : "=r"(value));
  return value;
}

uintptr_t fs_word_16() noexcept {
  uintptr_t value;
  __asm__ volatile("movq %%fs:0x10, %0" : "=r"(value));
  return value;
}

int check_owned_thread() noexcept {
  const uintptr_t dtv = fs_word_8();
  const uintptr_t self = fs_word_16();
  if (!lang_gc_world_register_v1())
    return 11;
  if (fs_word_8() != dtv || fs_word_16() != self)
    return 12;
  if (!lang_gc_world_unregister_v1())
    return 13;
  if (fs_word_8() != dtv || fs_word_16() != self)
    return 14;
  return 0;
}

void *pthread_body(void *argument) noexcept {
  auto *result = static_cast<int *>(argument);
  *result = check_owned_thread();
  return nullptr;
}

} // namespace

int main() {
  if (const int status = check_owned_thread(); status != 0) {
    std::fprintf(stderr, "hosted main TLS changed: %d\n", status);
    return status;
  }

  pthread_t thread;
  int thread_status = 0;
  if (pthread_create(&thread, nullptr, &pthread_body, &thread_status) != 0)
    return 20;
  if (pthread_join(thread, nullptr) != 0)
    return 21;
  if (thread_status != 0) {
    std::fprintf(stderr, "hosted pthread TLS changed: %d\n", thread_status);
    return thread_status;
  }

  std::puts("GC_TID_CACHE_HOSTED_OK");
  return 0;
}
