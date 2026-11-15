#include "gc.hpp"
#include "alloc.hpp"

#include <cstddef>
#include <cstdint>

#if defined(__x86_64__) && defined(__linux__)

extern "C" [[noreturn]] void lang_native_panic_v1(uint32_t cause,
                                                   uint64_t snapshot,
                                                   uint64_t begin,
                                                   uint64_t end) noexcept;

namespace {

constexpr uint64_t kMaxU64 = ~uint64_t{0};
constexpr uint32_t kReachable = 1u;

uint32_t g_lock_next;
uint32_t g_lock_serving;
uint32_t g_collecting;

// The registry list is still what the collector snapshots, but duplicate
// detection and unregister must not walk it.  A small private address index
// maps objects to an intrusive hash chain.  The bucket count is fixed: it is
// only a performance bound, never a correctness bound.  Any number of
// colliding addresses share a bucket and are still found by exact pointer
// comparison.  The fixed table is BSS; it adds no allocation or public ABI.
constexpr uint64_t kRegistryBuckets = 16384;
static_assert((kRegistryBuckets & (kRegistryBuckets - 1)) == 0,
              "registry bucket count must be a power of two");

struct Entry {
  void *object;
  LangGcOps ops;
  Entry *next;       // collector list
  Entry *prev;       // collector-list back-link for O(1) removal
  Entry *hash_next;  // private address-index chain
  Entry **hash_prev; // link (bucket head or previous Entry::hash_next)
};

Entry *g_entries;
Entry *g_buckets[kRegistryBuckets];
uint64_t g_live;

// Mix the whole pointer value.  mmap-backed objects are page aligned, so
// taking raw low or high bits alone would put every allocation in one bucket.
static inline uint64_t registry_hash(const void *object) noexcept {
  uint64_t value = reinterpret_cast<uintptr_t>(object);
  value ^= value >> 33;
  value *= 0xff51afd7ed558ccdULL;
  value ^= value >> 33;
  value *= 0xc4ceb9fe1a85ec53ULL;
  value ^= value >> 33;
  return value & (kRegistryBuckets - 1);
}

// Caller holds the registry lock.
static Entry *find_entry_locked(const void *object) noexcept {
  for (Entry *entry = g_buckets[registry_hash(object)]; entry;
       entry = entry->hash_next)
    if (entry->object == object) return entry;
  return nullptr;
}

// Caller holds the registry lock.  The new entry is already allocated and all
// of its fields are initialized here before either list can reference it.
static void insert_entry_locked(Entry *entry) noexcept {
  entry->prev = nullptr;
  entry->next = g_entries;
  if (g_entries) g_entries->prev = entry;
  g_entries = entry;

  Entry **bucket = &g_buckets[registry_hash(entry->object)];
  entry->hash_prev = bucket;
  entry->hash_next = *bucket;
  if (*bucket) (*bucket)->hash_prev = &entry->hash_next;
  *bucket = entry;
}

// Caller holds the registry lock.  The entry stays allocated until the caller
// has released the lock, so no hash or list link can outlive it.
static void remove_entry_locked(Entry *entry) noexcept {
  if (entry->prev)
    entry->prev->next = entry->next;
  else
    g_entries = entry->next;
  if (entry->next) entry->next->prev = entry->prev;

  *entry->hash_prev = entry->hash_next;
  if (entry->hash_next) entry->hash_next->hash_prev = entry->hash_prev;
}

static inline void cpu_pause() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  __asm__ volatile("pause" ::: "memory");
#endif
}

static inline void registry_lock_acquire() noexcept {
  const uint32_t ticket =
      __atomic_fetch_add(&g_lock_next, 1u, __ATOMIC_RELAXED);
  while (__atomic_load_n(&g_lock_serving, __ATOMIC_ACQUIRE) != ticket)
    cpu_pause();
}

static inline void registry_lock_release() noexcept {
  __atomic_fetch_add(&g_lock_serving, 1u, __ATOMIC_RELEASE);
}

struct RegistryLockGuard {
  RegistryLockGuard() noexcept { registry_lock_acquire(); }
  ~RegistryLockGuard() noexcept { registry_lock_release(); }
  RegistryLockGuard(const RegistryLockGuard &) = delete;
  RegistryLockGuard &operator=(const RegistryLockGuard &) = delete;
};

