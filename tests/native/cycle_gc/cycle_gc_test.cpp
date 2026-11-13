// Independent regression harness for runtime/native/gc.hpp (lang-gc/1).
//
// The same source builds as a hosted O0/O2 test and as a freestanding static
// ELF.  The linker wraps lang_alloc_v1/lang_free_v1 so every allocation made
// by the collector, registry and test objects is counted and checked for
// double free.  No libc allocator is used by the static build.

#include "../../../runtime/native/gc.hpp"
#include "../../../runtime/native/alloc.hpp"

#include <cstddef>
#include <cstdint>

#ifndef LANG_GC_FREESTANDING
#include <atomic>
#include <cerrno>
#include <cstring>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#endif

namespace {

constexpr uint64_t kMaxU64 = ~uint64_t{0};
constexpr uint64_t kMaxObjects = 12000;
constexpr uint64_t kMaxEdges = 8;
constexpr uint64_t kSideId = kMaxObjects - 1;

// ---------------------------------------------------------------------------
// Freestanding logging and allocation tracking
// ---------------------------------------------------------------------------

static long raw_write(int fd, const void *buffer, uint64_t size) noexcept {
  long result;
  __asm__ volatile("syscall"
                   : "=a"(result)
                   : "a"(1), "D"(fd), "S"(buffer), "d"(size)
                   : "rcx", "r11", "memory");
  return result;
}

static void write_all(const char *text, uint64_t size) noexcept {
  uint64_t offset = 0;
  while (offset < size) {
    const long result = raw_write(2, text + offset, size - offset);
    if (result == -4) continue;
    if (result <= 0) return;
    offset += static_cast<uint64_t>(result);
  }
}

static void write_text(const char *text) noexcept {
  uint64_t size = 0;
  while (text[size]) ++size;
  write_all(text, size);
}

static void write_u64(uint64_t value) noexcept {
  char digits[24];
  uint64_t used = 0;
  if (!value) {
    digits[used++] = '0';
  } else {
    while (value) {
      digits[used++] = static_cast<char>('0' + value % 10);
      value /= 10;
    }
  }
  char reversed[24];
  for (uint64_t index = 0; index < used; ++index)
    reversed[index] = digits[used - index - 1];
  write_all(reversed, used);
}

static void report_failure(const char *file, unsigned line) noexcept {
  write_text("CYCLE_GC_FAIL:");
  write_text(file);
  write_text(":");
  write_u64(line);
  write_text("\n");
}

#define REQUIRE(condition)                                                     \
  do {                                                                         \
    if (!(condition)) {                                                        \
      report_failure(__FILE__, __LINE__);                                      \
      return false;                                                            \
    }                                                                          \
  } while (0)

static void report_test(const char *name, bool passed) noexcept {
  write_text(passed ? "CYCLE_GC_PASS:" : "CYCLE_GC_FAIL:");
  write_text(name);
  write_text("\n");
}

// ---------------------------------------------------------------------------
// Link-time allocation tracker
// ---------------------------------------------------------------------------

extern "C" void *__real_lang_alloc_v1(uint64_t, uint64_t) noexcept;
extern "C" void __real_lang_free_v1(void *) noexcept;

constexpr uint64_t kTrackSlots = 1u << 19;
constexpr uintptr_t kTombstone = 1;
constexpr uint64_t kTrackMagic = 0x4c414e4747435452ULL;

uintptr_t g_track_slots[kTrackSlots];
uint32_t g_track_lock_next;
uint32_t g_track_lock_serving;
uint64_t g_track_live;
uint64_t g_track_total_allocations;
uint64_t g_track_total_frees;
uint64_t g_track_bad_free;
uint64_t g_track_double_free;
uint64_t g_track_table_overflow;

struct TrackHeader {
  uint64_t magic;
  void *real;
  uint64_t requested;
  uint64_t user;
};

static inline void track_pause() noexcept {
  __asm__ volatile("pause" ::: "memory");
}

static inline void track_lock() noexcept {
  const uint32_t ticket =
      __atomic_fetch_add(&g_track_lock_next, 1u, __ATOMIC_RELAXED);
  while (__atomic_load_n(&g_track_lock_serving, __ATOMIC_ACQUIRE) != ticket)
    track_pause();
}

static inline void track_unlock() noexcept {
  __atomic_fetch_add(&g_track_lock_serving, 1u, __ATOMIC_RELEASE);
}

static inline uint64_t track_hash(uintptr_t pointer) noexcept {
  uint64_t value = pointer >> 4;
  value ^= value >> 33;
  value *= 0xff51afd7ed558ccdULL;
  value ^= value >> 33;
  value *= 0xc4ceb9fe1a85ec53ULL;
  value ^= value >> 33;
  return value & (kTrackSlots - 1);
}

// Caller holds the tracker lock.
static bool track_insert_locked(uintptr_t pointer) noexcept {
  const uint64_t mask = kTrackSlots - 1;
  const uint64_t home = track_hash(pointer);
  uint64_t first_tombstone = kTrackSlots;
  for (uint64_t step = 0; step < kTrackSlots; ++step) {
    const uint64_t index = (home + step) & mask;
    const uintptr_t key = g_track_slots[index];
    if (!key) {
      if (first_tombstone != kTrackSlots) {
        g_track_slots[first_tombstone] = pointer;
      } else {
        g_track_slots[index] = pointer;
      }
      ++g_track_live;
      ++g_track_total_allocations;
      return true;
    }
    if (key == pointer) return false;
    if (key == kTombstone && first_tombstone == kTrackSlots)
      first_tombstone = index;
  }
  if (first_tombstone != kTrackSlots) {
    g_track_slots[first_tombstone] = pointer;
    ++g_track_live;
    ++g_track_total_allocations;
    return true;
  }
  __atomic_fetch_add(&g_track_table_overflow, 1u, __ATOMIC_RELAXED);
  return false;
}

// Caller holds the tracker lock.
static bool track_remove_locked(uintptr_t pointer) noexcept {
  const uint64_t mask = kTrackSlots - 1;
  const uint64_t home = track_hash(pointer);
  for (uint64_t step = 0; step < kTrackSlots; ++step) {
    const uint64_t index = (home + step) & mask;
    const uintptr_t key = g_track_slots[index];
    if (!key) return false;
    if (key == pointer) {
      g_track_slots[index] = kTombstone;
      --g_track_live;
      ++g_track_total_frees;
      return true;
    }
  }
  return false;
}

static uint64_t track_live_count() noexcept {
  track_lock();
  const uint64_t live = g_track_live;
  track_unlock();
  return live;
}

extern "C" void *__wrap_lang_alloc_v1(uint64_t size,
                                      uint64_t align) noexcept {
  // The real allocator rejects invalid alignment.  Preserve that behavior
  // without reading a header for a request that cannot have succeeded.
  if (align == 0 || (align & (align - 1)) != 0) {
    return __real_lang_alloc_v1(size, align);
  }
  if (align > 0x10000000ULL) return nullptr;
  if (align < 16) align = 16;
  if (size > kMaxU64 - sizeof(TrackHeader) - align) return nullptr;

  const uint64_t total = size + sizeof(TrackHeader) + align;
  void *base_allocation = __real_lang_alloc_v1(total, alignof(TrackHeader));
  if (!base_allocation) return nullptr;

  const uintptr_t base =
      reinterpret_cast<uintptr_t>(base_allocation) + sizeof(TrackHeader);
  const uintptr_t user = (base + align - 1) & ~(static_cast<uintptr_t>(align) - 1);
  auto *header = reinterpret_cast<TrackHeader *>(user - sizeof(TrackHeader));
  header->magic = kTrackMagic;
  header->real = base_allocation;
  header->requested = size;
  header->user = user;

  track_lock();
  const bool inserted = track_insert_locked(user);
  track_unlock();
  if (!inserted) {
    __real_lang_free_v1(base_allocation);
    return nullptr;
  }
  return reinterpret_cast<void *>(user);
}

extern "C" void __wrap_lang_free_v1(void *pointer) noexcept {
  if (!pointer) {
    __real_lang_free_v1(nullptr);
    return;
  }
  track_lock();
  const bool removed = track_remove_locked(reinterpret_cast<uintptr_t>(pointer));
  track_unlock();
  if (!removed) {
    __atomic_fetch_add(&g_track_double_free, 1u, __ATOMIC_RELAXED);
    return;
  }

  auto *header = reinterpret_cast<TrackHeader *>(
      reinterpret_cast<uintptr_t>(pointer) - sizeof(TrackHeader));
  if (header->magic != kTrackMagic || header->user != reinterpret_cast<uintptr_t>(pointer)) {
    __atomic_fetch_add(&g_track_bad_free, 1u, __ATOMIC_RELAXED);
    return;
  }
  void *real = header->real;
  header->magic = 0;
  __real_lang_free_v1(real);
}

// ---------------------------------------------------------------------------
// Test object graph
// ---------------------------------------------------------------------------

struct GcObject;

struct Payload {
  uint64_t magic;
  uint64_t id;
};

constexpr uint64_t kPayloadMagic = 0x5041594c4f414431ULL;

struct GcObject {
  uint64_t refs; // must remain the first field
  const LangGcOps *ops;
  uint64_t id;
  Payload *payload;
  uint64_t edge_count;
  GcObject *edges[kMaxEdges];
  void (*clear_hook)(GcObject *) noexcept;
};

GcObject *g_objects[kMaxObjects];
uint8_t g_destroyed[kMaxObjects];
uint8_t g_root[kMaxObjects];
uint8_t g_reach[kMaxObjects];
uint8_t g_payload_drops[kMaxObjects];
uint8_t g_payload_copies[kMaxObjects];
uint64_t g_incoming[kMaxObjects];
GcObject *g_oracle_stack[kMaxObjects];

uint64_t g_graph_count;
uint64_t g_created_total;
uint64_t g_destroyed_total;
uint64_t g_payload_copy_total;
uint64_t g_payload_drop_total;
uint64_t g_bad_refcount;
uint64_t g_bad_payload;
uint64_t g_double_destroy;
uint64_t g_bad_unregister;
GcObject *g_side_object;

static void object_trace(void *object, LangGcVisitor visitor,
                         void *context) noexcept;
static void object_clear(void *object) noexcept;
static void object_retain(void *object) noexcept;
static void object_release(void *object) noexcept;

static const LangGcOps g_object_ops = {
    object_trace, object_clear, object_retain, object_release};

static void drop_fields(GcObject *object) noexcept {
  const uint64_t count = object->edge_count;
  object->edge_count = 0;
  for (uint64_t index = 0; index < count; ++index) {
    GcObject *target = object->edges[index];
    object->edges[index] = nullptr;
    object_release(target);
  }
}

static void object_trace(void *opaque, LangGcVisitor visitor,
                         void *context) noexcept {
  auto *object = static_cast<GcObject *>(opaque);
  for (uint64_t index = 0; index < object->edge_count; ++index)
    visitor(object->edges[index], context);
}

static void object_clear(void *opaque) noexcept {
  auto *object = static_cast<GcObject *>(opaque);
  drop_fields(object);
  if (object->clear_hook) {
    auto hook = object->clear_hook;
    object->clear_hook = nullptr;
    hook(object);
  }
}

static void object_retain(void *opaque) noexcept {
  auto *object = static_cast<GcObject *>(opaque);
  uint64_t refs = __atomic_load_n(&object->refs, __ATOMIC_RELAXED);
  for (;;) {
    if (refs == kMaxU64) {
      __atomic_fetch_add(&g_bad_refcount, 1u, __ATOMIC_RELAXED);
      return;
    }
    if (__atomic_compare_exchange_n(&object->refs, &refs, refs + 1, false,
                                    __ATOMIC_RELAXED, __ATOMIC_RELAXED))
      return;
  }
}

static void object_release(void *opaque) noexcept {
  auto *object = static_cast<GcObject *>(opaque);
  uint64_t refs = __atomic_load_n(&object->refs, __ATOMIC_RELAXED);
  for (;;) {
    if (!refs) {
      __atomic_fetch_add(&g_bad_refcount, 1u, __ATOMIC_RELAXED);
      return;
    }
    const uint64_t next = refs - 1;
    if (__atomic_compare_exchange_n(&object->refs, &refs, next, false,
                                    __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
      if (next) return;
      break;
    }
  }
  __atomic_thread_fence(__ATOMIC_ACQUIRE);

  const uint64_t id = object->id;
  if (id >= kMaxObjects || g_destroyed[id]) {
    __atomic_fetch_add(&g_double_destroy, 1u, __ATOMIC_RELAXED);
    return;
  }
  g_destroyed[id] = 1;
  ++g_destroyed_total;

  Payload *payload = object->payload;
  if (!payload || payload->magic != kPayloadMagic || payload->id != id) {
    ++g_bad_payload;
  } else {
    ++g_payload_drops[id];
    ++g_payload_drop_total;
  }
  if (payload) lang_free_v1(payload);

  drop_fields(object);

  const LangGcStatus status = lang_gc_unregister_v1(object);
  if (status != LANG_GC_OK && status != LANG_GC_NOT_REGISTERED)
    ++g_bad_unregister;
  lang_free_v1(object);
}

static void reset_graph() noexcept {
  g_graph_count = 0;
  g_side_object = nullptr;
  g_created_total = 0;
  g_destroyed_total = 0;
  g_payload_copy_total = 0;
  g_payload_drop_total = 0;
  g_bad_refcount = 0;
  g_bad_payload = 0;
  g_double_destroy = 0;
  g_bad_unregister = 0;
  for (uint64_t index = 0; index < kMaxObjects; ++index) {
    g_objects[index] = nullptr;
    g_destroyed[index] = 0;
    g_root[index] = 0;
    g_reach[index] = 0;
    g_payload_drops[index] = 0;
    g_payload_copies[index] = 0;
    g_incoming[index] = 0;
    g_oracle_stack[index] = nullptr;
  }
}

static bool create_object_at(uint64_t id) noexcept {
  if (id >= kMaxObjects || g_objects[id]) return false;
  auto *object = static_cast<GcObject *>(
      lang_alloc_v1(sizeof(GcObject), alignof(GcObject)));
  auto *payload =
      static_cast<Payload *>(lang_alloc_v1(sizeof(Payload), alignof(Payload)));
  if (!object || !payload) {
    if (object) lang_free_v1(object);
    if (payload) lang_free_v1(payload);
    return false;
  }
  object->refs = 1;
  object->ops = &g_object_ops;
  object->id = id;
  object->payload = payload;
  object->edge_count = 0;
  object->clear_hook = nullptr;
  for (uint64_t index = 0; index < kMaxEdges; ++index)
    object->edges[index] = nullptr;
  payload->magic = kPayloadMagic;
  payload->id = id;

  const LangGcStatus status = lang_gc_register_v1(object, &g_object_ops);
  if (status != LANG_GC_OK) {
    lang_free_v1(payload);
    lang_free_v1(object);
    return false;
  }
  g_objects[id] = object;
  ++g_payload_copies[id];
  ++g_payload_copy_total;
  ++g_created_total;
  return true;
}

static bool graph_begin(uint64_t count) noexcept {
  if (count > kMaxObjects) return false;
  if (lang_gc_registry_live_v1() != 0 || track_live_count() != 0) return false;
  reset_graph();
  for (uint64_t id = 0; id < count; ++id)
    if (!create_object_at(id)) return false;
  g_graph_count = count;
  return true;
}

static bool add_edge(uint64_t from, uint64_t to) noexcept {
  if (from >= g_graph_count || to >= g_graph_count) return false;
  GcObject *source = g_objects[from];
  GcObject *target = g_objects[to];
  if (!source || !target || source->edge_count == kMaxEdges) return false;
  object_retain(target);
  source->edges[source->edge_count++] = target;
  return true;
}

static bool graph_finish() noexcept {
  for (uint64_t index = 0; index < g_graph_count; ++index)
    g_incoming[index] = 0;
  for (uint64_t index = 0; index < g_graph_count; ++index) {
    GcObject *object = g_objects[index];
    for (uint64_t edge = 0; edge < object->edge_count; ++edge) {
      const uint64_t target = object->edges[edge]->id;
      if (target >= g_graph_count || g_incoming[target] == kMaxU64)
        return false;
      ++g_incoming[target];
    }
  }
  // A live node with no incoming registered edge and no explicit root still
  // owns its construction reference.  Treat it as an additional external root
  // so the oracle and the collector use exactly the same root set.
  for (uint64_t index = 0; index < g_graph_count; ++index)
    if (!g_root[index] && g_incoming[index] == 0) g_root[index] = 1;

  for (uint64_t index = 0; index < g_graph_count; ++index) {
    if (g_root[index]) continue;
    if (g_incoming[index] == 0) return false;
    object_release(g_objects[index]); // drop the construction reference
    if (g_destroyed[index]) return false;
  }
  return true;
}

static uint64_t oracle_mark() noexcept {
  for (uint64_t index = 0; index < g_graph_count; ++index) g_reach[index] = 0;
  uint64_t top = 0;
  for (uint64_t index = 0; index < g_graph_count; ++index) {
    if (!g_root[index] || g_destroyed[index]) continue;
    g_reach[index] = 1;
    g_oracle_stack[top++] = g_objects[index];
  }
  while (top) {
    GcObject *object = g_oracle_stack[--top];
    for (uint64_t edge = 0; edge < object->edge_count; ++edge) {
      GcObject *target = object->edges[edge];
      const uint64_t id = target->id;
      if (id >= g_graph_count || g_reach[id] || g_destroyed[id]) continue;
      g_reach[id] = 1;
      g_oracle_stack[top++] = target;
    }
  }
  uint64_t reachable = 0;
  for (uint64_t index = 0; index < g_graph_count; ++index)
    reachable += g_reach[index] ? 1u : 0u;
  return reachable;
}

static uint64_t total_edges() noexcept {
  uint64_t edges = 0;
  for (uint64_t index = 0; index < g_graph_count; ++index)
    edges += g_objects[index]->edge_count;
  return edges;
}

static bool collect_and_verify() noexcept {
  const uint64_t reachable = oracle_mark();
  const uint64_t candidates = g_graph_count - reachable;
  const uint64_t edges = total_edges();
  LangGcStats stats{};
  const LangGcStatus status = lang_gc_collect_stopped_v1(&stats);
  REQUIRE(status == LANG_GC_OK);
  REQUIRE(stats.registered == g_graph_count);
  REQUIRE(stats.traced_edges == edges);
  REQUIRE(stats.reachable == reachable);
  REQUIRE(stats.candidates == candidates);
  REQUIRE(stats.pinned == candidates);
  REQUIRE(stats.cleared == candidates);
  REQUIRE(stats.collected == candidates);
  REQUIRE(stats.registry_live_after == reachable);
  for (uint64_t index = 0; index < g_graph_count; ++index) {
    const bool should_destroy = !g_reach[index];
    REQUIRE(g_destroyed[index] == (should_destroy ? 1u : 0u));
    REQUIRE(g_payload_drops[index] == (should_destroy ? 1u : 0u));
  }
  return true;
}

static bool check_accounting() noexcept {
  REQUIRE(g_created_total == g_destroyed_total);
  REQUIRE(g_payload_copy_total == g_payload_drop_total);
  for (uint64_t id = 0; id < kMaxObjects; ++id) {
    REQUIRE(g_payload_copies[id] == g_payload_drops[id]);
    REQUIRE(g_payload_copies[id] <= 1);
  }
  REQUIRE(g_bad_refcount == 0);
  REQUIRE(g_bad_payload == 0);
  REQUIRE(g_double_destroy == 0);
  REQUIRE(g_bad_unregister == 0);
  REQUIRE(lang_gc_registry_live_v1() == 0);
  REQUIRE(track_live_count() == 0);
  REQUIRE(g_track_double_free == 0);
  REQUIRE(g_track_bad_free == 0);
  REQUIRE(g_track_table_overflow == 0);
  return true;
}

static bool cleanup_graph() noexcept {
  // Drop the external construction references.  Any survivors are then either
  // finalized immediately by ordinary ARC or are cycles with no remaining
  // external root.  A second stopped-world collection removes those cycles.
  for (uint64_t index = 0; index < g_graph_count; ++index) {
    if (g_root[index] && !g_destroyed[index]) {
      g_root[index] = 0;
      object_release(g_objects[index]);
    }
  }
  LangGcStats stats{};
  REQUIRE(lang_gc_collect_stopped_v1(&stats) == LANG_GC_OK);
  REQUIRE(stats.candidates == stats.registered);
  REQUIRE(stats.collected == stats.candidates);
  REQUIRE(stats.registry_live_after == 0);
  for (uint64_t index = 0; index < g_graph_count; ++index)
    REQUIRE(g_destroyed[index] == 1);
  return check_accounting();
}

// ---------------------------------------------------------------------------
// Registry-only object used by the table, concurrency and fork tests.
// ---------------------------------------------------------------------------

struct RegistryOnly {
  uint64_t refs;
};

static void registry_only_trace(void *, LangGcVisitor, void *) noexcept {}

static void registry_only_clear(void *) noexcept {}

static void registry_only_retain(void *opaque) noexcept {
  auto *object = static_cast<RegistryOnly *>(opaque);
  __atomic_add_fetch(&object->refs, 1u, __ATOMIC_RELAXED);
}

static void registry_only_release(void *opaque) noexcept {
  auto *object = static_cast<RegistryOnly *>(opaque);
  if (__atomic_sub_fetch(&object->refs, 1u, __ATOMIC_ACQ_REL) == 0)
    lang_free_v1(object);
}

static const LangGcOps g_registry_only_ops = {
    registry_only_trace, registry_only_clear, registry_only_retain,
    registry_only_release};

// ---------------------------------------------------------------------------
// Focused graph tests
// ---------------------------------------------------------------------------

static bool test_immediate_arc_and_empty_collection() noexcept {
  if (!graph_begin(1)) return false;
  GcObject *object = g_objects[0];
  LangGcStats stats{};
  REQUIRE(lang_gc_collect_stopped_v1(&stats) == LANG_GC_OK);
  REQUIRE(stats.registered == 1);
  REQUIRE(stats.candidates == 0);
  REQUIRE(stats.collected == 0);
  REQUIRE(stats.registry_live_after == 1);
  REQUIRE(g_destroyed[0] == 0);

  // No cycle means ordinary ARC remains the only cleanup path.
  object_release(object);
  REQUIRE(g_destroyed[0] == 1);
  REQUIRE(lang_gc_registry_live_v1() == 0);
  REQUIRE(check_accounting());

  if (!graph_begin(1)) return false;
  object = g_objects[0];
  object_release(object);
  REQUIRE(g_destroyed[0] == 1);
  REQUIRE(check_accounting());

  LangGcStats empty{};
  REQUIRE(lang_gc_collect_stopped_v1(&empty) == LANG_GC_OK);
  REQUIRE(empty.registered == 0);
  REQUIRE(empty.traced_edges == 0);
  REQUIRE(empty.reachable == 0);
  REQUIRE(empty.candidates == 0);
  REQUIRE(empty.pinned == 0);
  REQUIRE(empty.cleared == 0);
  REQUIRE(empty.collected == 0);
  REQUIRE(empty.registry_live_after == 0);
  REQUIRE(lang_gc_collect_stopped_v1(nullptr) == LANG_GC_OK);
  return true;
}

static bool test_self_cycle() noexcept {
  REQUIRE(graph_begin(1));
  REQUIRE(add_edge(0, 0));
  REQUIRE(graph_finish());
  REQUIRE(collect_and_verify());
  REQUIRE(cleanup_graph());
  return true;
}

static bool test_two_node_cycle() noexcept {
  REQUIRE(graph_begin(2));
  REQUIRE(add_edge(0, 1));
  REQUIRE(add_edge(1, 0));
  REQUIRE(graph_finish());
  REQUIRE(collect_and_verify());
  REQUIRE(cleanup_graph());
  return true;
}

static bool test_multi_node_cycle() noexcept {
  constexpr uint64_t kNodes = 17;
  REQUIRE(graph_begin(kNodes));
  for (uint64_t index = 0; index < kNodes; ++index)
    REQUIRE(add_edge(index, (index + 1) % kNodes));
  REQUIRE(graph_finish());
  REQUIRE(collect_and_verify());
  REQUIRE(cleanup_graph());
  return true;
}

static bool test_duplicate_edges() noexcept {
  REQUIRE(graph_begin(2));
  REQUIRE(add_edge(0, 1));
  REQUIRE(add_edge(0, 1)); // duplicate edge must be counted twice
  REQUIRE(add_edge(1, 0));
  REQUIRE(graph_finish());
  REQUIRE(collect_and_verify());
  REQUIRE(cleanup_graph());
  return true;
}

static bool test_shared_subcycle() noexcept {
  // Two cycles share node 0, including a duplicate edge around the shared
  // node.  Every node is a candidate; duplicate incoming edges must not be
  // collapsed.
  REQUIRE(graph_begin(5));
  REQUIRE(add_edge(0, 1));
  REQUIRE(add_edge(1, 0));
  REQUIRE(add_edge(0, 2));
  REQUIRE(add_edge(2, 0));
  REQUIRE(add_edge(0, 2)); // duplicate
  REQUIRE(add_edge(3, 4));
  REQUIRE(add_edge(4, 3));
  REQUIRE(add_edge(3, 0));
  REQUIRE(add_edge(0, 3));
  REQUIRE(graph_finish());
  REQUIRE(collect_and_verify());
  REQUIRE(cleanup_graph());
  return true;
}

static bool test_root_reachable_then_unroot() noexcept {
  REQUIRE(graph_begin(2));
  REQUIRE(add_edge(0, 1));
  REQUIRE(add_edge(1, 0));
  g_root[0] = 1;
  REQUIRE(graph_finish());
  REQUIRE(collect_and_verify()); // both reachable; nothing collected

  // Remove the only external root.  The remaining cycle stays alive until the
  // next stopped-world collection.
  g_root[0] = 0;
  object_release(g_objects[0]);
  REQUIRE(g_destroyed[0] == 0);
  REQUIRE(g_destroyed[1] == 0);
  REQUIRE(collect_and_verify());
  REQUIRE(cleanup_graph());
  return true;
}

static bool test_dead_graph_points_to_live_external() noexcept {
  // 0<->1 is a dead cycle, 0 points at live root 2, and 2 points nowhere back.
  REQUIRE(graph_begin(3));
  REQUIRE(add_edge(0, 1));
  REQUIRE(add_edge(1, 0));
  REQUIRE(add_edge(0, 2));
  g_root[2] = 1;
  REQUIRE(graph_finish());
  REQUIRE(collect_and_verify());
  REQUIRE(g_destroyed[0] == 1);
  REQUIRE(g_destroyed[1] == 1);
  REQUIRE(g_destroyed[2] == 0);
  REQUIRE(g_payload_drops[2] == 0);
  REQUIRE(lang_gc_registry_live_v1() == 1);
  REQUIRE(cleanup_graph());
  return true;
}

static bool test_acyclic_long_chain() noexcept {
  constexpr uint64_t kNodes = 1024;
  REQUIRE(graph_begin(kNodes));
  for (uint64_t index = 0; index + 1 < kNodes; ++index)
    REQUIRE(add_edge(index, index + 1));
  g_root[0] = 1;
  REQUIRE(graph_finish());
  REQUIRE(collect_and_verify()); // all reachable through the external root
  REQUIRE(lang_gc_registry_live_v1() == kNodes);
  REQUIRE(cleanup_graph());
  return true;
}

static bool test_large_cycle() noexcept {
  constexpr uint64_t kNodes = 10000;
  REQUIRE(graph_begin(kNodes));
  for (uint64_t index = 0; index < kNodes; ++index)
    REQUIRE(add_edge(index, (index + 1) % kNodes));
  REQUIRE(graph_finish());
  REQUIRE(collect_and_verify());
  REQUIRE(cleanup_graph());
  return true;
}

static bool test_max_u64_boundary() noexcept {
  REQUIRE(graph_begin(1));
  GcObject *object = g_objects[0];
  __atomic_store_n(&object->refs, kMaxU64, __ATOMIC_RELAXED);
  LangGcStats stats{};
  REQUIRE(lang_gc_collect_stopped_v1(&stats) == LANG_GC_OK);
  REQUIRE(stats.registered == 1);
  REQUIRE(stats.candidates == 0);
  REQUIRE(stats.pinned == 0);
  REQUIRE(stats.collected == 0);
  REQUIRE(stats.registry_live_after == 1);
  REQUIRE(g_destroyed[0] == 0);

  // Drop the artificial surplus, then use the normal construction reference.
  __atomic_store_n(&object->refs, 1, __ATOMIC_RELAXED);
  object_release(object);
  REQUIRE(g_destroyed[0] == 1);
  REQUIRE(check_accounting());
  return true;
}

static void hook_unregister_self(GcObject *object) noexcept {
  (void)lang_gc_unregister_v1(object);
}

static LangGcStatus g_nested_status;
static LangGcStats g_nested_stats;
static bool g_hook_create_failed;

static void hook_allocate_and_nested(GcObject *object) noexcept {
  (void)object;
  g_nested_stats = {};
  g_nested_status = lang_gc_collect_stopped_v1(&g_nested_stats);
  if (!g_side_object) {
    if (!create_object_at(kSideId)) {
      g_hook_create_failed = true;
    } else {
      g_side_object = g_objects[kSideId];
    }
  }
}

static bool test_clear_callback_unregister() noexcept {
  REQUIRE(graph_begin(2));
  REQUIRE(add_edge(0, 1));
  REQUIRE(add_edge(1, 0));
  g_objects[0]->clear_hook = hook_unregister_self;
  REQUIRE(graph_finish());
  REQUIRE(collect_and_verify());
  REQUIRE(lang_gc_registry_live_v1() == 0);
  REQUIRE(cleanup_graph());
  return true;
}

static void hook_retain_self(GcObject *object) noexcept {
  object_retain(object);
}

static bool test_clear_callback_resurrection_is_not_collected() noexcept {
  REQUIRE(graph_begin(2));
  REQUIRE(add_edge(0, 1));
  REQUIRE(add_edge(1, 0));
  g_objects[0]->clear_hook = hook_retain_self;
  REQUIRE(graph_finish());

  LangGcStats stats{};
  REQUIRE(lang_gc_collect_stopped_v1(&stats) == LANG_GC_OK);
  REQUIRE(stats.registered == 2);
  REQUIRE(stats.candidates == 2);
  REQUIRE(stats.pinned == 2);
  REQUIRE(stats.cleared == 2);
  // The cleared object that retained itself is still alive when its temporary
  // pin is released, so only the other candidate actually finalizes.
  REQUIRE(stats.collected == 1);
  REQUIRE(stats.registry_live_after == 1);
  REQUIRE(g_destroyed[0] == 0);
  REQUIRE(g_destroyed[1] == 1);
  REQUIRE(g_payload_drops[0] == 0);
  REQUIRE(g_payload_drops[1] == 1);

  object_release(g_objects[0]);
  REQUIRE(g_destroyed[0] == 1);
  REQUIRE(check_accounting());
  return true;
}

static bool test_clear_callback_allocate_nested_collect() noexcept {
  REQUIRE(graph_begin(2));
  REQUIRE(add_edge(0, 1));
  REQUIRE(add_edge(1, 0));
  g_nested_status = LANG_GC_OK;
  g_nested_stats = {};
  g_hook_create_failed = false;
  g_side_object = nullptr;
  g_objects[0]->clear_hook = hook_allocate_and_nested;
  REQUIRE(graph_finish());

  const uint64_t reachable = oracle_mark();
  const uint64_t candidates = g_graph_count - reachable;
  REQUIRE(candidates == 2);
  LangGcStats stats{};
  REQUIRE(lang_gc_collect_stopped_v1(&stats) == LANG_GC_OK);
  REQUIRE(stats.registered == 2); // the side object is outside the snapshot
  REQUIRE(stats.candidates == 2);
  REQUIRE(stats.pinned == 2);
  REQUIRE(stats.cleared == 2);
  REQUIRE(stats.collected == 2);
  REQUIRE(stats.registry_live_after == 1);
  REQUIRE(g_nested_status == LANG_GC_BUSY);
  REQUIRE(g_nested_stats.registered == 0);
  REQUIRE(g_nested_stats.collected == 0);
  REQUIRE(!g_hook_create_failed);
  REQUIRE(g_side_object == g_objects[kSideId]);
  REQUIRE(g_destroyed[0] == 1);
  REQUIRE(g_destroyed[1] == 1);
  REQUIRE(g_destroyed[kSideId] == 0);
  REQUIRE(lang_gc_registry_live_v1() == 1);

  // The newly registered object survived the outer snapshot and is cleaned up
  // through ordinary ARC once the callback has returned.
  GcObject *side = g_side_object;
  g_side_object = nullptr;
  object_release(side);
  REQUIRE(g_destroyed[kSideId] == 1);
  REQUIRE(check_accounting());
  return true;
}

static uint64_t g_rng = 0x9e3779b97f4a7c15ULL;

static uint64_t rng_next() noexcept {
  uint64_t value = g_rng;
  value ^= value << 13;
  value ^= value >> 7;
  value ^= value << 17;
  g_rng = value;
  return value;
}

static bool test_random_graph_oracle() noexcept {
  // At least hundreds of independently generated graphs.  Each graph's
  // expected collection set is computed by a simple reachability oracle from
  // its explicit/derived external roots, independent of the collector's
  // incoming-edge algorithm.
  for (uint64_t graph = 0; graph < 320; ++graph) {
    constexpr uint64_t kNodes = 48;
    REQUIRE(graph_begin(kNodes));
    for (uint64_t node = 0; node < kNodes; ++node) {
      const uint64_t degree = rng_next() & 3u;
      for (uint64_t edge = 0; edge < degree; ++edge) {
        const uint64_t target = rng_next() % kNodes;
        if (!add_edge(node, target)) return false;
      }
    }
    for (uint64_t node = 0; node < kNodes; ++node)
      g_root[node] = (rng_next() & 15u) == 0 ? 1 : 0;
    REQUIRE(graph_finish());
    REQUIRE(collect_and_verify());
    REQUIRE(cleanup_graph());
  }
  return true;
}

static bool test_registry_index() noexcept {
  REQUIRE(lang_gc_registry_live_v1() == 0);

  // Keep this mirror in sync with the private index in runtime/native/gc.cpp.
  // It is used only to construct deterministic collisions in the real table;
  // the assertions below still rely on exact pointer identity and statuses.
  constexpr uint64_t kTestRegistryBuckets = 2048;
  auto test_hash = [](uintptr_t pointer) noexcept {
    uint64_t value = pointer;
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ULL;
    value ^= value >> 33;
    return value & (kTestRegistryBuckets - 1);
  };

  // Five aligned synthetic addresses in one real bucket.  register and
  // unregister never dereference the target, so this table-only test needs no
  // object storage.
  void *colliders[5];
  uint64_t found = 0;
  const uintptr_t base = 0x100000000ULL;
  const uint64_t bucket = test_hash(base);
  for (uintptr_t candidate = base; found < 5; candidate += 8) {
    if (test_hash(candidate) != bucket) continue;
    colliders[found++] = reinterpret_cast<void *>(candidate);
  }

  REQUIRE(lang_gc_register_v1(colliders[0], &g_registry_only_ops) ==
          LANG_GC_OK);
  REQUIRE(lang_gc_register_v1(colliders[1], &g_registry_only_ops) ==
          LANG_GC_OK);
  REQUIRE(lang_gc_register_v1(colliders[2], &g_registry_only_ops) ==
          LANG_GC_OK);
  REQUIRE(lang_gc_register_v1(colliders[3], &g_registry_only_ops) ==
          LANG_GC_OK);
  REQUIRE(lang_gc_registry_live_v1() == 4);

  // Every colliding address is still distinguished exactly.
  for (uint64_t index = 0; index < 4; ++index)
    REQUIRE(lang_gc_register_v1(colliders[index], &g_registry_only_ops) ==
            LANG_GC_DUPLICATE);
  REQUIRE(lang_gc_unregister_v1(colliders[4]) == LANG_GC_NOT_REGISTERED);
  REQUIRE(lang_gc_registry_live_v1() == 4);

  // The list is 3, 2, 1, 0.  Remove a middle entry, the head, then the tail;
  // re-register the removed middle address and remove it again.
  REQUIRE(lang_gc_unregister_v1(colliders[1]) == LANG_GC_OK);
  REQUIRE(lang_gc_unregister_v1(colliders[1]) == LANG_GC_NOT_REGISTERED);
  REQUIRE(lang_gc_unregister_v1(colliders[3]) == LANG_GC_OK);
  REQUIRE(lang_gc_unregister_v1(colliders[0]) == LANG_GC_OK);
  REQUIRE(lang_gc_registry_live_v1() == 1);
  REQUIRE(lang_gc_register_v1(colliders[1], &g_registry_only_ops) ==
          LANG_GC_OK);
  REQUIRE(lang_gc_register_v1(colliders[1], &g_registry_only_ops) ==
          LANG_GC_DUPLICATE);
  REQUIRE(lang_gc_unregister_v1(colliders[1]) == LANG_GC_OK);
  REQUIRE(lang_gc_unregister_v1(colliders[2]) == LANG_GC_OK);
  REQUIRE(lang_gc_registry_live_v1() == 0);

  // A normal registered graph still snapshots, computes stats and reclaims
  // cycles through the unchanged collector list.
  REQUIRE(graph_begin(3));
  REQUIRE(add_edge(0, 1));
  REQUIRE(add_edge(1, 0));
  REQUIRE(add_edge(2, 2));
  REQUIRE(graph_finish());
  REQUIRE(collect_and_verify());
  REQUIRE(cleanup_graph());
  return true;
}

#ifndef LANG_GC_FREESTANDING

// ---------------------------------------------------------------------------
// Hosted concurrency and panic-path tests
// ---------------------------------------------------------------------------

static void registry_worker(unsigned thread_id, std::atomic<unsigned> *failures) noexcept {
  uint64_t seed = 0x123456789abcdef0ULL ^ (static_cast<uint64_t>(thread_id) << 32);
  for (uint64_t iteration = 0; iteration < 1500; ++iteration) {
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    auto *object = static_cast<RegistryOnly *>(
        lang_alloc_v1(sizeof(RegistryOnly), alignof(RegistryOnly)));
    if (!object) {
      failures->fetch_add(1, std::memory_order_relaxed);
      return;
    }
    object->refs = 1;
    if (lang_gc_register_v1(object, &g_registry_only_ops) != LANG_GC_OK ||
        lang_gc_register_v1(object, &g_registry_only_ops) != LANG_GC_DUPLICATE ||
        lang_gc_unregister_v1(object) != LANG_GC_OK ||
        lang_gc_unregister_v1(object) != LANG_GC_NOT_REGISTERED) {
      failures->fetch_add(1, std::memory_order_relaxed);
      if (lang_gc_unregister_v1(object) == LANG_GC_OK) {
        // already handled above; keep this branch allocation-free
      }
      registry_only_release(object);
      return;
    }
    if ((seed & 7u) == 0) {
      // Re-register a few objects before the final unregister/free.
      if (lang_gc_register_v1(object, &g_registry_only_ops) != LANG_GC_OK ||
          lang_gc_unregister_v1(object) != LANG_GC_OK) {
        failures->fetch_add(1, std::memory_order_relaxed);
        registry_only_release(object);
        return;
      }
    }
    registry_only_release(object);
  }
}

static bool test_concurrent_registration_then_collect() noexcept {
  std::atomic<unsigned> failures{0};
  std::thread workers[8];
  for (unsigned index = 0; index < 8; ++index)
    workers[index] = std::thread(registry_worker, index, &failures);
  for (auto &worker : workers) worker.join();
  REQUIRE(failures.load(std::memory_order_relaxed) == 0);
  REQUIRE(lang_gc_registry_live_v1() == 0);
  REQUIRE(track_live_count() == 0);

  // All mutator threads have stopped.  Only now build and collect cycles; this
  // harness does not claim the test mutex is a language safepoint.
  constexpr uint64_t kNodes = 64;
  REQUIRE(graph_begin(kNodes));
  for (uint64_t index = 0; index < kNodes; ++index)
    REQUIRE(add_edge(index, (index + 1) % kNodes));
  REQUIRE(graph_finish());
  REQUIRE(collect_and_verify());
  REQUIRE(cleanup_graph());
  return true;
}

static void panic_zero_refs() noexcept {
  if (!graph_begin(1)) return;
  __atomic_store_n(&g_objects[0]->refs, 0, __ATOMIC_RELAXED);
  LangGcStats stats{};
  (void)lang_gc_collect_stopped_v1(&stats);
}

static void panic_incoming_exceeds_refs() noexcept {
  if (!graph_begin(2)) return;
  if (!add_edge(0, 1) || !add_edge(0, 1) || !add_edge(1, 0)) return;
  g_root[0] = 1;
  if (!graph_finish()) return;
  __atomic_store_n(&g_objects[1]->refs, 1, __ATOMIC_RELAXED);
  LangGcStats stats{};
  (void)lang_gc_collect_stopped_v1(&stats);
}

static void bad_retain_noop(void *) noexcept {}

static const LangGcOps g_bad_retain_ops = {
    object_trace, object_clear, bad_retain_noop, object_release};

static void panic_retain_contract() noexcept {
  if (!graph_begin(1)) return;
  if (!add_edge(0, 0)) return;
  GcObject *object = g_objects[0];
  if (lang_gc_unregister_v1(object) != LANG_GC_OK) return;
  if (lang_gc_register_v1(object, &g_bad_retain_ops) != LANG_GC_OK) return;
  object->ops = &g_bad_retain_ops;
  if (!graph_finish()) return;
  LangGcStats stats{};
  (void)lang_gc_collect_stopped_v1(&stats);
}

static void bad_clear_noop(void *) noexcept {}

static const LangGcOps g_bad_clear_ops = {
    object_trace, bad_clear_noop, object_retain, object_release};

static void panic_clear_contract() noexcept {
  if (!graph_begin(1)) return;
  if (!add_edge(0, 0)) return;
  GcObject *object = g_objects[0];
  if (lang_gc_unregister_v1(object) != LANG_GC_OK) return;
  if (lang_gc_register_v1(object, &g_bad_clear_ops) != LANG_GC_OK) return;
  object->ops = &g_bad_clear_ops;
  if (!graph_finish()) return;
  LangGcStats stats{};
  (void)lang_gc_collect_stopped_v1(&stats);
}

static bool expect_panic(const char *name, void (*run)(),
                         const char *needle) noexcept {
  int fds[2];
  if (pipe(fds) != 0) {
    report_failure(__FILE__, __LINE__);
    return false;
  }
  const pid_t pid = fork();
  if (pid < 0) {
    report_failure(__FILE__, __LINE__);
    return false;
  }
  if (pid == 0) {
    close(fds[0]);
    if (dup2(fds[1], 2) < 0) _exit(97);
    close(fds[1]);
    run();
    _exit(0);
  }

  close(fds[1]);
  char record[1024];
  uint64_t used = 0;
  for (;;) {
    const ssize_t result =
        read(fds[0], record + used, sizeof(record) - 1 - used);
    if (result > 0) {
      used += static_cast<uint64_t>(result);
      if (used == sizeof(record) - 1) break;
      continue;
    }
    if (result < 0 && errno == EINTR) continue;
    break;
  }
  close(fds[0]);
  record[used] = '\0';

  int status = 0;
  while (waitpid(pid, &status, 0) < 0)
    if (errno != EINTR) {
      report_failure(__FILE__, __LINE__);
      return false;
    }

  if (!WIFEXITED(status) || WEXITSTATUS(status) != 101 ||
      !std::strstr(record, "LANG_NATIVE_PANIC:v1:") ||
      !std::strstr(record, needle)) {
    write_text("CYCLE_GC_PANIC_MISMATCH:");
    write_text(name);
    write_text(":");
    write_text(record);
    return false;
  }
  return true;
}

static bool test_statuses_and_fork_hooks() noexcept {
  auto *object = static_cast<RegistryOnly *>(
      lang_alloc_v1(sizeof(RegistryOnly), alignof(RegistryOnly)));
  REQUIRE(object != nullptr);
  object->refs = 1;

  REQUIRE(lang_gc_register_v1(nullptr, &g_registry_only_ops) == LANG_GC_INVALID);
  REQUIRE(lang_gc_register_v1(reinterpret_cast<void *>(1),
                              &g_registry_only_ops) == LANG_GC_INVALID);
  LangGcOps incomplete{};
  REQUIRE(lang_gc_register_v1(object, &incomplete) == LANG_GC_INVALID);
  REQUIRE(lang_gc_unregister_v1(nullptr) == LANG_GC_INVALID);
  REQUIRE(lang_gc_unregister_v1(object) == LANG_GC_NOT_REGISTERED);
  REQUIRE(lang_gc_register_v1(object, &g_registry_only_ops) == LANG_GC_OK);
  REQUIRE(lang_gc_register_v1(object, &g_registry_only_ops) ==
          LANG_GC_DUPLICATE);
  REQUIRE(lang_gc_unregister_v1(object) == LANG_GC_OK);
  registry_only_release(object);
  REQUIRE(lang_gc_registry_live_v1() == 0);
  REQUIRE(track_live_count() == 0);

  lang_gc_fork_prepare_v1();
  const pid_t pid = fork();
  if (pid < 0) {
    lang_gc_fork_parent_v1();
    report_failure(__FILE__, __LINE__);
    return false;
  }
  if (pid == 0) {
    lang_gc_fork_child_v1();
    auto *child_object = static_cast<RegistryOnly *>(
        lang_alloc_v1(sizeof(RegistryOnly), alignof(RegistryOnly)));
    if (!child_object) _exit(2);
    child_object->refs = 1;
    if (lang_gc_register_v1(child_object, &g_registry_only_ops) != LANG_GC_OK)
      _exit(3);
    if (lang_gc_unregister_v1(child_object) != LANG_GC_OK) _exit(4);
    registry_only_release(child_object);
    _exit(0);
  }
  lang_gc_fork_parent_v1();

  int status = 0;
  while (waitpid(pid, &status, 0) < 0)
    if (errno != EINTR) {
      report_failure(__FILE__, __LINE__);
      return false;
    }
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 0);
  REQUIRE(lang_gc_registry_live_v1() == 0);
  REQUIRE(track_live_count() == 0);
  return true;
}

static bool test_fork_with_registry_entries() noexcept {
  auto *first = static_cast<RegistryOnly *>(
      lang_alloc_v1(sizeof(RegistryOnly), alignof(RegistryOnly)));
  auto *second = static_cast<RegistryOnly *>(
      lang_alloc_v1(sizeof(RegistryOnly), alignof(RegistryOnly)));
  REQUIRE(first != nullptr);
  REQUIRE(second != nullptr);
  first->refs = 1;
  second->refs = 1;
  REQUIRE(lang_gc_register_v1(first, &g_registry_only_ops) == LANG_GC_OK);
  REQUIRE(lang_gc_register_v1(second, &g_registry_only_ops) == LANG_GC_OK);
  REQUIRE(lang_gc_registry_live_v1() == 2);

  lang_gc_fork_prepare_v1();
  const pid_t pid = fork();
  if (pid < 0) {
    lang_gc_fork_parent_v1();
    report_failure(__FILE__, __LINE__);
    return false;
  }
  if (pid == 0) {
    lang_gc_fork_child_v1();
    // The child inherits the list and hash chains.  A snapshot must still see
    // both entries, and inherited entries must unlink exactly like new ones.
    LangGcStats stats{};
    if (lang_gc_collect_stopped_v1(&stats) != LANG_GC_OK) _exit(2);
    if (stats.registered != 2 || stats.reachable != 2 ||
        stats.candidates != 0 || stats.registry_live_after != 2)
      _exit(3);
    if (lang_gc_unregister_v1(second) != LANG_GC_OK) _exit(4);
    if (lang_gc_unregister_v1(first) != LANG_GC_OK) _exit(5);
    if (lang_gc_unregister_v1(first) != LANG_GC_NOT_REGISTERED) _exit(6);
    if (lang_gc_register_v1(first, &g_registry_only_ops) != LANG_GC_OK)
      _exit(7);
    if (lang_gc_unregister_v1(first) != LANG_GC_OK) _exit(8);
    registry_only_release(first);
    registry_only_release(second);
    _exit(0);
  }
  lang_gc_fork_parent_v1();

  int status = 0;
  while (waitpid(pid, &status, 0) < 0)
    if (errno != EINTR) {
      report_failure(__FILE__, __LINE__);
      return false;
    }
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 0);
  // The parent's entries are untouched by the child's unregister calls.
  REQUIRE(lang_gc_registry_live_v1() == 2);
  REQUIRE(lang_gc_unregister_v1(second) == LANG_GC_OK);
  REQUIRE(lang_gc_unregister_v1(first) == LANG_GC_OK);
  registry_only_release(first);
  registry_only_release(second);
  REQUIRE(lang_gc_registry_live_v1() == 0);
  REQUIRE(track_live_count() == 0);
  return true;
}

