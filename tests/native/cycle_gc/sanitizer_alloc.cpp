// Hosted sanitizer-only allocator implementation.  The normal host/static
// builds link the real runtime/native/alloc.cpp; this file makes object memory
// visible to ASAN so use-after-free in the GC callbacks and test graph is
// reported instead of hiding inside raw mmap mappings.

#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace {
constexpr uint64_t kMaxU64 = ~uint64_t{0};
constexpr uint64_t kAlignmentLimit = 0x10000000ULL;
}

extern "C" void *lang_alloc_v1(uint64_t size, uint64_t align) noexcept {
  if (align == 0 || (align & (align - 1)) != 0) return nullptr;
  if (align > kAlignmentLimit) return nullptr;
  if (align < alignof(void *)) align = alignof(void *);
  if (size > kMaxU64 - align) return nullptr;
  const uint64_t rounded = (size + align - 1) & ~(align - 1);
  const uint64_t allocation_size = rounded ? rounded : align;
  if (allocation_size > static_cast<uint64_t>(static_cast<size_t>(-1)))
    return nullptr;
  return std::aligned_alloc(static_cast<size_t>(align),
                            static_cast<size_t>(allocation_size));
}

extern "C" void lang_free_v1(void *pointer) noexcept { std::free(pointer); }