[[noreturn]] void gc_panic(uint32_t cause, uint64_t first = 0,
                           uint64_t second = 0, uint64_t third = 0) noexcept {
  lang_native_panic_v1(cause, first, second, third);
  __builtin_unreachable();
}

static inline uint64_t load_refs(void *object) noexcept {
  return __atomic_load_n(static_cast<uint64_t *>(object), __ATOMIC_RELAXED);
}

struct Node {
  void *object;
  LangGcOps ops;
  uint64_t refs;
  uint64_t incoming;
  uint32_t flags;
};

static inline uintptr_t node_address(const Node &node) noexcept {
  return reinterpret_cast<uintptr_t>(node.object);
}

// In-place heapsort by object address.  Addresses are compared as integers so
// unrelated pointers are never ordered with operator<.  The runtime uses no
// libc qsort or heap allocation here.
static void sift_down(Node *nodes, uint64_t root, uint64_t end) noexcept {
  for (;;) {
    const uint64_t left = root * 2 + 1;
    if (left >= end) return;
    uint64_t child = left;
    if (left + 1 < end &&
        node_address(nodes[left]) < node_address(nodes[left + 1]))
      child = left + 1;
    if (node_address(nodes[root]) >= node_address(nodes[child])) return;
    const Node temporary = nodes[root];
    nodes[root] = nodes[child];
    nodes[child] = temporary;
    root = child;
  }
}

static void sort_nodes(Node *nodes, uint64_t count) noexcept {
  if (count < 2) return;
  for (uint64_t root = count / 2; root > 0; --root)
    sift_down(nodes, root - 1, count);
  for (uint64_t end = count - 1; end > 0; --end) {
    const Node temporary = nodes[0];
    nodes[0] = nodes[end];
    nodes[end] = temporary;
    sift_down(nodes, 0, end);
  }
}

static bool find_node(const Node *nodes, uint64_t count, const void *object,
                      uint64_t *index) noexcept {
  const uintptr_t key = reinterpret_cast<uintptr_t>(object);
  uint64_t low = 0;
  uint64_t high = count;
  while (low < high) {
    const uint64_t middle = low + (high - low) / 2;
    if (node_address(nodes[middle]) < key)
      low = middle + 1;
    else
      high = middle;
  }
  if (low < count && node_address(nodes[low]) == key) {
    *index = low;
    return true;
  }
  return false;
}

struct CountContext {
  Node *nodes;
  uint64_t count;
  uint64_t edges;
};

static void count_visitor(void *target, void *context) noexcept {
  auto *state = static_cast<CountContext *>(context);
  if (!target) return;
  if (state->edges == kMaxU64)
    gc_panic(7, reinterpret_cast<uintptr_t>(target));
  ++state->edges;
  uint64_t index;
  if (!find_node(state->nodes, state->count, target, &index)) return;
  if (state->nodes[index].incoming == kMaxU64)
    gc_panic(7, reinterpret_cast<uintptr_t>(target));
  ++state->nodes[index].incoming;
}

struct MarkContext {
  Node *nodes;
  uint64_t count;
  Node **stack;
  uint64_t top;
};

static void mark_visitor(void *target, void *context) noexcept {
  auto *state = static_cast<MarkContext *>(context);
  if (!target) return;
  uint64_t index;
  if (!find_node(state->nodes, state->count, target, &index)) return;
  Node &node = state->nodes[index];
  if (node.flags & kReachable) return;
  node.flags |= kReachable;
  if (state->top == state->count)
    gc_panic(7, reinterpret_cast<uintptr_t>(target));
  state->stack[state->top++] = &node;
}

static void clear_check_visitor(void *target, void *context) noexcept {
  (void)context;
  // clear() must have removed every owned edge.  Any visit is a contract
  // violation, including a visit to an unregistered leaf target.
  gc_panic(7, reinterpret_cast<uintptr_t>(target));
}

