#include "gc_program.hpp"

#include "alloc.hpp"
#include "gc_runtime.hpp"
#include "gc_world.hpp"
#include "linux.hpp"

#if !defined(__x86_64__) || !defined(__linux__)
#error "lang-gc-program/1 is implemented only for Linux x86_64"
#endif

// Program lifecycle/guard layer. It uses only atomics and the three accepted
// GC layers; there is no libc, libstdc++, pthread, TLS, or dynamic
// initialization. The default disabled path is a single atomic load (and, for
// runtime hooks, one null-pointer check).

extern "C" [[noreturn]] void lang_native_panic_v1(uint32_t, uint64_t, uint64_t,
                                                   uint64_t) noexcept;

namespace {

using lang::native::gc_program::hooks_v1;

constexpr uint32_t kStateNew = 0;
constexpr uint32_t kStateActive = 1;
constexpr uint32_t kStateFinished = 2;
constexpr uint32_t kForkTokenMagic = 0x47435046u; // "GCPF"

uint32_t g_state = kStateNew;
uint32_t g_main_finished = 0;
int32_t g_main_tid = 0;

[[noreturn]] __attribute__((no_stack_protector)) void
program_panic(const char *reason) noexcept {
  (void)reason;
  // The shared scalar panic already writes LANG_NATIVE_PANIC:v1 and exits 101.
  // Cause 10 is the runtime-handle-misuse record; this path is only reached by
  // unbalanced/corrupted private fork tokens.
  lang_native_panic_v1(10, 0, 0, 0);
}

int32_t raw_current_tid() noexcept {
  const int64_t tid =
      lang::native::linux_x86_64::syscall6(186, 0, 0, 0, 0, 0, 0);
  return tid > 0 ? static_cast<int32_t>(tid) : 0;
}

// After main finish the main thread is unregistered, while detached language
// threads continue with live hooks. Runtime calls made by the main thread after
// its final collection (for example from a host atexit/destructor) must not
// enter a world region for an unregistered thread.
bool main_after_finish() noexcept {
  if (!__atomic_load_n(&g_main_finished, __ATOMIC_ACQUIRE))
    return false;
  return raw_current_tid() ==
         __atomic_load_n(&g_main_tid, __ATOMIC_ACQUIRE);
}

int32_t enter_safe_hook() noexcept {
  if (!lang::native::gc_program::enabled() || main_after_finish())
    return 0;
  lang_gc_world_enter_safe_v1();
  return 1;
}

void leave_safe_hook(int32_t entered) noexcept {
  if (entered)
    lang_gc_world_leave_safe_v1();
}

int32_t enter_critical_hook() noexcept {
  if (!lang::native::gc_program::enabled() || main_after_finish())
    return 0;
  lang_gc_world_enter_critical_v1();
  return 1;
}

void leave_critical_hook(int32_t entered) noexcept {
  if (entered)
    lang_gc_world_leave_critical_v1();
}

int32_t fork_ready_hook() noexcept {
  return lang::native::gc_program::enabled() && !main_after_finish() ? 1 : 0;
}

int32_t thread_enter_hook() noexcept {
  if (!lang::native::gc_program::enabled() || main_after_finish())
    return 0;
  return lang_gc_world_register_v1() ? 1 : 0;
}

void thread_leave_hook(int32_t registered) noexcept {
  if (registered)
    (void)lang_gc_world_unregister_v1();
}

void vec_final_release_hook(void *object) noexcept {
  if (!lang::native::gc_program::enabled())
    return;
  (void)lang_gc_runtime_unregister_v1(object);
}

void install_hooks() noexcept {
  hooks_v1.enter_safe = &enter_safe_hook;
  hooks_v1.leave_safe = &leave_safe_hook;
  hooks_v1.enter_critical = &enter_critical_hook;
  hooks_v1.leave_critical = &leave_critical_hook;
  hooks_v1.thread_enter = &thread_enter_hook;
  hooks_v1.thread_leave = &thread_leave_hook;
  hooks_v1.fork_enter = &lang_gc_program_fork_enter_v1;
  hooks_v1.fork_leave = &lang_gc_program_fork_leave_v1;
  hooks_v1.fork_ready = &fork_ready_hook;
  hooks_v1.vec_final_release = &vec_final_release_hook;
  hooks_v1.gc_fork_prepare = &lang_gc_fork_prepare_v1;
  hooks_v1.gc_fork_parent = &lang_gc_fork_parent_v1;
  hooks_v1.gc_fork_child = &lang_gc_fork_child_v1;
  hooks_v1.world_fork_prepare = &lang_gc_world_fork_prepare_v1;
  hooks_v1.world_fork_parent = &lang_gc_world_fork_parent_v1;
  hooks_v1.world_fork_child = &lang_gc_world_fork_child_v1;
  hooks_v1.runtime_fork_prepare = &lang_gc_runtime_fork_prepare_v1;
  hooks_v1.runtime_fork_parent = &lang_gc_runtime_fork_parent_v1;
  hooks_v1.runtime_fork_child = &lang_gc_runtime_fork_child_v1;
}

} // namespace

