// C++ runtime harness for the private capturing C registration ABI
// (runtime/hosted/c_callback.hpp).
//
// The controlled fixture in c_callback_fixture.hpp models one ordinary C
// registration API (register/unregister/invoke) and the generated typed bridge
// shape. No source compiler, executable trampoline or special TLS teardown is
// involved.

#include "c_callback_fixture.hpp"

#include "alloc.hpp"
#include "gc.hpp"
#include "gc_program.hpp"
#include "gc_runtime.hpp"
#include "gc_world.hpp"
#include "vec.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {

using namespace callback_fixture;

#define CHECK C_CALLBACK_CHECK

std::atomic<int64_t> g_deferred_releases{0};
std::atomic<int64_t> g_reentry_calls{0};
std::atomic<int64_t> g_active_env_drops{0};

// Concurrent closer / collection fixture state (no captures, so the hooks stay
// plain function pointers).
std::atomic<bool> g_block_stop_entered{false};
std::atomic<bool> g_block_stop_resume{false};
std::atomic<bool> g_leader_done{false};
std::atomic<bool> g_follower_done{false};
std::atomic<int32_t> g_follower_tid{0};
std::atomic<bool> g_follower_waiting{false};
void *g_thread_context = nullptr;

// --wrap=pthread_cond_wait: proves the follower's condition wait is reached
// while the runtime holds it SAFE (the wrapper is entered before the wait).
extern "C" int __real_pthread_cond_wait(pthread_cond_t *, pthread_mutex_t *);
extern "C" int __wrap_pthread_cond_wait(pthread_cond_t *condition,
                                       pthread_mutex_t *mutex) {
  if (static_cast<int32_t>(::syscall(SYS_gettid)) ==
      g_follower_tid.load(std::memory_order_acquire))
    g_follower_waiting.store(true, std::memory_order_release);
  return __real_pthread_cond_wait(condition, mutex);
}

// Ordinary GC-state assertion used by the outer-SAFE tests: dropping the env
// root is graph work and must run ACTIVE, never inside a SAFE region.
void require_active_env_drop() {
  lang_gc_world_enter_critical_v1();
  g_active_env_drops.fetch_add(1, std::memory_order_relaxed);
  lang_gc_world_leave_critical_v1();
}

// ---------------------------------------------------------------------------
// Synchronous registration callback, foreign-thread callbacks, idempotent
// explicit close and exactly-once env drop / unregister.

void test_synchronous_and_foreign_threads() {
  reset_counters();
  LangVec *env = make_env(1000);
  Registration registration = register_captured(env);

  CHECK(dispatch(registration.context, 1));
  CHECK(g_calls.load() == 1);
  CHECK(g_last.load() == 1001);

  std::vector<std::thread> threads;
  threads.reserve(4);
  for (int index = 0; index < 4; ++index) {
    threads.emplace_back([context = registration.context] {
      lang::native::gc_program::ThreadGuard guard;
      for (int round = 0; round < 200; ++round)
        CHECK(dispatch(context, 2));
    });
  }
  for (auto &thread : threads)
    thread.join();
  CHECK(g_calls.load() == 1 + 4 * 200);
  CHECK(g_last.load() == 1002);

  lang_c_registration_close_v1(registration.context);
  CHECK(g_unregister_calls.load() == 1);
  CHECK(g_bad_token.load() == 0);
  CHECK(g_env_element_drops.load() == 1);
  CHECK(g_env_drop_busy.load() == 0);
  CHECK(g_unregister_busy.load() == 0);

  // Explicit close is idempotent while the Source owner still holds the
  // resource; the final owner release frees the private context.
  lang_c_registration_close_v1(registration.context);
  CHECK(g_unregister_calls.load() == 1);
  CHECK(g_env_element_drops.load() == 1);
  lang_c_registration_release_v1(registration.context);
  CHECK(g_unregister_calls.load() == 1);
  CHECK(g_env_element_drops.load() == 1);
  std::printf("C_CALLBACK_SYNC_FOREIGN_OK\n");
}

// ---------------------------------------------------------------------------
// Close must block on a real active callback: the foreign unregister already
// ran, but the env root is not dropped until the callback leaves.

