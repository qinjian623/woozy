#include "thread.hpp"
#include "../native/alloc.hpp"
#include "../native/gc_program.hpp"
#include "../native/gc_runtime.hpp"
#include "../native/weak.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

extern "C" [[noreturn]] void lang_native_panic_v1(uint32_t, uint64_t, uint64_t,
                                                 uint64_t) noexcept;
#pragma weak lang_gc_runtime_defer_release_v1

#if defined(__x86_64__) && defined(__linux__)
namespace {
constexpr int64_t kDeadlock = -35; // EDEADLK
constexpr int64_t kTimedOut = -110;
constexpr int64_t kInvalidTimeout = -22;
constexpr int64_t kNanoseconds = 1000000000;
constexpr uint64_t kContextAlignment = 16;

struct alignas(16) HostedThreadState {
  LangHandle handle;
  pthread_t thread;
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  LangThreadEntry entry;
  LangValueOp context_drop;
  void *context;
  int64_t result;
  int64_t error; // spawn/copy failure, immutable after return
  uint32_t done;
  uint32_t created;
  uint32_t mutex_ready;
  uint32_t cond_ready;
  int32_t process_id;
  int32_t reserved;
};
static_assert(sizeof(LangHandle) == 32 && offsetof(LangHandle, refs) == 0 &&
              offsetof(LangHandle, kind) == 8 &&
              offsetof(LangHandle, destroy) == 16 &&
              offsetof(LangHandle, weak_control) == 24);

// Safe host TLS: each runner records its own state.  This is used only by the
// fork-child compatibility hook; it is not a replacement for the C library's
// thread pointer or TCB.
thread_local HostedThreadState *current_thread = nullptr;

void thread_atfork_child() noexcept { lang_thread_fork_child_v1(); }
__attribute__((constructor(101), used)) void register_thread_atfork_hook()
    noexcept {
  // A direct libc fork from a hosted runtime thread must also invalidate the
  // inherited process id, not only language process.fork calls.
  pthread_atfork(nullptr, nullptr, &thread_atfork_child);
}

void destroy_thread(LangHandle *handle) noexcept;

void validate_handle(const LangHandle *handle) noexcept {
  if (!handle || !handle->destroy || handle->kind < LangHandleKind::thread ||
      handle->kind > LangHandleKind::ipc)
    lang_handle_panic_v1();
}

HostedThreadState *thread(LangHandle *handle) noexcept {
  validate_handle(handle);
  if (handle->kind != LangHandleKind::thread || handle->destroy != destroy_thread)
    lang_handle_panic_v1();
  return reinterpret_cast<HostedThreadState *>(handle);
}

void *context(HostedThreadState *state) noexcept { return state->context; }

void drop_context(HostedThreadState *state) noexcept {
  if (state->context_drop) state->context_drop(context(state));
}

LangHandle *fail_spawn(HostedThreadState *state, int64_t error,
                       LangValueOp drop) noexcept {
  state->context_drop = drop;
  drop_context(state);
  __atomic_store_n(&state->error, error, __ATOMIC_RELEASE);
  __atomic_store_n(&state->done, 1u, __ATOMIC_RELEASE);
  // The caller still owns the returned handle; the failed runner reference is
  // dropped here, matching the native clone-failure contract.
  lang_handle_release_v1(&state->handle);
  return &state->handle;
}

void destroy_thread_now(void *payload) noexcept {
  auto *state = static_cast<HostedThreadState *>(payload);
  if (__atomic_load_n(&state->created, __ATOMIC_ACQUIRE)) {
    if (__atomic_load_n(&state->process_id, __ATOMIC_RELAXED) !=
        static_cast<int32_t>(getpid())) {
      // An inherited handle in a fork child has no live pthread to join.
    } else if (pthread_equal(pthread_self(), state->thread)) {
      // The last reference was dropped while the runner was finishing.  A
      // self-detach is required because the runner owns this final release.
      pthread_detach(state->thread);
    } else {
      // Reap the pthread's stack and libc TLS once the completion signal is
      // visible.  Joining also covers the normal "last handle released after
      // the thread completed but before any join" path.
      lang::native::gc_program::SafeGuard safe;
      pthread_join(state->thread, nullptr);
    }
  }
  if (state->cond_ready) pthread_cond_destroy(&state->cond);
  if (state->mutex_ready) pthread_mutex_destroy(&state->mutex);
  if (state->context) lang_free_v1(state->context);
  lang_free_v1(state);
}

void destroy_thread(LangHandle *handle) noexcept {
  // The runner drops the Woozy context before its own reference. At refs=0
  // only host state remains; joining must wait until the GC world resumes.
  if (lang_gc_runtime_defer_release_v1 &&
      lang_gc_runtime_defer_release_v1(handle, &destroy_thread_now))
    return;
  destroy_thread_now(handle);
}

void *run_thread(void *pointer) noexcept {
  auto *state = static_cast<HostedThreadState *>(pointer);
  current_thread = state;
  // Register the language thread before its entry can touch the graph. The
  // ThreadState context remains an external strong root; only the thread's
  // registry node is created here.
  lang::native::gc_program::ThreadGuard gc_thread;
  // LangThreadEntry and the drop boundary are no-throws by ABI.  This object
  // is compiled with -fno-exceptions; a foreign C++ throw is outside the ABI
  // and terminates at the boundary rather than being translated into a fake
  // language status.
  const int64_t result = state->entry(context(state));
  drop_context(state);
  // The context is no longer a root. Unregister before publishing completion.
  gc_thread.release();
  state->result = result;
  __atomic_store_n(&state->done, 1u, __ATOMIC_RELEASE);
  if (pthread_mutex_lock(&state->mutex) == 0) {
    pthread_cond_broadcast(&state->cond);
    pthread_mutex_unlock(&state->mutex);
  }
  current_thread = nullptr;
  // This may destroy the state (and detach this pthread) if the user already
  // released the final handle.
  lang_handle_release_v1(&state->handle);
  return nullptr;
}
} // namespace