extern "C" int64_t lang_gc_program_init_v1(void) noexcept {
  uint32_t expected = kStateNew;
  if (!__atomic_compare_exchange_n(&g_state, &expected, kStateActive, false,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return LANG_GC_PROGRAM_ERROR_STATE;

  // Install hooks before world registration. They still observe the disabled
  // word until the end of this function, so a registration-time allocation or
  // safepoint cannot enter a half-initialized guard.
  install_hooks();
  const int32_t main_tid = raw_current_tid();
  if (!main_tid)
    program_panic("gettid-failed");
  __atomic_store_n(&g_main_tid, main_tid, __ATOMIC_RELEASE);
  // Explicit world registration for the main thread. A repeated registration
  // is not an init failure; finish removes the main node either way.
  (void)lang_gc_world_register_v1();
  __atomic_store_n(&lang::native::gc_program::enabled_word_v1, 1u,
                   __ATOMIC_RELEASE);
  return LANG_GC_PROGRAM_OK;
}

extern "C" int64_t lang_gc_program_finish_v1(void) noexcept {
  uint32_t expected = kStateActive;
  if (!__atomic_compare_exchange_n(&g_state, &expected, kStateFinished, false,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return LANG_GC_PROGRAM_ERROR_STATE;

  int64_t status = LANG_GC_PROGRAM_SKIPPED_BUSY;
  // Direct collection would Panic from SAFE; the query also keeps an
  // outstanding collector from being disturbed. A normal main wrapper reaches
  // this point ACTIVE and outside both regions. The timeout bounds stop
  // acquisition, not arbitrary trace/clear/foreign-cleanup wall time.
  if (lang_gc_world_can_collect_v1())
    status =
        lang_gc_runtime_collect_v1(LANG_GC_PROGRAM_FINAL_COLLECT_NS, nullptr);

  // From this point the main thread may still run host atexit/destructor code,
  // but its world registration is about to be removed. Hooks on this exact tid
  // become no-ops; worker tids keep the live bookkeeping.
  __atomic_store_n(&g_main_finished, 1u, __ATOMIC_RELEASE);
  // Keep the enabled word and all hooks installed for the process lifetime.
  // A detached language thread may still be registered after main returns; its
  // ordinary final releases must be able to remove registry entries, and a
  // later poll/collection must never read freed nodes. Disabling here would
  // make program_unregister skip removal, creating a real stale-entry UAF.
  // This runtime therefore has no per-main disable point; process exit is the
  // stable shutdown boundary.
  // Remove the main thread's node even if it pre-existed init; a surviving
  // ACTIVE main node would otherwise prevent worker collection. init already
  // required the main thread to be outside SAFE/critical.
  (void)lang_gc_world_unregister_v1();
  return status;
}

extern "C" bool lang_gc_program_register_v1(void *object,
                                            const LangGcOps *ops) noexcept {
  if (!lang::native::gc_program::enabled())
    return false;
  return lang_gc_runtime_register_v1(object, ops) == LANG_GC_OK;
}

extern "C" void lang_gc_program_unregister_v1(void *object) noexcept {
  if (!lang::native::gc_program::enabled())
    return;
  (void)lang_gc_runtime_unregister_v1(object);
}

extern "C" int32_t lang_gc_program_enter_safe_v1(void) noexcept {
  return enter_safe_hook();
}

extern "C" void lang_gc_program_leave_safe_v1(int32_t entered) noexcept {
  leave_safe_hook(entered);
}

extern "C" int32_t lang_gc_program_enter_critical_v1(void) noexcept {
  return enter_critical_hook();
}

extern "C" void lang_gc_program_leave_critical_v1(int32_t entered) noexcept {
  leave_critical_hook(entered);
}

extern "C" int32_t lang_gc_program_thread_enter_v1(void) noexcept {
  return thread_enter_hook();
}

extern "C" int32_t lang_gc_program_callback_enter_v1(
    LangGcProgramCallbackToken *token) noexcept {
  if (!token || !lang::native::gc_program::enabled() || main_after_finish())
    return 0;
  // gc_world accepts a zero-initialized token, registers an absent foreign
  // thread only for this callback scope, saves the complete SAFE depth, and
  // returns only when the thread is ACTIVE graph work.
  lang_gc_world_callback_enter_v1(token);
  return 1;
}

extern "C" void lang_gc_program_callback_leave_v1(
    int32_t entered, LangGcProgramCallbackToken *token) noexcept {
  if (!entered)
    return;
  // Restore the exact saved SAFE depth/state and remove a registration created
  // by this callback's outermost scope. Existing registrations are retained.
  lang_gc_world_callback_leave_v1(token);
}

extern "C" void lang_gc_program_thread_leave_v1(int32_t registered) noexcept {
  thread_leave_hook(registered);
}

extern "C" void lang_gc_program_fork_enter_v1(
    LangGcProgramForkToken *token) noexcept {
  if (!token)
    program_panic("fork-null-token");
  if (token->magic == kForkTokenMagic)
    program_panic("fork-token-active");
  token->magic = kForkTokenMagic;
  token->active = 0;
  token->safe_depth = 0;
  token->caller_tid = raw_current_tid();
  token->main_caller =
      token->caller_tid != 0 &&
              token->caller_tid ==
                  __atomic_load_n(&g_main_tid, __ATOMIC_ACQUIRE)
          ? 1u
          : 0u;
  token->reserved = 0;

  if (!lang::native::gc_program::enabled() || main_after_finish())
    return;

  token->active = 1;
  // Suspend the complete outer SAFE depth before taking any runtime lock. If a
  // stop is pending, the world parks us here without holding launch/env/alloc.
  token->safe_depth = lang_gc_world_suspend_safe_v1();
  // ACTIVE callers with no SAFE depth must also honor an existing stop first.
  lang_gc_world_poll_v1();
  // A fork is graph-stable, not a partial graph modification. The dedicated
  // no-park interval prevents the collector from parking us while internal
  // fork locks are held, but keeps synchronous callbacks and SAFE legal. A
  // genuine nested critical region still rejects callbacks/SAFE normally.
  lang_gc_world_enter_fork_stable_v1();
}

extern "C" void lang_gc_program_fork_leave_v1(
    LangGcProgramForkToken *token) noexcept {
  if (!token || token->magic != kForkTokenMagic)
    program_panic("fork-token-invalid");
  const uint32_t active = token->active;
  const uint32_t safe_depth = token->safe_depth;
  const uint32_t main_caller = token->main_caller;
  token->magic = 0;
  token->active = 0;
  token->safe_depth = 0;
  token->main_caller = 0;
  token->caller_tid = 0;
  token->reserved = 0;

  if (main_caller) {
    // This same logical main thread may be the fork child with a new tid. Keep
    // post-finish main-hook detection accurate in that child.
    const int32_t now = raw_current_tid();
    if (now > 0 &&
        now != __atomic_load_n(&g_main_tid, __ATOMIC_ACQUIRE))
      __atomic_store_n(&g_main_tid, now, __ATOMIC_RELEASE);
  }
  if (!active)
    return;
  // Restore the outer SAFE depth while the fork no-park interval is still
  // active (it keeps the node ACTIVE for the collector), then leave no-park.
  // The outermost leave publishes SAFE/PARKED only after all fork locks have
  // been released by the caller.
  if (safe_depth)
    lang_gc_world_restore_safe_v1(safe_depth);
  lang_gc_world_leave_fork_stable_v1();
}
