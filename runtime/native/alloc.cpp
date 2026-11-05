#include "alloc.hpp"

#if defined(__x86_64__) && defined(__linux__)

static inline void *sys_mmap(void *addr, size_t length, int prot, int flags, int fd, long offset) noexcept {
  long ret;
  register long r10 __asm__("r10") = flags;
  register long r8 __asm__("r8") = fd;
  register long r9 __asm__("r9") = offset;
  __asm__ volatile (
    "syscall"
    : "=a"(ret)
    : "a"(9 /* SYS_mmap */), "D"(addr), "S"(length), "d"(prot), "r"(r10), "r"(r8), "r"(r9)
    : "rcx", "r11", "memory"
  );
  if (ret < 0 && ret >= -4095)
    return reinterpret_cast<void *>(-1);
  return reinterpret_cast<void *>(ret);
}

static inline int sys_munmap(void *addr, size_t length) noexcept {
  long ret;
  __asm__ volatile (
    "syscall"
    : "=a"(ret)
    : "a"(11 /* SYS_munmap */), "D"(addr), "S"(length)
    : "rcx", "r11", "memory"
  );
  return static_cast<int>(ret);
}

struct AllocHeader {
  void *mmap_base;
  uint64_t mmap_size;
};

extern "C" void *lang_alloc_v1(uint64_t size, uint64_t align) noexcept {
  // Check power of two alignment
  if (align == 0 || (align & (align - 1)) != 0)
    return nullptr;
  // Guard against unrealistic alignment overflow
  if (align > 0x10000000ULL)
    return nullptr;
  if (align < alignof(AllocHeader))
    align = alignof(AllocHeader);

  // header_space = sizeof(AllocHeader) + align
  // Safe since align <= 0x10000000
  uint64_t header_space = sizeof(AllocHeader) + align;
  if (size > 0xFFFFFFFFFFFFFFFFULL - header_space)
    return nullptr;
  uint64_t needed = header_space + size;

  uint64_t page_mask = 4096 - 1;
  if (needed > 0xFFFFFFFFFFFFFFFFULL - page_mask)
    return nullptr;
  uint64_t mmap_size = (needed + page_mask) & ~page_mask;

  void *base = sys_mmap(nullptr, mmap_size, 0x1 | 0x2 /* PROT_READ|PROT_WRITE */,
                        0x02 | 0x20 /* MAP_PRIVATE|MAP_ANONYMOUS */, -1, 0);
  if (base == reinterpret_cast<void *>(-1))
    return nullptr;

  uintptr_t raw = reinterpret_cast<uintptr_t>(base) + sizeof(AllocHeader);
  uintptr_t user_ptr = (raw + align - 1) & ~(align - 1);
  auto *hdr = reinterpret_cast<AllocHeader *>(user_ptr) - 1;
  hdr->mmap_base = base;
  hdr->mmap_size = mmap_size;
  return reinterpret_cast<void *>(user_ptr);
}

extern "C" void lang_free_v1(void *ptr) noexcept {
  if (!ptr) return;
  auto *hdr = reinterpret_cast<AllocHeader *>(ptr) - 1;
  sys_munmap(hdr->mmap_base, hdr->mmap_size);
}

#endif
