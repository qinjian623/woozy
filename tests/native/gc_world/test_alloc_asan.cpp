// ASAN/UBSAN host-test allocation shim. It satisfies the runtime's
// lang_alloc_v1/lang_free_v1 contract with libc heap storage so AddressSanitizer
// can see every registry node and LeakSanitizer can prove there is no residue.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace {
std::atomic<int64_t> g_live{0};
std::atomic<uint64_t> g_total{0};
} // namespace

extern "C" void *lang_alloc_v1(uint64_t size, uint64_t align) noexcept {
  if (align < sizeof(void *))
    align = sizeof(void *);
  void *ptr = nullptr;
  if (posix_memalign(&ptr, static_cast<size_t>(align),
                     static_cast<size_t>(size ? size : 1)) != 0)
    return nullptr;
  g_live.fetch_add(1, std::memory_order_relaxed);
  g_total.fetch_add(1, std::memory_order_relaxed);
  return ptr;
}

extern "C" void lang_free_v1(void *ptr) noexcept {
  if (ptr) {
    g_live.fetch_sub(1, std::memory_order_relaxed);
    std::free(ptr);
  }
}

extern "C" void lang_alloc_fork_prepare_v1() noexcept {}
extern "C" void lang_alloc_fork_parent_v1() noexcept {}
extern "C" void lang_alloc_fork_child_v1() noexcept {}

extern "C" uint64_t gc_test_alloc_live() noexcept {
  return static_cast<uint64_t>(g_live.load(std::memory_order_relaxed));
}

extern "C" uint64_t gc_test_alloc_total() noexcept {
  return g_total.load(std::memory_order_relaxed);
}
