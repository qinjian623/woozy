// Private hosted implementation of the capturing C registration ABI declared
// in c_callback.hpp and specified by
// development/docs/C_RETAINED_CAPTURING_CALLBACKS.md.
//
// This translation unit is part of the explicitly selected hosted runtime: it
// uses pthreads and libc (getpid/gettid) and must be compiled with the hosted
// replacement flags (no -ffreestanding, -pthread, -fno-exceptions, -fno-rtti).
// It introduces no C++ exception, RTTI or <functional>/<memory> dependency.
//
// Ownership summary:
//
//   new     allocates the private context ACTIVE, copies the immutable
//           {code, env} pair and retains env as one external root.
//   finish  publishes the foreign registration token, or consumes a failed
//           registration exactly once.
//   enter   is the C trampoline/bridge entry: it counts one in-flight callback
//           and yields the immutable Fn. The bridge owns the 32-byte GC
//           callback scope; this layer only does lifecycle/TLS bookkeeping.
//   borrow  leases the raw foreign token for one whole C call.
//   close   is the explicit unregister operation: it rejects unrelated new
//           leases, drains prior leases, calls the quiescent unregister while
//           SAFE and without any context lock, waits for callbacks, and then
//           detaches/drops the env ACTIVE.
//   release is the final Source owner drop: it first offers the still-retained
//           external root to the GC controller's deferred-release queue and
//           otherwise performs the same close and frees the context.
//
// Every close/release participant is pinned on the context (`pins`) so a
// close/drop re-entry from an env destructor or a concurrent closer cannot
// free the storage under another participant. `free_armed` is only set by the
// release path, so an explicit close leaves the context for the final owner
// release while "explicit close is idempotent" still holds.
//
// GC scopes: the leader teardown runs inside one existing GC cleanup scope (the
// language callback scope), which registers an absent foreign thread, makes the
// thread ACTIVE for the env drop, and restores the caller's SAFE depth on leave.
// The foreign unregister and every condition wait nest an ordinary SAFE scope,
// and no context mutex is held when that SAFE scope is left. Followers that
// wait for another closer's teardown wait SAFE as well. TLS entries (callback
// entries and foreign-call leases) form one per-thread LIFO stack: a nonempty
// token must be the current top before it is popped.

#include "c_callback.hpp"

#include "../native/gc_program.hpp"
#include "../native/gc_runtime.hpp"
#include "../native/vec.hpp"

#include <cstddef>
#include <cstdint>
#include <pthread.h>
#ifdef __APPLE__
#include "../native/darwin.hpp"
#else
#include <sys/syscall.h>
#endif
#include <unistd.h>

extern "C" [[noreturn]] void lang_native_panic_v1(uint32_t, uint64_t, uint64_t,
                                                  uint64_t) noexcept;
// The hosted archive always contains gc_runtime, but a hand-built unit test
// may not. A missing controller simply means "no deferred collection window".
#pragma weak lang_gc_runtime_defer_release_v1

namespace {

// Shared terminal-Panic causes from runtime/native/panic.cpp. The private
// site marker is carried in the snapshot word so the record distinguishes
// inherited use, self-wait, closed use and synchronization failures without
// changing the shared Panic ABI.
constexpr uint32_t kPanicHandle = 10; // "runtime-handle-misuse"
constexpr uint32_t kPanicAlloc = 5;   // "allocation-failed"

constexpr uint64_t kMarkerInherited = 1;
constexpr uint64_t kMarkerSelfWait = 2;
constexpr uint64_t kMarkerClosed = 3;
constexpr uint64_t kMarkerSync = 4;
constexpr uint64_t kMarkerReentry = 5;
constexpr uint64_t kMarkerInvalid = 6;
constexpr uint64_t kMarkerCorrupt = 7;
constexpr uint64_t kMarkerTokenOrder = 8;

[[noreturn]] void misuse(uint64_t marker) noexcept {
  lang_native_panic_v1(kPanicHandle, marker, 0, 0);
}

constexpr uint32_t kInitial = 0;
constexpr uint32_t kActive = 1;
constexpr uint32_t kClosing = 2;
constexpr uint32_t kDone = 3;

struct Context {
  // Immutable once `ready` is published. The C library may synchronously
  // invoke the trampoline with this context as userdata as soon as the
  // registration call sees it, including from another thread later.
  LangCCallbackFn fn;
  void (*unregister)(void *) noexcept;
  LangVec *env; // the one retained external root
  int32_t pid;  // creating process id
  uint32_t ready;

