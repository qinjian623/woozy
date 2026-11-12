#ifndef LANG_RUNTIME_NATIVE_GC_PROGRAM_HPP
#define LANG_RUNTIME_NATIVE_GC_PROGRAM_HPP

#include <cstdint>

#include "gc.hpp"

// Private native program integration ABI: lang-gc-program/1.
//
// This layer is the small lifecycle/guard surface between a generated Native
// program and the accepted layers:
//
//   lang-gc/1          stopped-world cycle collector  (gc.hpp)
//   lang-gc-world/1    mutator registry / SAFE / fork (gc_world.hpp)
//   lang-gc-runtime/1  pressure controller / defer     (gc_runtime.hpp)
//
// It does not emit source-level polls or critical regions, does not register
// primitive numeric Vecs, and does not claim that arbitrary Wz source is
// automatically collected. The compiler's outermost main wrapper owns the
// normal-return path:
//
//   if (lang_gc_program_init_v1() != 0) {
//       /* emit the normal typed error channel, then return/exit */
//   }
//   ... run the source program and perform all wrapper cleanup ...
//   (void)lang_gc_program_finish_v1();
//
// `finish` performs one finite final collection and then unregisters the main
// thread. It must be called after all wrapper cleanup and outside SAFE and
// critical regions. A Panic exits through the terminal panic path and performs
// no GC cleanup; this layer does not install a panic finalizer.
//
// Default/fast path: before `init`, the enabled word is zero and every guard is
// one atomic load plus a branch. No gettid, registry lookup, allocation, or
// primitive-Vec registration happens. Registration is opt-in through
// `lang_gc_program_register_v1`, which returns false while the program is not
// enabled. The Native backend must register only objects whose static type can
// hold a Wz owned edge, and must trace only the initialized Vec prefix.
//
// Threads: the runtime thread runner calls `thread_enter` before the language
// entry and `thread_leave` after the ThreadState context_drop. The ThreadState
// context stays an external strong root; its possibly stale bytes are never
// registered as a graph object. An already registered thread is not
// unregistered by this layer.
//
// Blocking runtime hooks enter SAFE only after their graph writes are complete
// and while they hold no allocator, environment, launch, or other runtime lock.
// A hook returns to ACTIVE before touching the graph again. Allocator-internal
// mmap and threads holding allocator/environment/launch locks are deliberately
// not parked as SAFE.
//
// Fork order (only when the program is enabled):
//
//   program fork scope: suspend every outer SAFE level, enter the graph-stable
//                       fork no-park interval
//   launch lock
//   environment lock
//   allocator prepare
//   Weak prepare
//   lang-gc prepare
//   lang-gc-world prepare
//   lang-gc-runtime prepare
//   fork
//   parent: runtime parent, world parent, gc parent, Weak parent,
//           allocator parent, environment unlock, launch unlock,
//           restore the saved SAFE depth, leave the no-park interval
//   child:  allocator child, Weak child, gc child, world child,
//           runtime child, reset runtime launch/environment locks, thread tid,
//           restore the saved SAFE depth, leave the no-park interval
//
// The child keeps only the calling thread registration and resets runtime
// locks; it does not fabricate ARC drops for vanished threads. The fork scope
// is entered before launch so a pending stop is waited out without holding
// launch. Outer SAFE depths are saved on the stack token and restored exactly.
// During the no-park interval synchronous callbacks and SAFE regions remain
// legal; the node stays ACTIVE for stop/collection and a pending request is
// handled when the interval ends after fork locks are released. A genuine
// graph-critical region nested inside still rejects callbacks and SAFE.
// Direct hosted libc fork is handled by the same atfork hooks. A fork from an
// unregistered foreign thread is outside the world contract and Panics.

typedef struct LangGcProgramForkToken {
  uint32_t magic;
  uint32_t active;
  uint32_t safe_depth;
  uint32_t main_caller;
  int32_t caller_tid;
  uint32_t reserved;
} LangGcProgramForkToken;

