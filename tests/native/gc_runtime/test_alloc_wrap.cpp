// Host-test allocation counters for the default build. All runtime layers use
// lang_alloc_v1/lang_free_v1; the link wraps those calls while the real runtime
// allocator still supplies storage.
#include <atomic>
#include <cstdint>

extern "C" void *__real_lang_alloc_v1(uint64_t size, uint64_t align) noexcept;
extern "C" void __real_lang_free_v1(void *ptr) noexcept;

namespace {
std::atomic<int64_t> g_live{0};
std::atomic<uint64_t> g_total{0};
} // namespace

extern "C" void *__wrap_lang_alloc_v1(uint64_t size, uint64_t align) noexcept {
  void *ptr = __real_lang_alloc_v1(size, align);
  if (ptr) {
    g_live.fetch_add(1, std::memory_order_relaxed);
    g_total.fetch_add(1, std::memory_order_relaxed);
  }
  return ptr;
}

extern "C" void __wrap_lang_free_v1(void *ptr) noexcept {
  if (ptr) {
    g_live.fetch_sub(1, std::memory_order_relaxed);
    __real_lang_free_v1(ptr);
  }
}

extern "C" uint64_t gc_test_alloc_live() noexcept {
  return static_cast<uint64_t>(g_live.load(std::memory_order_relaxed));
}

extern "C" uint64_t gc_test_alloc_total() noexcept {
  return g_total.load(std::memory_order_relaxed);
}
