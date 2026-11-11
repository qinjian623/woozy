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

namespace {

// The direct path keeps the original two-word header.  Page-backed slots use
// the same header, with the low bit of `owner` marking a pooled allocation.
struct AllocHeader {
  uintptr_t owner;
  uint64_t size;
};
static_assert(sizeof(AllocHeader) == 16, "allocation header size changed");

constexpr uint64_t kPoolPageSize = 64 * 1024;
// Empty pages stay mapped and reusable up to this per-class budget.  A page is
// only ever mapped on demand, so this is a cap rather than a reservation.
constexpr uint64_t kEmptyPageBudget = 1 * 1024 * 1024;
constexpr uint32_t kMaxEmptyPages =
    static_cast<uint32_t>(kEmptyPageBudget / kPoolPageSize);
static_assert(kMaxEmptyPages * kPoolPageSize == kEmptyPageBudget,
              "empty-page budget must hold whole pool pages");
constexpr uint64_t kAlignmentLimit = 0x10000000ULL;
constexpr uint64_t kSmallAlignMax = 16;
constexpr uint64_t kSmallClassSizes[] = {
    32, 48, 64, 96, 128, 192, 256, 384, 512, 1024, 2048, 4096};
constexpr uint32_t kSmallClassCount =
    sizeof(kSmallClassSizes) / sizeof(kSmallClassSizes[0]);
constexpr uint64_t kMaxSmallPayload =
    kSmallClassSizes[kSmallClassCount - 1] - sizeof(AllocHeader);

struct FreeSlot {
  FreeSlot *next;
};

struct PoolPage {
  PoolPage *next_free;
  PoolPage *prev_free;
  FreeSlot *free_list;
  uint32_t active;
  uint32_t class_index;
  uint32_t slot_count;
  uint32_t reserved;
};

struct ClassState {
  PoolPage *nonfull;
  uint32_t empty_count;
};

// Pool metadata is shared between threads; direct mmap allocations do not
// touch it. This lock needs no allocation, libc, or per-thread state.
uint32_t g_lock_next;
uint32_t g_lock_serving;

static inline void cpu_pause() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  __asm__ volatile ("pause" ::: "memory");
#endif
}

static inline void pool_lock_acquire() noexcept {
  const uint32_t ticket =
      __atomic_fetch_add(&g_lock_next, 1u, __ATOMIC_RELAXED);
  while (__atomic_load_n(&g_lock_serving, __ATOMIC_ACQUIRE) != ticket)
    cpu_pause();
}

static inline void pool_lock_release() noexcept {
  __atomic_fetch_add(&g_lock_serving, 1u, __ATOMIC_RELEASE);
}

struct PoolLockGuard {
  PoolLockGuard() noexcept { pool_lock_acquire(); }
  ~PoolLockGuard() noexcept { pool_lock_release(); }
  PoolLockGuard(const PoolLockGuard &) = delete;
  PoolLockGuard &operator=(const PoolLockGuard &) = delete;
};

// Zero-initialized POD state; there is no static destructor or dynamic setup.
ClassState g_classes[kSmallClassCount];

static inline uintptr_t align_up(uintptr_t value, uint64_t align) noexcept {
  return (value + align - 1) & ~static_cast<uintptr_t>(align - 1);
}

static inline void add_nonfull(PoolPage *page) noexcept {
  ClassState &state = g_classes[page->class_index];
  page->prev_free = nullptr;
  page->next_free = state.nonfull;
  if (state.nonfull)
    state.nonfull->prev_free = page;
  state.nonfull = page;
}

static inline void remove_nonfull(PoolPage *page) noexcept {
  ClassState &state = g_classes[page->class_index];
  if (page->prev_free)
    page->prev_free->next_free = page->next_free;
  else
    state.nonfull = page->next_free;
  if (page->next_free)
    page->next_free->prev_free = page->prev_free;
  page->next_free = nullptr;
  page->prev_free = nullptr;
}

static inline uint32_t class_for(uint64_t size) noexcept {
  const uint64_t needed = size + sizeof(AllocHeader);
  for (uint32_t index = 0; index < kSmallClassCount; ++index)
    if (needed <= kSmallClassSizes[index])
      return index;
  return kSmallClassCount;
}

// Caller holds the pool lock: both the mapping and the class bookkeeping are
// shared-state updates, and the empty-page budget is checked under the lock.
static PoolPage *new_pool_page(uint32_t class_index) noexcept {
  const uint64_t slot_size = kSmallClassSizes[class_index];
  void *base = sys_mmap(nullptr, kPoolPageSize,
                        0x1 | 0x2 /* PROT_READ|PROT_WRITE */,
                        0x02 | 0x20 /* MAP_PRIVATE|MAP_ANONYMOUS */, -1, 0);
  if (base == reinterpret_cast<void *>(-1))
    return nullptr;

  auto *page = static_cast<PoolPage *>(base);
  const uintptr_t page_base = reinterpret_cast<uintptr_t>(page);
  const uintptr_t slot_start =
      align_up(page_base + sizeof(PoolPage), 16);
  const uint64_t slot_count = (kPoolPageSize - (slot_start - page_base)) /
                              slot_size;
  if (!slot_count) {
    sys_munmap(base, kPoolPageSize);
    return nullptr;
  }

  page->next_free = nullptr;
  page->prev_free = nullptr;
  page->active = 0;
  page->class_index = class_index;
  page->slot_count = static_cast<uint32_t>(slot_count);
  page->reserved = 0;

  FreeSlot *head = nullptr;
  for (uint64_t index = 0; index < slot_count; ++index) {
    auto *slot = reinterpret_cast<FreeSlot *>(
        slot_start + static_cast<uintptr_t>(index * slot_size));
    slot->next = head;
    head = slot;
  }
  page->free_list = head;
  ++g_classes[class_index].empty_count;
  add_nonfull(page);
  return page;
}