extern "C" [[noreturn]] void lang_handle_panic_v1() noexcept {
  lang_native_panic_v1(10, 0, 0, 0);
}
extern "C" void lang_handle_retain_v1(LangHandle *handle) noexcept {
  validate_handle(handle);
  uint64_t refs = __atomic_load_n(&handle->refs, __ATOMIC_RELAXED);
  for (;;) {
    if (!refs || refs == ~uint64_t{0}) lang_handle_panic_v1();
    if (__atomic_compare_exchange_n(&handle->refs, &refs, refs + 1, false,
                                    __ATOMIC_RELAXED, __ATOMIC_RELAXED))
      return;
  }
}
extern "C" void lang_handle_release_v1(LangHandle *handle) noexcept {
  validate_handle(handle);
  uint64_t refs = __atomic_load_n(&handle->refs, __ATOMIC_RELAXED);
  for (;;) {
    if (!refs) lang_handle_panic_v1();
    if (__atomic_compare_exchange_n(&handle->refs, &refs, refs - 1, false,
                                    __ATOMIC_RELEASE, __ATOMIC_RELAXED))
      break;
  }
  if (refs == 1) {
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    // The last release can be the caller's or the runner's. Detach before the
    // hosted destroy joins/detaches the pthread and frees the state.
    lang_weak_detach_v1(handle->weak_control);
    handle->destroy(handle);
  }
}

extern "C" LangHandle *lang_thread_spawn_v1(LangThreadEntry entry,
                                            const void *argument, uint64_t size,
                                            LangValueOp copy,
                                            LangValueOp drop) noexcept {
  if (!entry || (size && !argument)) lang_handle_panic_v1();
  if (size > ~uint64_t{0} - sizeof(HostedThreadState) - 4096)
    lang_native_panic_v1(6, 0, 0, 0);

  auto *state = static_cast<HostedThreadState *>(
      lang_alloc_v1(sizeof(HostedThreadState), alignof(HostedThreadState)));
  if (!state) lang_native_panic_v1(5, 0, 0, 0);
  new (state) HostedThreadState{};
  state->handle = {2, LangHandleKind::thread, destroy_thread, nullptr};
  state->entry = entry;
  state->context_drop = drop;
  state->process_id = static_cast<int32_t>(getpid());

  if (size) {
    state->context = lang_alloc_v1(size, kContextAlignment);
    if (!state->context) lang_native_panic_v1(5, 0, 0, 0);
    auto *destination = static_cast<volatile unsigned char *>(state->context);
    const auto *source = static_cast<const volatile unsigned char *>(argument);
    for (uint64_t i = 0; i < size; ++i) destination[i] = source[i];
  }
  if (copy) copy(state->context);

  int status = pthread_mutex_init(&state->mutex, nullptr);
  if (status) return fail_spawn(state, -static_cast<int64_t>(status), drop);
  state->mutex_ready = 1;
  pthread_condattr_t condition_attributes;
  status = pthread_condattr_init(&condition_attributes);
  if (status) return fail_spawn(state, -static_cast<int64_t>(status), drop);
  status = pthread_condattr_setclock(&condition_attributes, CLOCK_MONOTONIC);
  if (!status) status = pthread_cond_init(&state->cond, &condition_attributes);
  pthread_condattr_destroy(&condition_attributes);
  if (status) return fail_spawn(state, -static_cast<int64_t>(status), drop);
  state->cond_ready = 1;

  pthread_attr_t attributes;
  status = pthread_attr_init(&attributes);
  if (status) return fail_spawn(state, -static_cast<int64_t>(status), drop);
  status = pthread_attr_setstacksize(&attributes, LANG_THREAD_STACK_BYTES);
  if (!status) pthread_attr_setguardsize(&attributes, 4096);
  if (!status)
    status = pthread_create(&state->thread, &attributes, &run_thread, state);
  pthread_attr_destroy(&attributes);
  if (status) return fail_spawn(state, -static_cast<int64_t>(status), drop);
  __atomic_store_n(&state->created, 1u, __ATOMIC_RELEASE);
  return &state->handle;
}