static bool snapshot_registry(Node **out_nodes, uint64_t *out_count) noexcept {
  for (;;) {
    uint64_t capacity;
    {
      RegistryLockGuard guard;
      capacity = g_live;
    }
    if (!capacity) {
      *out_nodes = nullptr;
      *out_count = 0;
      return true;
    }
    if (capacity > kMaxU64 / sizeof(Node))
      gc_panic(6, capacity);
    auto *nodes = static_cast<Node *>(
        lang_alloc_v1(capacity * sizeof(Node), alignof(Node)));
    if (!nodes) gc_panic(5, capacity * sizeof(Node));

    bool retry = false;
    uint64_t copied = 0;
    {
      RegistryLockGuard guard;
      if (g_live > capacity) {
        retry = true;
      } else {
        Entry *entry = g_entries;
        for (uint64_t index = 0; index < g_live; ++index) {
          if (!entry) gc_panic(7, index, g_live);
          nodes[index].object = entry->object;
          nodes[index].ops = entry->ops;
          nodes[index].refs = 0;
          nodes[index].incoming = 0;
          nodes[index].flags = 0;
          entry = entry->next;
        }
        copied = g_live;
      }
    }
    if (retry) {
      lang_free_v1(nodes);
      continue;
    }
    *out_nodes = nodes;
    *out_count = copied;
    return true;
  }
}

static uint64_t registry_live() noexcept {
  RegistryLockGuard guard;
  return g_live;
}

static void zero_stats(LangGcStats *out) noexcept {
  if (!out) return;
  out->registered = 0;
  out->traced_edges = 0;
  out->reachable = 0;
  out->candidates = 0;
  out->pinned = 0;
  out->cleared = 0;
  out->collected = 0;
  out->registry_live_after = 0;
}

} // namespace

extern "C" LangGcStatus lang_gc_register_v1(void *object,
                                            const LangGcOps *ops) noexcept {
  if (!object || !ops || !ops->trace || !ops->clear || !ops->retain ||
      !ops->release)
    return LANG_GC_INVALID;
  if (reinterpret_cast<uintptr_t>(object) % alignof(uint64_t) != 0)
    return LANG_GC_INVALID;

  auto *entry = static_cast<Entry *>(
      lang_alloc_v1(sizeof(Entry), alignof(Entry)));
  if (!entry) gc_panic(5, sizeof(Entry));

  bool duplicate = false;
  {
    RegistryLockGuard guard;
    duplicate = find_entry_locked(object) != nullptr;
    if (!duplicate) {
      if (g_live == kMaxU64) gc_panic(7, g_live);
      entry->object = object;
      entry->ops = *ops;
      insert_entry_locked(entry);
      ++g_live;
    }
  }
  if (duplicate) {
    lang_free_v1(entry);
    return LANG_GC_DUPLICATE;
  }
  return LANG_GC_OK;
}

extern "C" LangGcStatus lang_gc_unregister_v1(void *object) noexcept {
  if (!object) return LANG_GC_INVALID;
  Entry *removed = nullptr;
  {
    RegistryLockGuard guard;
    removed = find_entry_locked(object);
    if (removed) {
      remove_entry_locked(removed);
      --g_live;
    }
  }
  if (!removed) return LANG_GC_NOT_REGISTERED;
  // Callback-visible cleanup never runs with the registry lock held.  The
  // entry itself does not retain the target.
  lang_free_v1(removed);
  return LANG_GC_OK;
}

extern "C" uint64_t lang_gc_registry_live_v1() noexcept {
  return registry_live();
}

