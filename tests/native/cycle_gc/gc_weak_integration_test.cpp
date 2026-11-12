// Real GC + Weak integration regression for lang-gc/1 + the Weak control ABI.
//
// The collector must pin all candidates, invalidate every candidate Weak slot,
// and only then run the first clear callback.  This file builds the exact
// integration counterexample (A/B white cycle, clear(A) upgrades Weak(B)) plus
// root-reachable, closed-slot final release, and unsafe clear-resurrection
// cases.  It builds hosted and as a no-libc static ELF.

#include "../../../runtime/native/gc.hpp"
#include "../../../runtime/native/weak.hpp"
#include "../../../runtime/native/alloc.hpp"

#include <cstddef>
#include <cstdint>

namespace {

constexpr uintptr_t kClosedSlotTag = 1;
constexpr uint64_t kTrackMagic = 0x47435745414b5452ULL;

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
  write_text("GC_WEAK_FAIL:");
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

// ---------------------------------------------------------------------------
// Link-time allocation tracker for exact object/control/registry cleanup
// ---------------------------------------------------------------------------

extern "C" void *__real_lang_alloc_v1(uint64_t, uint64_t) noexcept;
extern "C" void __real_lang_free_v1(void *) noexcept;

uint64_t g_live_allocations;
uint64_t g_total_allocations;
uint64_t g_total_frees;
uint64_t g_bad_frees;

struct TrackHeader {
  uint64_t magic;
  void *real;
  uint64_t requested;
  uint64_t user;
};

extern "C" void *__wrap_lang_alloc_v1(uint64_t size,
                                      uint64_t align) noexcept {
  if (align == 0 || (align & (align - 1)) != 0) {
    return __real_lang_alloc_v1(size, align);
  }
  if (align > 0x10000000ULL) return nullptr;
  if (align < 16) align = 16;
  if (size > ~uint64_t{0} - sizeof(TrackHeader) - align) return nullptr;
  void *real = __real_lang_alloc_v1(size + sizeof(TrackHeader) + align,
                                    alignof(TrackHeader));
  if (!real) return nullptr;
  const uintptr_t base =
      reinterpret_cast<uintptr_t>(real) + sizeof(TrackHeader);
  const uintptr_t user = (base + align - 1) & ~(static_cast<uintptr_t>(align) - 1);
  auto *header = reinterpret_cast<TrackHeader *>(user - sizeof(TrackHeader));
  header->magic = kTrackMagic;
  header->real = real;
  header->requested = size;
  header->user = user;
  ++g_total_allocations;
  ++g_live_allocations;
  return reinterpret_cast<void *>(user);
}

extern "C" void __wrap_lang_free_v1(void *pointer) noexcept {
  if (!pointer) {
    __real_lang_free_v1(nullptr);
    return;
  }
  auto *header = reinterpret_cast<TrackHeader *>(
      reinterpret_cast<uintptr_t>(pointer) - sizeof(TrackHeader));
  if (header->magic != kTrackMagic ||
      header->user != reinterpret_cast<uintptr_t>(pointer)) {
    ++g_bad_frees;
    return;
  }
  void *real = header->real;
  header->magic = 0;
  ++g_total_frees;
  if (!g_live_allocations) {
    ++g_bad_frees;
  } else {
    --g_live_allocations;
  }
  __real_lang_free_v1(real);
}

// ---------------------------------------------------------------------------
// ARC object with a canonical Weak slot
// ---------------------------------------------------------------------------

struct Object {
  uint64_t refs;
  LangWeakControl *weak_slot;
  Object *edge;
  uint64_t id;
  void (*clear_hook)(Object *) noexcept;
  int cleared;
};

uint64_t g_destroyed;
uint64_t g_bad_refcount;
uint64_t g_bad_unregister;
uint64_t g_invalidations;
uint64_t g_peer_refs_at_invalidate;
LangWeakControl *g_weak_b;
Object *g_peer_b;
Object *g_self;
LangWeakControl *g_weak_self;

struct FirstClearProbe {
  bool done;
  bool peer_alive;
  void *upgraded_peer;
  LangWeakControl *reinstalled_peer;
  bool peer_slot_closed;
  bool all_invalidated;
  uint64_t peer_refs_at_invalidate;
};

FirstClearProbe g_probe;

static void object_trace(void *opaque, LangGcVisitor visitor,
                         void *context) noexcept;
static void object_clear(void *opaque) noexcept;
static void object_retain(void *opaque) noexcept;
static void object_release(void *opaque) noexcept;
static void object_invalidate_weak(void *opaque) noexcept;

static const LangGcOps g_ops = {object_trace, object_clear, object_retain,
                                object_release, object_invalidate_weak};

static bool slot_is_closed(const LangWeakControl *slot) noexcept {
  return reinterpret_cast<uintptr_t>(slot) == kClosedSlotTag;
}

static void object_trace(void *opaque, LangGcVisitor visitor,
                         void *context) noexcept {
  auto *object = static_cast<Object *>(opaque);
  if (object->edge) visitor(object->edge, context);
}

static void object_clear(void *opaque) noexcept {
  auto *object = static_cast<Object *>(opaque);
  if (object->clear_hook) object->clear_hook(object);
  Object *edge = object->edge;
  object->edge = nullptr;
  if (edge) object_release(edge);
  object->cleared = 1;
}

static void object_retain(void *opaque) noexcept {
  auto *object = static_cast<Object *>(opaque);
  uint64_t refs = __atomic_load_n(&object->refs, __ATOMIC_RELAXED);
  for (;;) {
    if (refs == ~uint64_t{0}) {
      ++g_bad_refcount;
      return;
    }
    if (__atomic_compare_exchange_n(&object->refs, &refs, refs + 1, false,
                                    __ATOMIC_RELAXED, __ATOMIC_RELAXED))
      return;
  }
}

static void object_release(void *opaque) noexcept {
  auto *object = static_cast<Object *>(opaque);
  uint64_t refs = __atomic_load_n(&object->refs, __ATOMIC_RELAXED);
  for (;;) {
    if (!refs) {
      ++g_bad_refcount;
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
  LangWeakControl *slot =
      __atomic_load_n(&object->weak_slot, __ATOMIC_ACQUIRE);
  // The slot may be null, the permanent closed tag, or a real control block.
  // detach treats null/tag as harmless and only releases the implicit Weak
  // reference for a real control.
  lang_weak_detach_v1(slot);
  const LangGcStatus status = lang_gc_unregister_v1(object);
  if (status != LANG_GC_OK && status != LANG_GC_NOT_REGISTERED)
    ++g_bad_unregister;
  ++g_destroyed;
  lang_free_v1(object);
}

static void object_invalidate_weak(void *opaque) noexcept {
  auto *object = static_cast<Object *>(opaque);
  ++g_invalidations;
  if (object == g_peer_b)
    g_peer_refs_at_invalidate =
        __atomic_load_n(&object->refs, __ATOMIC_RELAXED);
  lang_weak_invalidate_slot_v1(&object->weak_slot);
}

static Object *make_object(uint64_t id) noexcept {
  auto *object =
      static_cast<Object *>(lang_alloc_v1(sizeof(Object), alignof(Object)));
  if (!object) return nullptr;
  object->refs = 1;
  object->weak_slot = nullptr;
  object->edge = nullptr;
  object->id = id;
  object->clear_hook = nullptr;
  object->cleared = 0;
  if (lang_gc_register_v1(object, &g_ops) != LANG_GC_OK) {
    lang_free_v1(object);
    return nullptr;
  }
  return object;
}

static bool add_edge(Object *from, Object *to) noexcept {
  if (!from || !to || from->edge) return false;
  object_retain(to);
  from->edge = to;
  return true;
}

static void reset_test_state() noexcept {
  g_destroyed = 0;
  g_bad_refcount = 0;
  g_bad_unregister = 0;
  g_invalidations = 0;
  g_peer_refs_at_invalidate = 0;
  g_weak_b = nullptr;
  g_peer_b = nullptr;
  g_self = nullptr;
  g_weak_self = nullptr;
  g_probe = {};
}

static bool allocations_clean() noexcept {
  REQUIRE(g_live_allocations == 0);
  REQUIRE(g_total_allocations == g_total_frees);
  REQUIRE(g_bad_frees == 0);
  REQUIRE(lang_gc_registry_live_v1() == 0);
  REQUIRE(g_bad_refcount == 0);
  REQUIRE(g_bad_unregister == 0);
  return true;
}

static void first_clear_probe(Object *) noexcept {
  if (g_probe.done) return;
  g_probe.done = true;
  g_probe.peer_alive = lang_weak_alive_v1(g_weak_b);
  g_probe.upgraded_peer = lang_weak_upgrade_v1(g_weak_b);
  g_probe.reinstalled_peer =
      lang_weak_downgrade_v1(g_peer_b, &g_peer_b->weak_slot);
  g_probe.peer_slot_closed = slot_is_closed(g_peer_b->weak_slot);
  g_probe.all_invalidated = (g_invalidations == 2);
  g_probe.peer_refs_at_invalidate = g_peer_refs_at_invalidate;
  if (g_probe.upgraded_peer)
    object_release(g_probe.upgraded_peer); // keep the failing run leak-free
}

static bool test_white_weak_invalidated_before_clear() noexcept {
  reset_test_state();
  Object *a = make_object(1);
  Object *b = make_object(2);
  REQUIRE(a && b);
  REQUIRE(add_edge(a, b));
  REQUIRE(add_edge(b, a));

  g_peer_b = b;
  g_weak_b = lang_weak_downgrade_v1(b, &b->weak_slot);
  REQUIRE(g_weak_b != nullptr);
  REQUIRE(lang_weak_alive_v1(g_weak_b));

  a->clear_hook = first_clear_probe;
  b->clear_hook = first_clear_probe;

  object_release(a); // drop construction refs; both become white candidates
  object_release(b);

  LangGcStats stats{};
  REQUIRE(lang_gc_collect_stopped_v1(&stats) == LANG_GC_OK);
  REQUIRE(stats.candidates == 2);
  REQUIRE(stats.pinned == 2);
  REQUIRE(stats.cleared == 2);
  REQUIRE(stats.collected == 2);
  REQUIRE(stats.registry_live_after == 0);
  REQUIRE(g_destroyed == 2);

  REQUIRE(g_probe.done);
  REQUIRE(!g_probe.peer_alive);
  REQUIRE(g_probe.upgraded_peer == nullptr);
  REQUIRE(g_probe.reinstalled_peer == nullptr);
  REQUIRE(g_probe.peer_slot_closed);
  REQUIRE(g_probe.all_invalidated); // both candidates invalidated before clear #1
  REQUIRE(g_probe.peer_refs_at_invalidate == 2); // all candidates pinned first

  // The old Weak value remains a valid dead control block: copy/drop works,
  // upgrade/alive stay dead, then its final real Weak owner frees it.
  REQUIRE(!lang_weak_alive_v1(g_weak_b));
  REQUIRE(lang_weak_upgrade_v1(g_weak_b) == nullptr);
  lang_weak_retain_v1(g_weak_b);
  lang_weak_release_v1(g_weak_b);
  lang_weak_release_v1(g_weak_b);
  g_weak_b = nullptr;
  return allocations_clean();
}

static void self_upgrade_then_retain(Object *object) noexcept {
  // Unsafe internal resurrection: the clear callback retains itself.  This is
  // allowed only as an internal test of the post-clear state; Weak must stay
  // dead and must never be revived by the resurrected strong owner.
  g_probe.upgraded_peer = lang_weak_upgrade_v1(g_weak_self);
  object_retain(object);
  g_probe.peer_alive = lang_weak_upgrade_v1(g_weak_self) != nullptr;
}

static bool test_clear_resurrection_does_not_revive_weak() noexcept {
  reset_test_state();
  Object *object = make_object(3);
  REQUIRE(object);
  REQUIRE(add_edge(object, object));
  g_self = object;
  g_weak_self = lang_weak_downgrade_v1(object, &object->weak_slot);
  REQUIRE(g_weak_self != nullptr);
  object->clear_hook = self_upgrade_then_retain;
  object_release(object); // construction ref leaves the self-edge only

  LangGcStats stats{};
  REQUIRE(lang_gc_collect_stopped_v1(&stats) == LANG_GC_OK);
  REQUIRE(stats.candidates == 1);
  REQUIRE(stats.cleared == 1);
  REQUIRE(stats.collected == 0); // clear retained itself; pin is not the last ref
  REQUIRE(stats.registry_live_after == 1);
  REQUIRE(g_destroyed == 0);
  REQUIRE(g_probe.upgraded_peer == nullptr);
  REQUIRE(!g_probe.peer_alive);
  REQUIRE(g_invalidations == 1);
  REQUIRE(!lang_weak_alive_v1(g_weak_self));
  REQUIRE(lang_weak_upgrade_v1(g_weak_self) == nullptr);
  REQUIRE(slot_is_closed(object->weak_slot));
  REQUIRE(object->cleared == 1);

  // Values read through the unsafe strong escape see post-clear state.  Drop
  // that internal escape; final release sees the closed slot and does not read
  // a freed control block.
  object_release(object);
  REQUIRE(g_destroyed == 1);
  lang_weak_release_v1(g_weak_self);
  g_weak_self = nullptr;
  return allocations_clean();
}

static bool test_reachable_weak_survives_then_dies_after_unroot() noexcept {
  reset_test_state();
  Object *a = make_object(4);
  Object *b = make_object(5);
  REQUIRE(a && b);
  REQUIRE(add_edge(a, b));
  REQUIRE(add_edge(b, a));
  g_peer_b = b;
  g_weak_b = lang_weak_downgrade_v1(b, &b->weak_slot);
  REQUIRE(g_weak_b != nullptr);
  object_release(b); // a's construction reference remains the external root

  LangGcStats rooted{};
  REQUIRE(lang_gc_collect_stopped_v1(&rooted) == LANG_GC_OK);
  REQUIRE(rooted.candidates == 0);
  REQUIRE(rooted.collected == 0);
  REQUIRE(rooted.registry_live_after == 2);
  REQUIRE(g_invalidations == 0); // reachable objects are not white candidates
  REQUIRE(!slot_is_closed(b->weak_slot));
  REQUIRE(lang_weak_alive_v1(g_weak_b));
  void *upgraded = lang_weak_upgrade_v1(g_weak_b);
  REQUIRE(upgraded == b);
  object_release(upgraded);
  REQUIRE(g_destroyed == 0);

  object_release(a); // remove the root; the cycle becomes white
  LangGcStats collected{};
  REQUIRE(lang_gc_collect_stopped_v1(&collected) == LANG_GC_OK);
  REQUIRE(collected.candidates == 2);
  REQUIRE(collected.collected == 2);
  REQUIRE(collected.registry_live_after == 0);
  REQUIRE(g_destroyed == 2);
  REQUIRE(g_invalidations == 2);
  REQUIRE(!lang_weak_alive_v1(g_weak_b));
  REQUIRE(lang_weak_upgrade_v1(g_weak_b) == nullptr);
  lang_weak_release_v1(g_weak_b);
  g_weak_b = nullptr;
  return allocations_clean();
}

static bool run_all() noexcept {
  REQUIRE(test_white_weak_invalidated_before_clear());
  REQUIRE(test_clear_resurrection_does_not_revive_weak());
  REQUIRE(test_reachable_weak_survives_then_dies_after_unroot());
  write_text("GC_WEAK_ALL_PASS\n");
  return true;
}

} // namespace

#if defined(LANG_GC_FREESTANDING)
extern "C" int main() {
#else
int main() {
#endif
  return run_all() ? 0 : 1;
}