#ifdef __cplusplus
extern "C" {
#define LANG_GC_PROGRAM_NOEXCEPT noexcept
#else
#define LANG_GC_PROGRAM_NOEXCEPT
#endif

#define LANG_GC_PROGRAM_OK ((int64_t)0)
// init/finish was called in the wrong lifecycle state. The wrapper should
// report its own typed error; this runtime never prints a source error.
#define LANG_GC_PROGRAM_ERROR_STATE ((int64_t)-1)
// finish skipped collection because this thread was not an ACTIVE collector
// candidate (normally an outstanding stop, SAFE, or critical region).
#define LANG_GC_PROGRAM_SKIPPED_BUSY ((int64_t)-16)
// finish's finite final collection timed out; the graph is unchanged and the
// world request was retracted by the world layer.
#define LANG_GC_PROGRAM_TIMED_OUT ((int64_t)-110)

// Register the calling thread with lang-gc-world/1 and enable the integration
// guards. Returns 0 on success. Repeated init, init after finish, and other
// lifecycle misuse return LANG_GC_PROGRAM_ERROR_STATE without changing the
// existing lifecycle state. A world registration failure is a terminal Panic.
int64_t lang_gc_program_init_v1(void) LANG_GC_PROGRAM_NOEXCEPT;

// One finite-stop final collection (at most LANG_GC_PROGRAM_FINAL_COLLECT_NS),
// then remove the main thread registration (even if it pre-existed init). Returns 0
// on success, a negative collection status on failure/skip, or
// LANG_GC_PROGRAM_ERROR_STATE if not initialized. The timeout bounds stop
// acquisition, not arbitrary trace/clear/foreign-cleanup wall time. The caller
// must be outside SAFE/critical and must already have released main-owned
// values; unregistering from SAFE/critical is a world Panic.
//
// The enabled word and hooks intentionally remain active until process exit.
// A detached language thread may still be registered and may final-release
// objects after main returns; unregister bookkeeping must keep removing those
// nodes so a later poll/collection cannot read freed storage. finish is final
// for the main wrapper, not a runtime shutdown that disables worker hooks. The
// main tid's SAFE/critical/thread/fork entry hooks become no-ops after finish so
// host atexit/destructor code cannot enter the world for an unregistered main.
int64_t lang_gc_program_finish_v1(void) LANG_GC_PROGRAM_NOEXCEPT;

// Registration wrapper. The object must be fully constructed. Returns true
// only when the program is enabled and lang_gc_register_v1 reports LANG_GC_OK;
// false covers disabled, duplicate, invalid, and already-registered misuse.
bool lang_gc_program_register_v1(void *object,
                                 const LangGcOps *ops) LANG_GC_PROGRAM_NOEXCEPT;

// Unregistration wrapper. It is a cheap no-op while the program is disabled.
// A normal final strong release must call this before dropping the payload.
void lang_gc_program_unregister_v1(void *object) LANG_GC_PROGRAM_NOEXCEPT;

// C ABI SAFE/critical entry. enter returns 1 only when the program is enabled
// and the corresponding world region was entered; leave is a no-op for 0.
// Generated code may use these around a foreign call. Runtime hooks prefer the
// inline guards below, which avoid the call on the disabled fast path.
int32_t lang_gc_program_enter_safe_v1(void) LANG_GC_PROGRAM_NOEXCEPT;
void lang_gc_program_leave_safe_v1(int32_t entered) LANG_GC_PROGRAM_NOEXCEPT;
int32_t lang_gc_program_enter_critical_v1(void) LANG_GC_PROGRAM_NOEXCEPT;
void lang_gc_program_leave_critical_v1(int32_t entered) LANG_GC_PROGRAM_NOEXCEPT;

// Runtime thread-runner hooks. enter returns 1 only when this call created the
// registration; leave unregisters only that registration.
int32_t lang_gc_program_thread_enter_v1(void) LANG_GC_PROGRAM_NOEXCEPT;
void lang_gc_program_thread_leave_v1(int32_t registered) LANG_GC_PROGRAM_NOEXCEPT;

// Stack-scoped fork hooks. One distinct, zero-initialized token per fork. The
// enter hook must run before launch/environment/allocator locks are acquired;
// leave must run after all runtime fork locks are released. A token may cross
// fork only on the same stack and only through the matching leave scope.
void lang_gc_program_fork_enter_v1(
    LangGcProgramForkToken *token) LANG_GC_PROGRAM_NOEXCEPT;
void lang_gc_program_fork_leave_v1(
    LangGcProgramForkToken *token) LANG_GC_PROGRAM_NOEXCEPT;

#ifdef __cplusplus
} // extern "C"
#endif

#undef LANG_GC_PROGRAM_NOEXCEPT

// 10 ms: the same finite stop-acquisition budget used by the automatic
// controller. It is not a hard upper bound on arbitrary trace/clear/foreign
// cleanup work.
#define LANG_GC_PROGRAM_FINAL_COLLECT_NS ((int64_t)10000000)

#ifdef __cplusplus