extern "C" LangGcStatus
lang_gc_collect_stopped_v1(LangGcStats *out) noexcept {
  zero_stats(out);

  uint32_t expected = 0;
  if (!__atomic_compare_exchange_n(&g_collecting, &expected, 1u, false,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return LANG_GC_BUSY;

  LangGcStats stats{};
  Node *nodes = nullptr;
  uint64_t count = 0;
  snapshot_registry(&nodes, &count);
  stats.registered = count;

  if (!count) {
    if (nodes) lang_free_v1(nodes);
    stats.registry_live_after = registry_live();
    __atomic_store_n(&g_collecting, 0u, __ATOMIC_RELEASE);
    if (out) *out = stats;
    return LANG_GC_OK;
  }

  sort_nodes(nodes, count);
  for (uint64_t index = 0; index < count; ++index)
    nodes[index].refs = load_refs(nodes[index].object);

  CountContext counting{nodes, count, 0};
  for (uint64_t index = 0; index < count; ++index) {
    Node &node = nodes[index];
    node.ops.trace(node.object, count_visitor, &counting);
  }
  stats.traced_edges = counting.edges;

  if (count > kMaxU64 / sizeof(Node *)) gc_panic(6, count);
  auto **stack = static_cast<Node **>(
      lang_alloc_v1(count * sizeof(Node *), alignof(Node *)));
  if (!stack) gc_panic(5, count * sizeof(Node *));

  MarkContext marking{nodes, count, stack, 0};
  for (uint64_t index = 0; index < count; ++index) {
    Node &node = nodes[index];
    if (!node.refs)
      gc_panic(7, reinterpret_cast<uintptr_t>(node.object), node.refs,
               node.incoming);
    if (node.incoming > node.refs)
      gc_panic(7, reinterpret_cast<uintptr_t>(node.object), node.refs,
               node.incoming);
    if (node.refs != node.incoming) {
      if (!(node.flags & kReachable)) {
        node.flags |= kReachable;
        if (marking.top == marking.count)
          gc_panic(7, reinterpret_cast<uintptr_t>(node.object));
        stack[marking.top++] = &node;
      }
    }
  }
  while (marking.top) {
    Node *node = stack[--marking.top];
    node->ops.trace(node->object, mark_visitor, &marking);
  }

  uint64_t reachable = 0;
  for (uint64_t index = 0; index < count; ++index)
    if (nodes[index].flags & kReachable) ++reachable;
  stats.reachable = reachable;
  stats.candidates = count - reachable;

  // Pin every candidate before any clear callback can drop an edge.  This
  // keeps the whole candidate set addressable through the clear phase.
  for (uint64_t index = 0; index < count; ++index) {
    Node &node = nodes[index];
    if (node.flags & kReachable) continue;
    if (node.refs == kMaxU64)
      gc_panic(7, reinterpret_cast<uintptr_t>(node.object), node.refs);
    const uint64_t expected_refs = node.refs + 1;
    node.ops.retain(node.object);
    if (load_refs(node.object) != expected_refs)
      gc_panic(7, reinterpret_cast<uintptr_t>(node.object), expected_refs,
               load_refs(node.object));
    ++stats.pinned;
  }

  // Every candidate is now pinned.  Close all its canonical Weak slots before
  // the first clear callback can run.  Otherwise clear(A) could upgrade a weak
  // reference to its white peer B, resurrect B while B is being cleared, and
  // leave an escaped strong reference observing partially cleared state.
  for (uint64_t index = 0; index < count; ++index) {
    Node &node = nodes[index];
    if (node.flags & kReachable) continue;
    if (node.ops.invalidate_weak)
      node.ops.invalidate_weak(node.object);
  }

  for (uint64_t index = 0; index < count; ++index) {
    Node &node = nodes[index];
    if (node.flags & kReachable) continue;
    node.ops.clear(node.object);
    ++stats.cleared;
  }

  // A clear callback that leaves any traceable owned edge has violated its
  // contract.  Detect it before the temporary pins are released.
  for (uint64_t index = 0; index < count; ++index) {
    Node &node = nodes[index];
    if (node.flags & kReachable) continue;
    node.ops.trace(node.object, clear_check_visitor, nullptr);
  }

  for (uint64_t index = 0; index < count; ++index) {
    Node &node = nodes[index];
    if (node.flags & kReachable) continue;
    const uint64_t refs = load_refs(node.object);
    if (!refs)
      gc_panic(7, reinterpret_cast<uintptr_t>(node.object), refs);
    // The temporary pin is the final strong reference exactly when refs is
    // one.  If cleanup resurrected the candidate, release leaves it live and
    // it is not counted as collected.
    if (refs == 1) ++stats.collected;
    node.ops.release(node.object);
  }

  lang_free_v1(stack);
  lang_free_v1(nodes);

  stats.registry_live_after = registry_live();
  __atomic_store_n(&g_collecting, 0u, __ATOMIC_RELEASE);
  if (out) *out = stats;
  return LANG_GC_OK;
}

extern "C" void lang_gc_fork_prepare_v1() noexcept { registry_lock_acquire(); }

extern "C" void lang_gc_fork_parent_v1() noexcept { registry_lock_release(); }

extern "C" void lang_gc_fork_child_v1() noexcept {
  __atomic_store_n(&g_lock_next, 0u, __ATOMIC_RELAXED);
  __atomic_store_n(&g_lock_serving, 0u, __ATOMIC_RELAXED);
  __atomic_store_n(&g_collecting, 0u, __ATOMIC_RELAXED);
}

#endif