void test_close_waits_for_active_callback() {
  reset_counters();
  LangVec *env = make_env(0);
  Registration registration = register_captured(env);
  std::atomic<bool> entered{false};
  std::atomic<bool> release_callback{false};
  std::atomic<bool> close_done{false};

  std::thread callback([&] {
    lang::native::gc_program::ThreadGuard guard;
    LangGcProgramCallbackToken gc_token{};
    const int32_t gc_entered = lang_gc_program_callback_enter_v1(&gc_token);
    LangCCallbackToken token{};
    const LangCCallbackFn *fn =
        lang_c_registration_enter_v1(registration.context, &token);
    CHECK(fn != nullptr);
    entered.store(true, std::memory_order_release);
    while (!release_callback.load(std::memory_order_acquire))
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    lang_gc_program_callback_leave_v1(gc_entered, &gc_token);
    lang_c_registration_leave_v1(&token);
  });
  CHECK(wait_for(entered));

  std::thread closer([&] {
    lang::native::gc_program::ThreadGuard guard;
    lang_c_registration_close_v1(registration.context);
    close_done.store(true, std::memory_order_release);
  });

  while (g_unregister_calls.load(std::memory_order_acquire) == 0)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  CHECK(!close_done.load(std::memory_order_acquire));
  CHECK(g_env_element_drops.load() == 0);

  release_callback.store(true, std::memory_order_release);
  callback.join();
  closer.join();
  CHECK(close_done.load(std::memory_order_acquire));
  CHECK(g_unregister_calls.load() == 1);
  CHECK(g_env_element_drops.load() == 1);
  CHECK(g_env_drop_busy.load() == 0);
  lang_c_registration_release_v1(registration.context);
  CHECK(g_env_element_drops.load() == 1);
  std::printf("C_CALLBACK_CLOSE_CALLBACK_WAIT_OK\n");
}

// ---------------------------------------------------------------------------
// Close must block on a foreign-call lease *before* the quiescent unregister.

