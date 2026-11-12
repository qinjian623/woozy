#ifndef LANG_RUNTIME_NATIVE_GC_HPP
#define LANG_RUNTIME_NATIVE_GC_HPP

#include <cstdint>

// Private native cycle collector ABI: lang-gc/1.
//
// A registered object begins with a u64 atomic strong reference count.  The
// registry never retains a target.  Only objects whose static type can hold a
// Wz heap-owner strong reference need registration.  See
// tests/native/cycle_gc/CONTRACT.md for the complete callback contract.
extern "C" {

typedef int32_t LangGcStatus;

#define LANG_GC_OK ((LangGcStatus)0)
#define LANG_GC_INVALID ((LangGcStatus)1)
#define LANG_GC_DUPLICATE ((LangGcStatus)2)
#define LANG_GC_NOT_REGISTERED ((LangGcStatus)3)
#define LANG_GC_BUSY ((LangGcStatus)4)

// The visitor receives one direct owned edge per call.  Duplicate edges are
// separate calls.  The target may be absent from the registered snapshot; the
// collector then treats that edge as external to the registered graph.
typedef void (*LangGcVisitor)(void *target, void *context) noexcept;

typedef struct LangGcOps {
  void (*trace)(void *object, LangGcVisitor visitor, void *context) noexcept;
  void (*clear)(void *object) noexcept;
  void (*retain)(void *object) noexcept;
  void (*release)(void *object) noexcept;
  // Optional collector-internal callback.  After every candidate is retained
  // and before any candidate is cleared, the collector calls this for each
  // candidate in the stopped world.  It must only close the object's canonical
  // Weak slot (lang_weak_invalidate_slot_v1) and must not run user code.
  // Types without Weak leave this trailing field null; four-element aggregate
  // initialization remains valid.
  void (*invalidate_weak)(void *object) noexcept;
} LangGcOps;

typedef struct LangGcStats {
  uint64_t registered;
  uint64_t traced_edges;
  uint64_t reachable;
  uint64_t candidates;
  uint64_t pinned;
  uint64_t cleared;
  uint64_t collected;
  uint64_t registry_live_after;
} LangGcStats;

LangGcStatus lang_gc_register_v1(void *object, const LangGcOps *ops) noexcept;
LangGcStatus lang_gc_unregister_v1(void *object) noexcept;
uint64_t lang_gc_registry_live_v1() noexcept;

// Root must already have stopped all mutators.  This function performs no
// safepoint wait and no automatic mutator stopping.
LangGcStatus lang_gc_collect_stopped_v1(LangGcStats *out) noexcept;

// Independent fork hooks.  The child resets inherited lock/collection-state
// words; the registry contents themselves stay valid.
void lang_gc_fork_prepare_v1() noexcept;
void lang_gc_fork_parent_v1() noexcept;
void lang_gc_fork_child_v1() noexcept;

}

#endif
