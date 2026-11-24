#include "alloc.hpp"

#include <new>

#if defined(__APPLE__) && defined(__aarch64__)
#include <sys/mman.h>
#include <unistd.h>

static inline void *map_private_pages(size_t length) noexcept {
  return mmap(nullptr, length, PROT_READ | PROT_WRITE,
              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

static inline int sys_munmap(void *addr, size_t length) noexcept {
  return munmap(addr, length);
}

#elif defined(__x86_64__) && defined(__linux__)

static inline void *map_private_pages(size_t length) noexcept {
  long ret;
  register long r10 __asm__("r10") = 0x02 | 0x20; // MAP_PRIVATE | MAP_ANONYMOUS
  register long r8 __asm__("r8") = -1;
  register long r9 __asm__("r9") = 0;
  __asm__ volatile (
    "syscall"
    : "=a"(ret)
    : "a"(9 /* SYS_mmap */), "D"(0L), "S"(length),
      "d"(0x1L | 0x2L /* PROT_READ | PROT_WRITE */), "r"(r10), "r"(r8), "r"(r9)
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
#else
#error "Woozy native allocator requires Linux x86_64 or macOS arm64"
#endif

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
// Medium classes have fewer slots per 64 KiB page, so retaining sixteen empty
// pages would buy little reuse while adding 1 MiB per class to the RSS cap.
// Cap each medium class at one quarter of the small-class retention budget.
constexpr uint32_t kMaxMediumEmptyPages = kMaxEmptyPages / 4;
// The published small-class ceiling.  Requests at or below its payload limit
// keep their historical pooled-only behavior; only the added medium classes
// may fall back to the direct mmap path when their 64 KiB page cannot map.
constexpr uint64_t kLegacyClassMaxSize = 4096;
constexpr uint64_t kLegacyMaxSmallPayload =
    kLegacyClassMaxSize - sizeof(AllocHeader);
constexpr uint64_t kAlignmentLimit = 0x10000000ULL;
constexpr uint64_t kSmallAlignMax = 16;
// Larger slots would leave almost half of each 64 KiB pool page unused.
// Give those requests direct, page-rounded mappings instead.
constexpr uint64_t kSmallClassSizes[] = {
    32, 48, 64, 96, 128, 192, 256, 384, 512, 1024, 2048,
    kLegacyClassMaxSize, 8192, 16384};
constexpr uint32_t kSmallClassCount =
    sizeof(kSmallClassSizes) / sizeof(kSmallClassSizes[0]);
constexpr uint64_t kMaxSmallPayload =
    kSmallClassSizes[kSmallClassCount - 1] - sizeof(AllocHeader);
// Bound the aggregate retained direct mappings independently of request sizes
// and multiplicities. Index by multiples of the smallest supported page
// size (Linux 4 KiB; Darwin/arm64 16 KiB).
constexpr uint64_t kDirectCacheBudget = 8 * 1024 * 1024;
constexpr uint64_t kPageUnit = 4096;
constexpr uint64_t kDirectSizeBins = kDirectCacheBudget / kPageUnit;

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

// Pool metadata and the direct cache are shared between threads.  This lock
// needs no allocation, libc, or per-thread state.
uint32_t g_lock_next;
uint32_t g_lock_serving;

static inline void cpu_pause() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  __asm__ volatile ("pause" ::: "memory");
#elif defined(__aarch64__)
  __asm__ volatile ("yield" ::: "memory");
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
// Both the global eviction FIFO and each exact-size reuse FIFO are protected
// by the pool lock. The links occupy freed payload storage; the allocation
// header stays intact. There is no per-region metadata allocation.
struct DirectRegion {
  DirectRegion *older;
  DirectRegion *newer;
  DirectRegion *next_same_size;
};
static_assert(sizeof(AllocHeader) + sizeof(DirectRegion) <= kPageUnit);
struct DirectBin {
  DirectRegion *first;
  DirectRegion *last;
};
DirectBin g_direct_bins[kDirectSizeBins];
DirectRegion *g_direct_oldest;
DirectRegion *g_direct_newest;
uint64_t g_direct_bytes;
uint32_t g_pending_unmaps;

static AllocHeader *direct_header(DirectRegion *region) noexcept {
  return reinterpret_cast<AllocHeader *>(
      reinterpret_cast<uintptr_t>(region) - sizeof(AllocHeader));
}

static DirectBin &direct_bin(uint64_t size) noexcept {
  return g_direct_bins[size / kPageUnit - 1];
}

// Each caller removes a bin's oldest member, either by exact size or as the
// globally oldest entry. Both queues therefore remove in FIFO order.
static void detach_direct_locked(DirectRegion *entry) noexcept {
  const uint64_t size = direct_header(entry)->size;
  DirectBin &bin = direct_bin(size);
  bin.first = entry->next_same_size;
  if (!bin.first)
    bin.last = nullptr;
  if (entry->older)
    entry->older->newer = entry->newer;
  else
    g_direct_oldest = entry->newer;
  if (entry->newer)
    entry->newer->older = entry->older;
  else
    g_direct_newest = entry->older;
  g_direct_bytes -= size;
}

static void *take_direct_cache_locked(uint64_t size) noexcept {
  DirectRegion *entry = direct_bin(size).first;
  if (entry)
    detach_direct_locked(entry);
  return entry;
}

// Register a detached batch before releasing the lock. A concurrent fork
// waits for these unmaps so its child cannot inherit untracked mappings.
static void begin_unmap_locked() noexcept {
  __atomic_fetch_add(&g_pending_unmaps, 1u, __ATOMIC_RELAXED);
}

static void end_unmap() noexcept {
  __atomic_fetch_sub(&g_pending_unmaps, 1u, __ATOMIC_RELEASE);
}

static void unmap_direct_batch(DirectRegion *entry) noexcept {
  while (entry) {
    DirectRegion *next = entry->newer;
    AllocHeader *header = direct_header(entry);
    sys_munmap(header, header->size);
    entry = next;
  }
  end_unmap();
}

static bool drop_direct_cache() noexcept {
  DirectRegion *first;
  {
    PoolLockGuard guard;
    first = g_direct_oldest;
    if (!first)
      return false;
    // Clear only the occupied bins, preserving the global chain for unmap.
    for (DirectRegion *entry = first; entry; entry = entry->newer)
      direct_bin(direct_header(entry)->size) = {};
    g_direct_oldest = g_direct_newest = nullptr;
    g_direct_bytes = 0;
    begin_unmap_locked();
  }
  unmap_direct_batch(first);
  return true;
}

static void retain_direct(void *ptr, uint64_t size) noexcept {
  DirectRegion *retired = nullptr;
  {
    PoolLockGuard guard;
    while (g_direct_bytes > kDirectCacheBudget - size) {
      DirectRegion *victim = g_direct_oldest;
      detach_direct_locked(victim);
      victim->newer = retired;
      retired = victim;
    }
    auto *entry = new (ptr) DirectRegion{g_direct_newest, nullptr, nullptr};
    if (g_direct_newest)
      g_direct_newest->newer = entry;
    else
      g_direct_oldest = entry;
    g_direct_newest = entry;
    DirectBin &bin = direct_bin(size);
    if (bin.last)
      bin.last->next_same_size = entry;
    else
      bin.first = entry;
    bin.last = entry;
    g_direct_bytes += size;
    if (retired)
      begin_unmap_locked();
  }
  if (retired)
    unmap_direct_batch(retired);
}

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

static inline uint32_t empty_page_limit(uint32_t class_index) noexcept {
  return kSmallClassSizes[class_index] > kLegacyClassMaxSize
             ? kMaxMediumEmptyPages
             : kMaxEmptyPages;
}

// Caller holds the pool lock: both the mapping and the class bookkeeping are
// shared-state updates, and the empty-page budget is checked under the lock.
static PoolPage *new_pool_page(uint32_t class_index) noexcept {
  const uint64_t slot_size = kSmallClassSizes[class_index];
  void *base = map_private_pages(kPoolPageSize);
  if (base == reinterpret_cast<void *>(-1))
    return nullptr;

  auto *page = static_cast<PoolPage *>(base);
  const uintptr_t page_base = reinterpret_cast<uintptr_t>(page);
  constexpr uint64_t slot_offset = (sizeof(PoolPage) + 15) & ~uint64_t{15};
  static_assert(slot_offset + 2 * kSmallClassSizes[kSmallClassCount - 1] <=
                kPoolPageSize);
  const uintptr_t slot_start = page_base + slot_offset;
  const uint64_t slot_count = (kPoolPageSize - slot_offset) / slot_size;

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

  // Page bases are at least 16-byte aligned. The first aligned address after
  // the header is at most one alignment from the base (or one header for a
  // smaller alignment). Do not round an unnecessary extra page into the map.
  const uint64_t header_space =
      align > sizeof(AllocHeader) ? align : sizeof(AllocHeader);
  if (size > 0xFFFFFFFFFFFFFFFFULL - header_space)
    return nullptr;
  uint64_t needed = header_space + size;

  if (align <= kSmallAlignMax && size <= kMaxSmallPayload) {
    void *pointer = small_alloc(size);
    if (!pointer && drop_direct_cache())
      pointer = small_alloc(size);
    if (pointer || size <= kLegacyMaxSmallPayload)
      return pointer;
    // A newly pooled medium request can fail to map its 64 KiB page under a
    // tight RLIMIT_AS.  Keep the historical direct mmap path available for
    // those sizes; legacy small classes remain pooled-only.
  }

#if defined(__APPLE__)
  const uint64_t page_mask = static_cast<uint64_t>(getpagesize()) - 1;
#else
  constexpr uint64_t page_mask = kPageUnit - 1;
#endif
  if (needed > 0xFFFFFFFFFFFFFFFFULL - page_mask)
    return nullptr;

  uint64_t mmap_size = (needed + page_mask) & ~page_mask;

  if (align <= kSmallAlignMax && mmap_size <= kDirectCacheBudget) {
    PoolLockGuard guard;
    if (void *cached = take_direct_cache_locked(mmap_size))
      return cached;
  }

  void *base = map_private_pages(mmap_size);
  if (base == reinterpret_cast<void *>(-1) && drop_direct_cache())
    base = map_private_pages(mmap_size);
  if (base == reinterpret_cast<void *>(-1))
    return nullptr;

  uintptr_t raw = reinterpret_cast<uintptr_t>(base) + sizeof(AllocHeader);
  uintptr_t user_ptr = align_up(raw, align);
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
    {
      PoolLockGuard guard;
      ClassState &state = g_classes[page->class_index];
      const bool was_full = page->active == page->slot_count;
      --page->active;

      auto *slot = reinterpret_cast<FreeSlot *>(hdr);
      slot->next = page->free_list;
      page->free_list = slot;
      if (was_full)
        add_nonfull(page);

      if (page->active != 0)
        return;
      if (state.empty_count < empty_page_limit(page->class_index)) {
        ++state.empty_count;
        return;
      }
      remove_nonfull(page);
      begin_unmap_locked();
    }
    sys_munmap(page, kPoolPageSize);
    end_unmap();
    return;
  }
  auto *base = reinterpret_cast<void *>(owner);
  const uint64_t size = hdr->size;
  if (reinterpret_cast<void *>(hdr) == base && size <= kDirectCacheBudget) {
    retain_direct(ptr, size);
    return;
  }
  sys_munmap(base, size);
}

extern "C" void lang_alloc_fork_prepare_v1() noexcept {
  pool_lock_acquire();
  // No new batches can detach while we hold the lock. In-flight unmaps
  // complete without taking it.
  while (__atomic_load_n(&g_pending_unmaps, __ATOMIC_ACQUIRE))
    cpu_pause();
}
extern "C" void lang_alloc_fork_parent_v1() noexcept { pool_lock_release(); }
extern "C" void lang_alloc_fork_child_v1() noexcept {
  __atomic_store_n(&g_lock_next, 0u, __ATOMIC_RELAXED);
  __atomic_store_n(&g_lock_serving, 0u, __ATOMIC_RELAXED);
}