  // Short host synchronization. Every lock is taken only for counter/state
  // updates; condvar waits release it and no lock is held over a foreign call
  // or over the env drop.
  pthread_mutex_t mutex;
  pthread_cond_t cond;

  void *foreign;        // raw C registration token, set by finish
  uint64_t callbacks;   // in-flight enter/leave pairs
  uint64_t leases;      // in-flight borrow/end_borrow pairs
  uint64_t pins;        // close/release participants
  uint32_t phase;
  uint32_t accepting;   // callbacks admitted (cleared after unregister)
  uint32_t free_armed;  // a release participant wants the storage freed
  uint32_t detached;    // env ownership already moved out of the context
  int64_t teardown_tid; // close/release leader identity
};

// One TLS stack shared by callback entries and foreign-call leases. Every
// element is the caller's own 16-byte LangCCallbackToken; the runtime keeps no
// reference after the matching leave/end_borrow. A POD thread_local has no TLS
// destructor and therefore no special teardown path.
thread_local LangCCallbackToken *tls_top = nullptr;

int64_t current_tid() noexcept {
#ifdef __APPLE__
  const auto result = lang::native::darwin::thread_id();
  if (result <= 0) misuse(kMarkerSync);
  return result;
#else
  return static_cast<int32_t>(::syscall(SYS_gettid));
#endif
}

int32_t current_pid() noexcept {
  return static_cast<int32_t>(::getpid());
}

void push_token(LangCCallbackToken *token, void *context) noexcept {
  token->context = context;
  token->previous = tls_top;
  tls_top = token;
}

void pop_token(LangCCallbackToken *token) noexcept {
  // The TLS stack is LIFO: a nonempty token must be the current top before it
  // is popped. Leaving an outer callback/lease while a nested one is still
  // active is a terminal misuse, not something to silently repair.
  if (tls_top != token)
    misuse(kMarkerTokenOrder);
  tls_top = static_cast<LangCCallbackToken *>(token->previous);
  token->context = nullptr;
  token->previous = nullptr;
}

// True when this thread already has a callback or lease on `context`. Used to
// allow a nested C call from an existing callback/lease while close is pending
// and to reject self-waits before blocking.
bool thread_holds(const Context *context) noexcept {
  for (auto *token = tls_top; token != nullptr;
       token = static_cast<LangCCallbackToken *>(token->previous)) {
    if (token->context == context)
      return true;
  }
  return false;
}

void lock(Context *context) noexcept {
  if (pthread_mutex_lock(&context->mutex) != 0)
    misuse(kMarkerSync);
}

void unlock(Context *context) noexcept {
  if (pthread_mutex_unlock(&context->mutex) != 0)
    misuse(kMarkerSync);
}

void wait_locked(Context *context) noexcept {
  if (pthread_cond_wait(&context->cond, &context->mutex) != 0)
    misuse(kMarkerSync);
}

void broadcast_locked(Context *context) noexcept {
  if (pthread_cond_broadcast(&context->cond) != 0)
    misuse(kMarkerSync);
}

void destroy_sync(Context *context) noexcept {
  (void)pthread_cond_destroy(&context->cond);
  (void)pthread_mutex_destroy(&context->mutex);
}

// The context fields written here are all published before `ready` becomes 1
// with release ordering; enter reads `ready` with acquire before it exposes the
// Fn, so no callback can observe a half-initialized pair.
Context *context_new() noexcept {
  auto *context =
      static_cast<Context *>(lang_alloc_v1(sizeof(Context), alignof(Context)));
  if (!context)
    lang_native_panic_v1(kPanicAlloc, 0, 0, 0);
  context->fn.code = nullptr;
  context->fn.env = nullptr;
  context->unregister = nullptr;
  context->env = nullptr;
  context->pid = 0;
  context->ready = 0;
  context->foreign = nullptr;
  context->callbacks = 0;
  context->leases = 0;
  context->pins = 0;
  context->phase = kInitial;
  context->accepting = 0;
  context->free_armed = 0;
  context->detached = 0;
  context->teardown_tid = 0;
  return context;
}

void context_destroy(Context *context) noexcept {
  destroy_sync(context);
  lang_free_v1(context);
}

void pin(Context *context) noexcept {
  lock(context);
  ++context->pins;
  unlock(context);
}

// Drop one close/release participant. Only the participant that observes zero
// pinned users of an already-torn-down context destroys the synchronization
// objects and frees the storage, so concurrent closers never touch freed
// memory after their own decrement.
void unpin(Context *context) noexcept {
  lock(context);
  const uint64_t remaining = --context->pins;
  const bool free_now =
      remaining == 0 && context->free_armed != 0 && context->phase == kDone;
  unlock(context);
  if (free_now)
    context_destroy(context);
}

// Existing GC cleanup scope: it registers an absent foreign thread for the
// scope, saves the caller's complete SAFE depth, and exposes ACTIVE graph work.
// Leaving restores the exact prior SAFE state (or removes the created
// registration). It is a no-op while the program layer is disabled or already
// finished for the main thread.
class CleanupScope {
 public:
  CleanupScope() noexcept {
    token_ = LangGcProgramCallbackToken{};
    entered_ = lang_gc_program_callback_enter_v1(&token_);
  }
  ~CleanupScope() noexcept {
    if (entered_)
      lang_gc_program_callback_leave_v1(entered_, &token_);
  }
  CleanupScope(const CleanupScope &) = delete;
  CleanupScope &operator=(const CleanupScope &) = delete;