static bool test_refcount_panics() noexcept {
  REQUIRE(expect_panic("zero-refs", panic_zero_refs,
                       "reference-count-overflow"));
  REQUIRE(expect_panic("incoming-exceeds-refs", panic_incoming_exceeds_refs,
                       "reference-count-overflow"));
  REQUIRE(expect_panic("retain-contract", panic_retain_contract,
                       "reference-count-overflow"));
  REQUIRE(expect_panic("clear-contract", panic_clear_contract,
                       "reference-count-overflow"));
  return true;
}

#endif // LANG_GC_FREESTANDING

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

struct TestCase {
  const char *name;
  bool (*run)();
};

} // namespace

#if defined(LANG_GC_FREESTANDING)
extern "C" int main() {
#else
int main() {
#endif
  const TestCase tests[] = {
      {"immediate-arc-empty", test_immediate_arc_and_empty_collection},
      {"self-cycle", test_self_cycle},
      {"two-node-cycle", test_two_node_cycle},
      {"multi-node-cycle", test_multi_node_cycle},
      {"duplicate-edges", test_duplicate_edges},
      {"shared-subcycle", test_shared_subcycle},
      {"root-then-unroot", test_root_reachable_then_unroot},
      {"dead-points-live", test_dead_graph_points_to_live_external},
      {"acyclic-long-chain", test_acyclic_long_chain},
      {"large-cycle-10000", test_large_cycle},
      {"max-u64", test_max_u64_boundary},
      {"clear-unregister", test_clear_callback_unregister},
      {"clear-resurrection",
       test_clear_callback_resurrection_is_not_collected},
      {"clear-allocate-nested",
       test_clear_callback_allocate_nested_collect},
      {"random-oracle", test_random_graph_oracle},
      {"registry-index", test_registry_index},
#ifndef LANG_GC_FREESTANDING
      {"concurrent-registry", test_concurrent_registration_then_collect},
      {"statuses-fork", test_statuses_and_fork_hooks},
      {"fork-registry", test_fork_with_registry_entries},
      {"refcount-panics", test_refcount_panics},
#endif
  };

  for (const TestCase &test : tests) {
    const bool passed = test.run();
    report_test(test.name, passed);
    if (!passed) return 1;
  }

  if (track_live_count() != 0 || g_track_double_free != 0 ||
      g_track_bad_free != 0 || g_track_table_overflow != 0) {
    report_failure(__FILE__, __LINE__);
    return 1;
  }
  write_text("CYCLE_GC_ALL_PASS\n");
  return 0;
}