static void *small_alloc(uint64_t size) noexcept {
  const uint32_t class_index = class_for(size);
  if (class_index == kSmallClassCount)
    return nullptr;

  PoolLockGuard guard;
  ClassState &state = g_classes[class_index];
  PoolPage *page = state.nonfull;
  if (!page) {
    page = new_pool_page(class_index);
    if (!page)
      return nullptr;
  }

  FreeSlot *slot = page->free_list;
  if (!slot)
    return nullptr;
  page->free_list = slot->next;

  if (page->active == 0)
    --state.empty_count;
  ++page->active;
  if (!page->free_list)
    remove_nonfull(page);

  auto *header = reinterpret_cast<AllocHeader *>(slot);
  header->owner = reinterpret_cast<uintptr_t>(page) | 1;
  header->size = 0;
  return reinterpret_cast<unsigned char *>(header) + sizeof(AllocHeader);
}

} // namespace

extern "C" void *lang_alloc_v1(uint64_t size, uint64_t align) noexcept {
  // Check power of two alignment
  if (align == 0 || (align & (align - 1)) != 0)
    return nullptr;
  // Guard against unrealistic alignment overflow
  if (align > kAlignmentLimit)
    return nullptr;
  if (align < alignof(AllocHeader))
    align = alignof(AllocHeader);

  // header_space = sizeof(AllocHeader) + align
  // Safe since align <= kAlignmentLimit
  uint64_t header_space = sizeof(AllocHeader) + align;
  if (size > 0xFFFFFFFFFFFFFFFFULL - header_space)
    return nullptr;
  uint64_t needed = header_space + size;

  uint64_t page_mask = 4096 - 1;
  if (needed > 0xFFFFFFFFFFFFFFFFULL - page_mask)
    return nullptr;

  if (align <= kSmallAlignMax && size <= kMaxSmallPayload)
    return small_alloc(size);

  uint64_t mmap_size = (needed + page_mask) & ~page_mask;

  void *base = sys_mmap(nullptr, mmap_size, 0x1 | 0x2 /* PROT_READ|PROT_WRITE */,
                        0x02 | 0x20 /* MAP_PRIVATE|MAP_ANONYMOUS */, -1, 0);
  if (base == reinterpret_cast<void *>(-1))
    return nullptr;

  uintptr_t raw = reinterpret_cast<uintptr_t>(base) + sizeof(AllocHeader);
  uintptr_t user_ptr = (raw + align - 1) & ~(align - 1);
  auto *hdr = reinterpret_cast<AllocHeader *>(user_ptr) - 1;
  hdr->owner = reinterpret_cast<uintptr_t>(base);
  hdr->size = mmap_size;
  return reinterpret_cast<void *>(user_ptr);
}

extern "C" void lang_free_v1(void *ptr) noexcept {
  if (!ptr) return;
  auto *hdr = reinterpret_cast<AllocHeader *>(ptr) - 1;
  const uintptr_t owner = hdr->owner;
  if (owner & 1) {
    auto *page = reinterpret_cast<PoolPage *>(owner & ~static_cast<uintptr_t>(1));
    PoolLockGuard guard;
    ClassState &state = g_classes[page->class_index];
    const bool was_full = page->active == page->slot_count;
    --page->active;

    auto *slot = reinterpret_cast<FreeSlot *>(hdr);
    slot->next = page->free_list;
    page->free_list = slot;
    if (was_full)
      add_nonfull(page);

    if (page->active == 0) {
      if (state.empty_count < kMaxEmptyPages) {
        // Keep the page on the nonfull list for later reuse.  The double-linked
        // list already holds any number of pages; only the budget is new.
        ++state.empty_count;
      } else {
        // Budget is full.  Drop the page from the allocatable list before
        // unmapping it; no class or free-list pointer may retain an address
        // into this page.
        remove_nonfull(page);
        sys_munmap(page, kPoolPageSize);
      }
    }
  } else {
    sys_munmap(reinterpret_cast<void *>(owner), hdr->size);
  }
}

extern "C" void lang_alloc_fork_prepare_v1() noexcept { pool_lock_acquire(); }
extern "C" void lang_alloc_fork_parent_v1() noexcept { pool_lock_release(); }
extern "C" void lang_alloc_fork_child_v1() noexcept {
  __atomic_store_n(&g_lock_next, 0u, __ATOMIC_RELAXED);
  __atomic_store_n(&g_lock_serving, 0u, __ATOMIC_RELAXED);
}

#endif