 private:
  LangGcProgramCallbackToken token_{};
  int32_t entered_ = 0;
};

// Wait until no foreign-call lease is in flight. New unrelated leases are
// already rejected because the phase is `kClosing`; nested leases taken from an
// existing callback/lease on this context may complete normally. Always called
// SAFE and without a context lock.
void drain_leases(Context *context) noexcept {
  lock(context);
  while (context->leases != 0)
    wait_locked(context);
  unlock(context);
}

// Wait for another thread's teardown. The wait is SAFE so a concurrent
// collection does not have to wait for this thread, and every wake-up drops the
// context mutex before the SAFE scope is left.
void wait_for_teardown(Context *context) noexcept {
  // Cheap path without a GC scope when the leader already finished.
  lock(context);
  const bool done = context->phase == kDone;
  unlock(context);
  if (done)
    return;
  CleanupScope scope;
  lang::native::gc_program::SafeGuard safe;
  lock(context);
  while (context->phase != kDone)
    wait_locked(context);
  unlock(context);
}

// Leader teardown. Runs with no context lock held.
//
// The whole sequence runs inside one existing GC cleanup scope: the caller's
// SAFE depth (for example the generated close adapter being invoked inside a
// C-call SAFE block) is saved and the thread becomes ACTIVE, so the env drop and
// its destructors are ACTIVE graph work. The foreign unregister and the
// lease/callback waits nest their own SAFE scope and leave it, and every context
// mutex is released before SAFE is left. The cleanup scope restores the prior
// SAFE state afterwards.
void quiesce_and_detach(Context *context) noexcept {
  LangVec *env = nullptr;
  {
    CleanupScope scope;
    {
      lang::native::gc_program::SafeGuard safe;
      drain_leases(context);
      // Quiescent foreign stop: the C library guarantees that once this returns
      // no future or in-flight callback remains, so it is called with no context
      // lock and before callbacks are refused.
      if (context->foreign)
        context->unregister(context->foreign);
      lock(context);
      context->accepting = 0;
      while (context->callbacks != 0)
        wait_locked(context);
      env = context->env;
      context->env = nullptr;
      context->detached = 1;
      unlock(context);
    }
    // ACTIVE, outside every host lock: the env root and its destructors.
    if (env)
      lang_vec_release_v1(env);
  }
}

// Shared close/release body. `release` is the Source owner drop and may defer;
// `close` is the explicit unregister operation.
void close_or_release(void *handle, bool release) noexcept {
  if (!handle)
    return;
  auto *context = static_cast<Context *>(handle);
  // PID checks always precede every context lock and foreign call.
  if (context->pid != current_pid()) {
    // Inherited foreign registrations are abandoned: the implicit release is a
    // total no-op (no reference, env, token or unregister work).
    if (release)
      return;
    misuse(kMarkerInherited);
  }
  if (!__atomic_load_n(&context->ready, __ATOMIC_ACQUIRE))
    misuse(kMarkerInvalid);
  // Closing one's own callback/lease would wait on itself.
  if (thread_holds(context))
    misuse(kMarkerSelfWait);
  if (release) {
    // The retained env is already an external root: the collector keeps it (and
    // its graph) intact while stopped and runs this release again after resume.
    // No callback/cleanup scope is entered before this attempt.
    if (lang_gc_runtime_defer_release_v1 &&
        lang_gc_runtime_defer_release_v1(handle,
                                        &lang_c_registration_release_v1))
      return;
  }
  pin(context);
  const int64_t tid = current_tid();
  lock(context);
  if (context->phase == kDone) {
    if (release)
      context->free_armed = 1;
    unlock(context);
    unpin(context);
    return;
  }
  if (context->phase == kClosing) {
    if (context->teardown_tid == tid) {
      // Same-thread re-entry. After the env ownership is detached the destructor
      // re-entry is allowed to do nothing; before that it would wait on its own
      // foreign stop. It never arms the final free: the outer participant owns
      // that decision, so an explicit close stays idempotent afterwards.
      if (context->detached == 0) {
        unlock(context);
        misuse(kMarkerReentry);
      }
      unlock(context);
      unpin(context);
      return;
    }
    // Another closer owns the teardown; wait SAFE for it to complete.
    unlock(context);
    wait_for_teardown(context);
    lock(context);
    if (release)
      context->free_armed = 1;
    unlock(context);
    unpin(context);
    return;
  }
  if (context->phase != kActive || context->foreign == nullptr) {
    unlock(context);
    misuse(kMarkerInvalid);
  }
  context->phase = kClosing;
  context->teardown_tid = tid;
  unlock(context);
  quiesce_and_detach(context);
  lock(context);
  context->phase = kDone;
  if (release)
    context->free_armed = 1;
  broadcast_locked(context);
  unlock(context);
  unpin(context);
}

} // namespace

