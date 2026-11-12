// Test-only allocator link wrappers for the real-object Weak suite.
//
// Counters are shared by the stress workers and the main thread, so every
// update and read uses atomic builtins only. No std::atomic is required: this
// file is linked into the freestanding static ELF too.
//
// The suite resets only while no worker exists. The permanent 4 x 20000
// observer stress phase asserts exact deltas, so a lost counter update cannot
// be hidden by alloc == free.

#include "weak.hpp"

#include <cstddef>
#include <cstdint>

extern "C" void *__real_lang_alloc_v1(uint64_t, uint64_t) noexcept;
extern "C" void __real_lang_free_v1(void *) noexcept;

namespace {

uint64_t g_allocations;
uint64_t g_frees;
uint64_t g_live;
uint64_t g_bad_frees;
uint64_t g_control_allocations;

} // namespace

extern "C" void *__wrap_lang_alloc_v1(uint64_t size,
                                      uint64_t align) noexcept {
  void *pointer = __real_lang_alloc_v1(size, align);
  if (!pointer) return nullptr;
  __atomic_fetch_add(&g_allocations, 1u, __ATOMIC_RELAXED);
  __atomic_fetch_add(&g_live, 1u, __ATOMIC_RELAXED);
  if (size == sizeof(LangWeakControl))
    __atomic_fetch_add(&g_control_allocations, 1u, __ATOMIC_RELAXED);
  return pointer;
}

extern "C" void __wrap_lang_free_v1(void *pointer) noexcept {
  if (!pointer) return;
  __real_lang_free_v1(pointer);
  __atomic_fetch_add(&g_frees, 1u, __ATOMIC_RELAXED);
  const uint64_t previous = __atomic_fetch_sub(&g_live, 1u, __ATOMIC_RELEASE);
  if (previous == 0)
    __atomic_fetch_add(&g_bad_frees, 1u, __ATOMIC_RELAXED);
}

extern "C" uint64_t lang_weak_objects_allocations() noexcept {
  return __atomic_load_n(&g_allocations, __ATOMIC_ACQUIRE);
}
extern "C" uint64_t lang_weak_objects_frees() noexcept {
  return __atomic_load_n(&g_frees, __ATOMIC_ACQUIRE);
}
extern "C" uint64_t lang_weak_objects_live_allocations() noexcept {
  return __atomic_load_n(&g_live, __ATOMIC_ACQUIRE);
}
extern "C" uint64_t lang_weak_objects_bad_frees() noexcept {
  return __atomic_load_n(&g_bad_frees, __ATOMIC_ACQUIRE);
}
extern "C" uint64_t lang_weak_objects_control_allocations() noexcept {
  return __atomic_load_n(&g_control_allocations, __ATOMIC_ACQUIRE);
}
extern "C" void lang_weak_objects_reset_stats() noexcept {
  __atomic_store_n(&g_allocations, 0u, __ATOMIC_RELEASE);
  __atomic_store_n(&g_frees, 0u, __ATOMIC_RELEASE);
  __atomic_store_n(&g_live, 0u, __ATOMIC_RELEASE);
  __atomic_store_n(&g_bad_frees, 0u, __ATOMIC_RELEASE);
  __atomic_store_n(&g_control_allocations, 0u, __ATOMIC_RELEASE);
}