void test_close_waits_for_lease() {
  reset_counters();
  LangVec *env = make_env(0);
  Registration registration = register_captured(env);
  std::atomic<bool> leased{false};
  std::atomic<bool> release_lease{false};
  std::atomic<bool> close_done{false};
  std::atomic<void *> borrowed{nullptr};

  std::thread caller([&] {
    LangCCallbackToken token{};
    borrowed.store(
        lang_c_registration_borrow_v1(registration.context, &token),
        std::memory_order_release);
    leased.store(true, std::memory_order_release);
    while (!release_lease.load(std::memory_order_acquire))
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    lang_c_registration_end_borrow_v1(&token);
  });
  CHECK(wait_for(leased));
  CHECK(borrowed.load(std::memory_order_acquire) == registration.token);

  std::thread closer([&] {
    lang::native::gc_program::ThreadGuard guard;
    lang_c_registration_close_v1(registration.context);
    close_done.store(true, std::memory_order_release);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK(!close_done.load(std::memory_order_acquire));
  // Leases drain before the foreign stop, so no unregister happened while a C
  // call could still use the registration.
  CHECK(g_unregister_calls.load() == 0);

  release_lease.store(true, std::memory_order_release);
  caller.join();
  closer.join();
  CHECK(close_done.load(std::memory_order_acquire));
  CHECK(g_unregister_calls.load() == 1);
  CHECK(g_env_element_drops.load() == 1);
  lang_c_registration_release_v1(registration.context);
  std::printf("C_CALLBACK_CLOSE_LEASE_WAIT_OK\n");
}

// ---------------------------------------------------------------------------
// An existing callback may nest borrows while close is pending; a callback the
// foreign library dispatches from inside its unregister is still served.

void test_nested_while_closing() {
  reset_counters();
  LangVec *env = make_env(10);
  Registration registration = register_captured(env);
  std::atomic<bool> entered{false};
  std::atomic<bool> nested_done{false};
  std::atomic<bool> close_done{false};
  std::atomic<void *> nested_token{nullptr};

  std::thread callback([&] {
    lang::native::gc_program::ThreadGuard guard;
    LangCCallbackToken outer{};
    const LangCCallbackFn *fn =
        lang_c_registration_enter_v1(registration.context, &outer);
    CHECK(fn != nullptr);
    entered.store(true, std::memory_order_release);
    while (g_unregister_calls.load(std::memory_order_acquire) == 0)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    // The foreign stop is in progress: a nested call from this existing
    // callback is still allowed.
    LangCCallbackToken lease{};
    nested_token.store(
        lang_c_registration_borrow_v1(registration.context, &lease),
        std::memory_order_release);
    lang_c_registration_end_borrow_v1(&lease);
    nested_done.store(true, std::memory_order_release);
    lang_c_registration_leave_v1(&outer);
  });
  CHECK(wait_for(entered));

  // The foreign library may still dispatch a callback synchronously inside its
  // quiescent unregister; it must be served normally.
  g_unregister_dispatch_ctx = registration.context;
  g_on_unregister = [] {
    void *context = g_unregister_dispatch_ctx;
    if (context)
      CHECK(dispatch(context, 7));
  };

  std::thread closer([&] {
    lang::native::gc_program::ThreadGuard guard;
    lang_c_registration_close_v1(registration.context);
    close_done.store(true, std::memory_order_release);
  });
  CHECK(wait_for(nested_done));
  CHECK(nested_token.load(std::memory_order_acquire) == registration.token);
  callback.join();
  closer.join();
  CHECK(close_done.load(std::memory_order_acquire));
  CHECK(g_unregister_calls.load() == 1);
  CHECK(g_calls.load() == 1); // the dispatch from inside unregister
  CHECK(g_last.load() == 17);
  CHECK(g_env_element_drops.load() == 1);
  lang_c_registration_release_v1(registration.context);
  std::printf("C_CALLBACK_CLOSING_NESTED_OK\n");
}

// ---------------------------------------------------------------------------
// A destructor that runs while the env root is dropped may re-enter release on
// the same thread; after env ownership is detached that re-entry no-ops and
// does not arm the final free, so an explicit close stays idempotent.

void test_env_destructor_reentry() {
  reset_counters();
  g_reentry_calls.store(0);
  LangVec *env = make_env(0);
  Registration registration = register_captured(env);
  g_on_env_reentry = [] {
    // Same thread, after detach: must return without waiting or panicking.
    lang_c_registration_release_v1(g_observed_env);
    g_reentry_calls.fetch_add(1, std::memory_order_relaxed);
  };
  g_observed_env = registration.context;
  lang_c_registration_close_v1(registration.context);
  CHECK(g_reentry_calls.load(std::memory_order_relaxed) == 1);
  CHECK(g_unregister_calls.load() == 1);
  CHECK(g_env_element_drops.load() == 1);
  // Still idempotent: close did not free the context.
  lang_c_registration_close_v1(registration.context);
  CHECK(g_env_element_drops.load() == 1);
  g_observed_env = nullptr;
  lang_c_registration_release_v1(registration.context);
  std::printf("C_CALLBACK_ENV_REENTRY_OK\n");
}

// ---------------------------------------------------------------------------
// A null foreign token consumes the failed registration: one env drop, no
// unregister call, and null returned to Source.

void test_failed_registration_cleanup() {
  reset_counters();
  LangVec *env = make_env(0);
  void *context = lang_c_registration_new_v1(
      reinterpret_cast<void *>(&captured_code), env, &fake_unregister);
  CHECK(context != nullptr);
  CHECK(__atomic_load_n(&env->refs, __ATOMIC_RELAXED) == 2);
  CHECK(lang_c_registration_finish_v1(context, nullptr) == nullptr);
  CHECK(__atomic_load_n(&env->refs, __ATOMIC_RELAXED) == 1);
  lang_vec_release_v1(env);
  CHECK(g_env_element_drops.load() == 1);
  CHECK(g_unregister_calls.load() == 0);
  std::printf("C_CALLBACK_FAILED_FINISH_OK\n");
}

// ---------------------------------------------------------------------------
// Collector release: the already-retained env stays an external root while the
// world is stopped, and unregister/env drop happen only after resume.

struct Payload {
  uint64_t refs; // must stay first for lang-gc/1
  Payload *edge;
  void *context;
};

void payload_unref(Payload *payload);

const LangGcOps kPayloadOps = {
    [](void *object, LangGcVisitor visitor, void *context) noexcept {
      auto *payload = static_cast<Payload *>(object);
      if (payload->edge)
        visitor(payload->edge, context);
    },
    [](void *object) noexcept {
      auto *payload = static_cast<Payload *>(object);
      Payload *edge = payload->edge;
      payload->edge = nullptr;
      if (edge)
        payload_unref(edge);
    },
    [](void *object) noexcept {
      auto *payload = static_cast<Payload *>(object);
      __atomic_add_fetch(&payload->refs, 1u, __ATOMIC_RELAXED);
    },
    [](void *object) noexcept { payload_unref(static_cast<Payload *>(object)); },
    nullptr,
};

void payload_unref(Payload *payload) {
  if (__atomic_sub_fetch(&payload->refs, 1u, __ATOMIC_ACQ_REL) != 0)
    return;
  (void)lang_gc_runtime_unregister_v1(payload);
  if (payload->context) {
    auto *env = static_cast<LangVec *>(g_observed_env);
    CHECK(env != nullptr);
    // Still inside the collector window: the env root and its element are alive
    // and no teardown work ran while the world was stopped.
    CHECK(__atomic_load_n(&env->refs, __ATOMIC_RELAXED) == 1);
    lang_c_registration_release_v1(payload->context);
    CHECK(__atomic_load_n(&env->refs, __ATOMIC_RELAXED) == 1);
    CHECK(g_env_element_drops.load(std::memory_order_relaxed) == 0);
    CHECK(g_unregister_calls.load(std::memory_order_relaxed) == 0);
    g_deferred_releases.fetch_add(1, std::memory_order_relaxed);
    payload->context = nullptr;
  }
  lang_free_v1(payload);
}

void test_gc_deferred_external_root() {
  reset_counters();
  g_deferred_releases.store(0);
  LangVec *env = make_env(0);
  Registration registration = register_captured(env);
  g_observed_env = env;

  auto *first =
      static_cast<Payload *>(lang_alloc_v1(sizeof(Payload), alignof(Payload)));
  auto *second =
      static_cast<Payload *>(lang_alloc_v1(sizeof(Payload), alignof(Payload)));
  CHECK(first != nullptr && second != nullptr);
  first->refs = 2;
  first->edge = second;
  first->context = registration.context;
  second->refs = 2;
  second->edge = first;
  second->context = nullptr;
  CHECK(lang_gc_runtime_register_v1(first, &kPayloadOps) == LANG_GC_OK);
  CHECK(lang_gc_runtime_register_v1(second, &kPayloadOps) == LANG_GC_OK);
  // Drop both external owners; only the cycle keeps the payloads alive.
  __atomic_sub_fetch(&first->refs, 1u, __ATOMIC_ACQ_REL);
  __atomic_sub_fetch(&second->refs, 1u, __ATOMIC_ACQ_REL);
  CHECK(__atomic_load_n(&first->refs, __ATOMIC_RELAXED) == 1);
  CHECK(__atomic_load_n(&second->refs, __ATOMIC_RELAXED) == 1);

  LangGcStats stats{};
  CHECK(lang_gc_runtime_collect_v1(2000000000LL, &stats) == 0);
  CHECK(g_deferred_releases.load() == 1);
  CHECK(lang_gc_registry_live_v1() == 0);
  // The deferred release ran after the world resumed and the controller cleared
  // its busy phase, and dropped the external root exactly once.
  CHECK(g_env_element_drops.load() == 1);
  CHECK(g_env_drop_busy.load() == 0);
  CHECK(g_unregister_calls.load() == 1);
  CHECK(g_unregister_busy.load() == 0);
  std::printf("C_CALLBACK_GC_DEFERRED_ROOT_OK\n");
}

// ---------------------------------------------------------------------------
// An outer SAFE region (the generated C-call/cleanup caller) must not break
// the teardown: the runtime uses the existing GC cleanup scope so waits and the
// foreign stop are nested SAFE and the env drop is ACTIVE, then restores the
// caller's prior SAFE depth.

void test_outer_safe_close() {
  reset_counters();
  g_active_env_drops.store(0);
  LangVec *env = make_env(0);
  Registration registration = register_captured(env);
  g_on_env_drop = &require_active_env_drop;
  {
    lang::native::gc_program::SafeGuard outer;
    lang_c_registration_close_v1(registration.context);
  }
  // Leaving the outer SAFE scope without a panic proves the depth was restored.
  CHECK(g_active_env_drops.load() == 1);
  CHECK(g_env_drop_busy.load() == 0);
  CHECK(g_env_element_drops.load() == 1);
  CHECK(g_unregister_calls.load() == 1);
  g_on_env_drop = nullptr;
  // The thread is ACTIVE again after the caller's SAFE scope exited.
  lang_gc_world_enter_critical_v1();
  lang_gc_world_leave_critical_v1();
  lang_c_registration_release_v1(registration.context);
  CHECK(g_env_element_drops.load() == 1);
  std::printf("C_CALLBACK_OUTER_SAFE_CLOSE_OK\n");
}

void test_outer_safe_release() {
  reset_counters();
  g_active_env_drops.store(0);
  LangVec *env = make_env(0);
  Registration registration = register_captured(env);
  g_on_env_drop = &require_active_env_drop;
  {
    lang::native::gc_program::SafeGuard outer;
    lang_c_registration_release_v1(registration.context); // tears down and frees
  }
  CHECK(g_active_env_drops.load() == 1);
  CHECK(g_env_drop_busy.load() == 0);
  CHECK(g_env_element_drops.load() == 1);
  CHECK(g_unregister_calls.load() == 1);
  g_on_env_drop = nullptr;
  lang_gc_world_enter_critical_v1();
  lang_gc_world_leave_critical_v1();
  std::printf("C_CALLBACK_OUTER_SAFE_RELEASE_OK\n");
}

// ---------------------------------------------------------------------------
// A follower closer waits SAFE while the leader is blocked inside the foreign
// stop, so a real collection can still stop/resume the world.

void test_concurrent_follower_close_and_collection() {
  reset_counters();
  LangVec *env = make_env(0);
  Registration registration = register_captured(env);
  g_thread_context = registration.context;
  g_block_stop_entered.store(false);
  g_block_stop_resume.store(false);
  g_leader_done.store(false);
  g_follower_done.store(false);
  g_follower_waiting.store(false);
  g_follower_tid.store(0);
  g_on_unregister = [] {
    g_block_stop_entered.store(true, std::memory_order_release);
    while (!g_block_stop_resume.load(std::memory_order_acquire))
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  };

  std::thread leader([] {
    lang::native::gc_program::ThreadGuard guard;
    lang_c_registration_close_v1(g_thread_context);
    g_leader_done.store(true, std::memory_order_release);
  });
  CHECK(wait_for(g_block_stop_entered));

  std::thread follower([] {
    lang::native::gc_program::ThreadGuard guard;
    g_follower_tid.store(static_cast<int32_t>(::syscall(SYS_gettid)),
                         std::memory_order_release);
    lang_c_registration_close_v1(g_thread_context);
    g_follower_done.store(true, std::memory_order_release);
  });
  CHECK(wait_for(g_follower_waiting));
  CHECK(!g_leader_done.load(std::memory_order_acquire));
  CHECK(!g_follower_done.load(std::memory_order_acquire));

  // Both closers are blocked (leader in the foreign stop, follower in its SAFE
  // condition wait); a real stop-the-world collection must still succeed.
  LangGcStats stats{};
  const int64_t collected = lang_gc_runtime_collect_v1(30000000LL, &stats);
  CHECK(collected == 0);
  CHECK(!g_leader_done.load(std::memory_order_acquire));
  CHECK(!g_follower_done.load(std::memory_order_acquire));

  g_block_stop_resume.store(true, std::memory_order_release);
  leader.join();
  follower.join();
  CHECK(g_leader_done.load(std::memory_order_acquire));
  CHECK(g_follower_done.load(std::memory_order_acquire));
  CHECK(g_unregister_calls.load() == 1); // exactly once
  CHECK(g_env_element_drops.load() == 1);
  CHECK(g_env_drop_busy.load() == 0);
  lang_c_registration_release_v1(registration.context);
  std::printf("C_CALLBACK_CONCURRENT_FOLLOWER_GC_OK\n");
}

} // namespace

int main() {
  static_assert(sizeof(LangCCallbackFn) == 16);
  static_assert(sizeof(LangCCallbackToken) == 16);
  C_CALLBACK_CHECK(lang_gc_program_init_v1() == 0);
  test_synchronous_and_foreign_threads();
  test_close_waits_for_active_callback();
  test_close_waits_for_lease();
  test_nested_while_closing();
  test_env_destructor_reentry();
  test_failed_registration_cleanup();
  test_gc_deferred_external_root();
  test_outer_safe_close();
  test_outer_safe_release();
  test_concurrent_follower_close_and_collection();
  (void)lang_gc_program_finish_v1();
  std::printf("C_CALLBACK_RUNTIME_OK\n");
  return 0;
}