extern "C" void *lang_c_registration_new_v1(
    void *code, void *env, void (*unregister)(void *) noexcept) noexcept {
  if (code == nullptr || unregister == nullptr)
    misuse(kMarkerInvalid);
  Context *context = context_new();
  // The retained env is the single external root. `env` is a LangVec closure
  // environment; a noncapturing Fn value has a null env and is not retained.
  if (env != nullptr)
    lang_vec_retain_v1(static_cast<LangVec *>(env));
  context->fn.code = code;
  context->fn.env = env;
  context->unregister = unregister;
  context->env = static_cast<LangVec *>(env);
  context->pid = current_pid();
  if (pthread_mutex_init(&context->mutex, nullptr) != 0) {
    if (context->env)
      lang_vec_release_v1(context->env);
    lang_free_v1(context);
    misuse(kMarkerSync);
  }
  if (pthread_cond_init(&context->cond, nullptr) != 0) {
    (void)pthread_mutex_destroy(&context->mutex);
    if (context->env)
      lang_vec_release_v1(context->env);
    lang_free_v1(context);
    misuse(kMarkerSync);
  }
  context->phase = kActive;
  context->accepting = 1;
  // Publish the fully initialized immutable Fn/code/env/pid fields.
  __atomic_store_n(&context->ready, 1u, __ATOMIC_RELEASE);
  return context;
}

extern "C" void *lang_c_registration_finish_v1(void *handle,
                                               void *foreign_token) noexcept {
  if (handle == nullptr)
    misuse(kMarkerInvalid);
  auto *context = static_cast<Context *>(handle);
  if (context->pid != current_pid())
    misuse(kMarkerInherited);
  if (!__atomic_load_n(&context->ready, __ATOMIC_ACQUIRE))
    misuse(kMarkerInvalid);
  if (foreign_token == nullptr) {
    // Failed registration: the C API must already have stopped and drained any
    // failed registration. Consume the context and drop its env exactly once.
    lock(context);
    if (context->phase != kActive || context->callbacks != 0 ||
        context->leases != 0 || context->pins != 0) {
      unlock(context);
      misuse(kMarkerCorrupt);
    }
    context->phase = kDone;
    context->accepting = 0;
    context->detached = 1;
    LangVec *env = context->env;
    context->env = nullptr;
    unlock(context);
    if (env)
      lang_vec_release_v1(env);
    context_destroy(context);
    return nullptr;
  }
  lock(context);
  if (context->phase != kActive || context->foreign != nullptr) {
    unlock(context);
    misuse(kMarkerInvalid);
  }
  context->foreign = foreign_token;
  unlock(context);
  return context;
}