namespace {
int64_t terminal_status(HostedThreadState *state) noexcept {
  return __atomic_load_n(&state->error, __ATOMIC_ACQUIRE);
}

int64_t make_deadline(int64_t timeout_ns, timespec &deadline) noexcept {
  if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0)
    return -static_cast<int64_t>(errno);
  if (deadline.tv_sec < 0 || deadline.tv_nsec < 0 ||
      deadline.tv_nsec >= kNanoseconds)
    return kInvalidTimeout;
  constexpr auto maximum = std::numeric_limits<time_t>::max();
  const auto seconds = timeout_ns / kNanoseconds;
  const auto nanoseconds = timeout_ns % kNanoseconds;
  if (seconds > maximum - deadline.tv_sec) {
    deadline = {maximum, kNanoseconds - 1};
    return 0;
  }
  deadline.tv_sec += seconds;
  deadline.tv_nsec += nanoseconds;
  if (deadline.tv_nsec >= kNanoseconds) {
    if (deadline.tv_sec == maximum)
      deadline.tv_nsec = kNanoseconds - 1;
    else {
      ++deadline.tv_sec;
      deadline.tv_nsec -= kNanoseconds;
    }
  }
  return 0;
}

int64_t deadline_status(const timespec &deadline) noexcept {
  timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return -static_cast<int64_t>(errno);
  return now.tv_sec > deadline.tv_sec ||
                 (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)
             ? kTimedOut
             : 0;
}

int64_t wait_thread(LangHandle *handle, int64_t timeout_ns) noexcept {
  if (timeout_ns < -1) return kInvalidTimeout;
  auto *state = thread(handle);
  if (__atomic_load_n(&state->done, __ATOMIC_ACQUIRE))
    return terminal_status(state);
  // An inherited pthread may have left its mutex locked in a vanished thread.
  if (__atomic_load_n(&state->process_id, __ATOMIC_RELAXED) !=
      static_cast<int32_t>(getpid()))
    return -3; // ESRCH
  if (pthread_equal(pthread_self(), state->thread)) return kDeadlock;
  if (timeout_ns == 0) return kTimedOut;

  timespec deadline{};
  if (timeout_ns > 0) {
    const int64_t status = make_deadline(timeout_ns, deadline);
    if (status) return status;
  }
  int status = pthread_mutex_lock(&state->mutex);
  if (status) return -static_cast<int64_t>(status);
  // SAFE spans the predicate/condvar loop, but leave only after the handle
  // mutex is released. pthread_cond_wait releases that mutex while sleeping;
  // the remaining predicate windows are short and no allocator/environment/
  // launch lock is involved. This cannot park while holding the handle mutex.
  lang::native::gc_program::SafeGuard safe;
  int64_t result = 0;
  while (!__atomic_load_n(&state->done, __ATOMIC_ACQUIRE)) {
    if (timeout_ns > 0) {
      result = deadline_status(deadline);
      if (result) {
        if (result == kTimedOut &&
            __atomic_load_n(&state->done, __ATOMIC_ACQUIRE))
          result = 0;
        break;
      }
      // Keep the same absolute monotonic deadline after every wakeup; the
      // clock check bounds a stream of spurious successes.
      status = pthread_cond_timedwait(&state->cond, &state->mutex, &deadline);
    } else {
      status = pthread_cond_wait(&state->cond, &state->mutex);
    }
    if (status && status != EINTR && status != ETIMEDOUT) {
      result = -static_cast<int64_t>(status);
      break;
    }
  }
  pthread_mutex_unlock(&state->mutex);
  safe.release();
  return result ? result : terminal_status(state);
}
} // namespace

extern "C" int64_t lang_thread_error_v1(LangHandle *handle) noexcept {
  return __atomic_load_n(&thread(handle)->error, __ATOMIC_ACQUIRE);
}

extern "C" int64_t lang_thread_wait_v1(LangHandle *handle) noexcept {
  return wait_thread(handle, -1);
}

extern "C" int64_t lang_thread_wait_for_v1(LangHandle *handle,
                                           int64_t timeout_ns) noexcept {
  return wait_thread(handle, timeout_ns);
}

extern "C" int64_t lang_thread_result_v1(LangHandle *handle) noexcept {
  auto *state = thread(handle);
  if (__atomic_load_n(&state->error, __ATOMIC_ACQUIRE) ||
      !__atomic_load_n(&state->done, __ATOMIC_ACQUIRE))
    lang_handle_panic_v1();
  return state->result;
}

extern "C" void lang_thread_fork_child_v1() noexcept {
  auto *state = current_thread;
  if (state)
    __atomic_store_n(&state->process_id, static_cast<int32_t>(getpid()),
                     __ATOMIC_RELAXED);
}

// A raw pthread stack cannot be unmapped by the thread using it, and host TLS
// is owned by libc.  This native-only epilogue is therefore unsupported in the
// hosted runtime; fail closed instead of corrupting the pthread stack.
extern "C" [[noreturn]] void lang_thread_unmap_exit_v1(void *, uint64_t) noexcept {
  lang_handle_panic_v1();
}

#endif