namespace lang {
namespace native {
namespace gc_program {

// One inline variable shared by every runtime translation unit. It is zero
// before init, set to one after the world registration succeeds, and remains
// enabled until process exit, including after main unregisters. Hooks use this instead of
// a linked symbol so a test that compiles a single runtime unit keeps working.
inline uint32_t enabled_word_v1 = 0;

inline bool enabled() noexcept {
  return __atomic_load_n(&enabled_word_v1, __ATOMIC_ACQUIRE) != 0;
}

// Runtime-internal hooks installed once by init. They are function pointers in
// a header-defined inline aggregate so runtime units that do not link
// gc_program.cpp keep their previous behavior (all pointers null). The pointed
// functions are stable after init; finish never clears a pointer, so there is
// no concurrent pointer publication race.
struct Hooks {
  int32_t (*enter_safe)(void) noexcept = nullptr;
  void (*leave_safe)(int32_t) noexcept = nullptr;
  int32_t (*enter_critical)(void) noexcept = nullptr;
  void (*leave_critical)(int32_t) noexcept = nullptr;
  int32_t (*thread_enter)(void) noexcept = nullptr;
  void (*thread_leave)(int32_t) noexcept = nullptr;
  void (*fork_enter)(LangGcProgramForkToken *) noexcept = nullptr;
  void (*fork_leave)(LangGcProgramForkToken *) noexcept = nullptr;
  // True only for a thread that should run the GC/world/controller fork hooks
  // now. Main after finish is false; surviving worker tids are true.
  int32_t (*fork_ready)(void) noexcept = nullptr;
  // Vec final strong release: unregister before element/payload drop.
  void (*vec_final_release)(void *) noexcept = nullptr;
  // Complete fork order for the three accepted GC layers.
  void (*gc_fork_prepare)(void) noexcept = nullptr;
  void (*gc_fork_parent)(void) noexcept = nullptr;
  void (*gc_fork_child)(void) noexcept = nullptr;
  void (*world_fork_prepare)(void) noexcept = nullptr;
  void (*world_fork_parent)(void) noexcept = nullptr;
  void (*world_fork_child)(void) noexcept = nullptr;
  void (*runtime_fork_prepare)(void) noexcept = nullptr;
  void (*runtime_fork_parent)(void) noexcept = nullptr;
  void (*runtime_fork_child)(void) noexcept = nullptr;
};

inline Hooks hooks_v1{};

// Nestable blocking/foreign-call guard. The disabled path performs one atomic
// load and a pointer check; it does not gettid or look up the registry.
class SafeGuard {
 public:
  SafeGuard() noexcept {
    if (enabled() && hooks_v1.enter_safe)
      armed_ = hooks_v1.enter_safe();
  }
  ~SafeGuard() noexcept { release(); }
  SafeGuard(const SafeGuard &) = delete;
  SafeGuard &operator=(const SafeGuard &) = delete;
  void release() noexcept {
    if (!armed_)
      return;
    armed_ = 0;
    if (hooks_v1.leave_safe)
      hooks_v1.leave_safe(1);
  }

 private:
  int32_t armed_ = 0;
};

class CriticalGuard {
 public:
  CriticalGuard() noexcept {
    if (enabled() && hooks_v1.enter_critical)
      armed_ = hooks_v1.enter_critical();
  }
  ~CriticalGuard() noexcept { release(); }
  CriticalGuard(const CriticalGuard &) = delete;
  CriticalGuard &operator=(const CriticalGuard &) = delete;
  void release() noexcept {
    if (!armed_)
      return;
    armed_ = 0;
    if (hooks_v1.leave_critical)
      hooks_v1.leave_critical(1);
  }

 private:
  int32_t armed_ = 0;
};

class ThreadGuard {
 public:
  ThreadGuard() noexcept {
    if (enabled() && hooks_v1.thread_enter)
      armed_ = hooks_v1.thread_enter();
  }
  ~ThreadGuard() noexcept { release(); }
  ThreadGuard(const ThreadGuard &) = delete;
  ThreadGuard &operator=(const ThreadGuard &) = delete;
  void release() noexcept {
    if (!armed_)
      return;
    armed_ = 0;
    if (hooks_v1.thread_leave)
      hooks_v1.thread_leave(1);
  }

 private:
  int32_t armed_ = 0;
};

} // namespace gc_program
} // namespace native
} // namespace lang

#endif // __cplusplus

#endif // LANG_RUNTIME_NATIVE_GC_PROGRAM_HPP