extern "C" void lang_c_registration_close_v1(void *handle) noexcept {
  close_or_release(handle, false);
}

extern "C" void lang_c_registration_release_v1(void *handle) noexcept {
  close_or_release(handle, true);
}

extern "C" const LangCCallbackFn *lang_c_registration_enter_v1(
    void *handle, LangCCallbackToken *token) noexcept {
  if (token == nullptr)
    misuse(kMarkerInvalid);
  token->context = nullptr;
  token->previous = nullptr;
  if (handle == nullptr)
    misuse(kMarkerClosed);
  auto *context = static_cast<Context *>(handle);
  if (context->pid != current_pid())
    misuse(kMarkerInherited);
  if (!__atomic_load_n(&context->ready, __ATOMIC_ACQUIRE))
    misuse(kMarkerInvalid);
  lock(context);
  // Callbacks stay admitted while the foreign unregister runs (the C library
  // may still dispatch). After the quiescent stop the registration is closed:
  // a late or null entry is a terminal misuse, never a skipped callback.
  if (context->phase == kInitial || context->phase == kDone ||
      context->accepting == 0) {
    unlock(context);
    misuse(kMarkerClosed);
  }
  ++context->callbacks;
  unlock(context);
  push_token(token, context);
  return &context->fn;
}

extern "C" void lang_c_registration_leave_v1(LangCCallbackToken *token) noexcept {
  if (token == nullptr || token->context == nullptr)
    return;
  auto *context = static_cast<Context *>(token->context);
  // The TLS stack unwinds first: no Wz value is accessed after this point.
  pop_token(token);
  if (context->pid != current_pid())
    return; // inherited: restore TLS only, no counters and no locks
  lock(context);
  if (context->callbacks == 0) {
    unlock(context);
    misuse(kMarkerCorrupt);
  }
  --context->callbacks;
  if (context->callbacks == 0)
    broadcast_locked(context);
  unlock(context);
}

extern "C" void *lang_c_registration_borrow_v1(
    void *handle, LangCCallbackToken *token) noexcept {
  if (token == nullptr)
    misuse(kMarkerInvalid);
  token->context = nullptr;
  token->previous = nullptr;
  if (handle == nullptr)
    return nullptr;
  auto *context = static_cast<Context *>(handle);
  if (context->pid != current_pid())
    misuse(kMarkerInherited);
  if (!__atomic_load_n(&context->ready, __ATOMIC_ACQUIRE))
    misuse(kMarkerInvalid);
  const bool nested = thread_holds(context);
  lock(context);
  // A closed/unregistered resource cannot be borrowed. While close is pending
  // an unrelated new lease is rejected, but a nested call made from an
  // existing callback/lease on this same context may still borrow.
  if (context->phase == kInitial || context->phase == kDone ||
      context->foreign == nullptr) {
    unlock(context);
    misuse(kMarkerClosed);
  }
  if (context->phase == kClosing && !nested) {
    unlock(context);
    misuse(kMarkerClosed);
  }
  void *foreign = context->foreign;
  ++context->leases;
  unlock(context);
  push_token(token, context);
  // The raw C token, valid for the whole leased call.
  return foreign;
}

extern "C" void lang_c_registration_end_borrow_v1(
    LangCCallbackToken *token) noexcept {
  if (token == nullptr || token->context == nullptr)
    return;
  auto *context = static_cast<Context *>(token->context);
  pop_token(token);
  if (context->pid != current_pid())
    return; // inherited: restore TLS only, no counters and no locks
  lock(context);
  if (context->leases == 0) {
    unlock(context);
    misuse(kMarkerCorrupt);
  }
  --context->leases;
  if (context->leases == 0)
    broadcast_locked(context);
  unlock(context);
}
