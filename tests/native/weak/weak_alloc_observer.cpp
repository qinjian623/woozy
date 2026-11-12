// Test-only link wrappers around the native allocator. The production Weak
// runtime allocates exactly one kind of object (LangWeakControl), so these
// counters directly expose control-block allocation/free balance. The fixture
// objects themselves use mmap in the harness and never enter these counters.
//
// In the ASAN-oracle build the wrapper also poisons freed control storage. The
// Weak source is compiled with sanitizers in that build, so an accidental read
// or write through a released control block is reported even though the native
// allocator itself is a custom mmap pool rather than the system allocator.

#include "weak.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>

extern "C" void *__real_lang_alloc_v1(uint64_t, uint64_t) noexcept;
extern "C" void __real_lang_free_v1(void *) noexcept;

#if defined(LANG_WEAK_ASAN_ORACLE)
extern "C" void __asan_poison_memory_region(void const volatile *,
                                            size_t) noexcept;
extern "C" void __asan_unpoison_memory_region(void const volatile *,
                                              size_t) noexcept;
#endif

namespace {

std::atomic<uint64_t> g_allocations{0};
std::atomic<uint64_t> g_frees{0};
std::atomic<uint64_t> g_live{0};
std::atomic<uint64_t> g_bad_frees{0};
std::atomic<uint64_t> g_unexpected_sizes{0};

} // namespace

extern "C" void *__wrap_lang_alloc_v1(uint64_t size,
                                      uint64_t align) noexcept {
  void *pointer = __real_lang_alloc_v1(size, align);
  if (!pointer) return nullptr;
  if (size != sizeof(LangWeakControl))
    g_unexpected_sizes.fetch_add(1, std::memory_order_relaxed);
#if defined(LANG_WEAK_ASAN_ORACLE)
  __asan_unpoison_memory_region(pointer, sizeof(LangWeakControl));
#endif
  g_allocations.fetch_add(1, std::memory_order_relaxed);
  g_live.fetch_add(1, std::memory_order_relaxed);
  return pointer;
}

extern "C" void __wrap_lang_free_v1(void *pointer) noexcept {
  if (!pointer) return;
  const uint64_t live = g_live.load(std::memory_order_relaxed);
  if (!live)
    g_bad_frees.fetch_add(1, std::memory_order_relaxed);
  else
    g_live.fetch_sub(1, std::memory_order_relaxed);
  g_frees.fetch_add(1, std::memory_order_relaxed);
#if defined(LANG_WEAK_ASAN_ORACLE)
  // Poison before returning the block to the unmapped-by-ASAN allocator. The
  // allocator's own free-list write is uninstrumented, and a later successful
  // allocation unpoisons the same region.
  __asan_poison_memory_region(pointer, sizeof(LangWeakControl));
#endif
  __real_lang_free_v1(pointer);
}

extern "C" uint64_t lang_weak_test_allocations() noexcept {
  return g_allocations.load(std::memory_order_relaxed);
}

extern "C" uint64_t lang_weak_test_frees() noexcept {
  return g_frees.load(std::memory_order_relaxed);
}

extern "C" uint64_t lang_weak_test_live_allocations() noexcept {
  return g_live.load(std::memory_order_relaxed);
}

extern "C" uint64_t lang_weak_test_bad_frees() noexcept {
  return g_bad_frees.load(std::memory_order_relaxed);
}

extern "C" uint64_t lang_weak_test_unexpected_sizes() noexcept {
  return g_unexpected_sizes.load(std::memory_order_relaxed);
}

extern "C" void lang_weak_test_reset_alloc_stats() noexcept {
  g_allocations.store(0, std::memory_order_relaxed);
  g_frees.store(0, std::memory_order_relaxed);
  g_live.store(0, std::memory_order_relaxed);
  g_bad_frees.store(0, std::memory_order_relaxed);
  g_unexpected_sizes.store(0, std::memory_order_relaxed);
}
