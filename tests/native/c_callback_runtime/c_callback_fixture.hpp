// Shared controlled C++ fixture for the private capturing C registration
// runtime tests.
//
// It models one ordinary C library registration API:
//
//   register(userdata, callback) -> opaque token   (new + finish here)
//   unregister(token)                              (quiescent stop)
//   invoke the stored callback trampoline later, possibly on another thread
//
// The typed bridge shape mirrors the generated adapter contract: the existing
// 32-byte GC callback scope surrounds Fn.code, the private 16-byte runtime
// token is separate, and runtime leave runs after the GC callback scope.

#ifndef LANG_TESTS_NATIVE_C_CALLBACK_RUNTIME_FIXTURE_HPP
#define LANG_TESTS_NATIVE_C_CALLBACK_RUNTIME_FIXTURE_HPP

#include "c_callback.hpp"

#include "alloc.hpp"
#include "gc_program.hpp"
#include "gc_runtime.hpp"
#include "gc_world.hpp"
#include "vec.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace callback_fixture {

[[noreturn]] inline void fail_at(const char *file, int line,
                                 const char *what) {
  std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, what);
  std::fflush(stderr);
  std::_Exit(1);
}

#define C_CALLBACK_CHECK(condition)                                            \
  do {                                                                         \
    if (!(condition))                                                          \
      callback_fixture::fail_at(__FILE__, __LINE__, #condition);               \
  } while (0)

inline std::atomic<int64_t> g_calls{0};
inline std::atomic<int64_t> g_last{0};
inline std::atomic<int64_t> g_env_element_drops{0};
inline std::atomic<int64_t> g_env_drop_busy{0};
inline std::atomic<int64_t> g_unregister_calls{0};
inline std::atomic<int64_t> g_unregister_busy{0};
inline std::atomic<void *> g_expected_token{nullptr};
inline std::atomic<int64_t> g_bad_token{0};

// Test-controlled hooks; plain function pointers so neither the runtime nor the
// fixture needs std::function.
inline void (*g_on_unregister)() = nullptr;
inline void (*g_on_env_drop)() = nullptr;
inline void (*g_on_env_reentry)() = nullptr;
inline void *g_observed_env = nullptr;
inline void *g_unregister_dispatch_ctx = nullptr;

inline uintptr_t g_token_counter = 0;

inline void *make_token() {
  g_token_counter += 0x40;
  return reinterpret_cast<void *>(g_token_counter);
}

inline bool gc_busy() {
  LangGcRuntimeStats stats{};
  lang_gc_runtime_stats_v1(&stats);
  return stats.busy != 0 || stats.enqueue_open != 0;
}

// Element drop callback of the closure environment Vec. The Vec descriptor is
// the retained external root; this records the final drop and whether it ran
// while the collector was stopped.
inline void env_element_drop(void *) noexcept {
  if (gc_busy())
    g_env_drop_busy.fetch_add(1, std::memory_order_relaxed);
  g_env_element_drops.fetch_add(1, std::memory_order_relaxed);
  if (g_on_env_drop)
    g_on_env_drop();
  if (g_on_env_reentry) {
    auto hook = g_on_env_reentry;
    g_on_env_reentry = nullptr;
    hook();
  }
}

inline void fake_unregister(void *token) noexcept {
  if (token != g_expected_token.load(std::memory_order_relaxed))
    g_bad_token.fetch_add(1, std::memory_order_relaxed);
  if (gc_busy())
    g_unregister_busy.fetch_add(1, std::memory_order_relaxed);
  g_unregister_calls.fetch_add(1, std::memory_order_relaxed);
  if (g_on_unregister) {
    auto hook = g_on_unregister;
    g_on_unregister = nullptr;
    hook();
  }
}

// Closure body copied into LangCCallbackFn.code: env first, then the typed
// callback parameters. `env` is the retained LangVec closure environment.
inline void captured_code(void *env, int64_t value) noexcept {
  auto *vec = static_cast<LangVec *>(env);
  const int64_t base = *static_cast<int64_t *>(lang_vec_at_v1(vec, 0));
  g_last.store(base + value, std::memory_order_relaxed);
  g_calls.fetch_add(1, std::memory_order_relaxed);
}

inline bool dispatch(void *context, int64_t value) {
  LangGcProgramCallbackToken gc_token{};
  const int32_t entered = lang_gc_program_callback_enter_v1(&gc_token);
  LangCCallbackToken token{};
  const LangCCallbackFn *fn = lang_c_registration_enter_v1(context, &token);
  bool called = false;
  if (fn) {
    reinterpret_cast<void (*)(void *, int64_t)>(fn->code)(fn->env, value);
    called = true;
  }
  lang_gc_program_callback_leave_v1(entered, &gc_token);
  lang_c_registration_leave_v1(&token);
  return called;
}

inline LangVec *make_env(int64_t base) {
  LangVec *env =
      lang_vec_new_v1(1, sizeof(int64_t), nullptr, &env_element_drop);
  C_CALLBACK_CHECK(env != nullptr);
  lang_vec_init_v1(env, &base);
  return env;
}

struct Registration {
  void *context;
  void *token;
};

// Creates a fully initialized context (ACTIVE, before the C call) and finishes
// it after the registration returned the raw token. The caller's own env
// reference is dropped here, so the context is the only external root.
inline Registration register_captured(LangVec *env) {
  Registration registration{};
  registration.token = make_token();
  g_expected_token.store(registration.token, std::memory_order_relaxed);
  registration.context = lang_c_registration_new_v1(
      reinterpret_cast<void *>(&captured_code), env, &fake_unregister);
  C_CALLBACK_CHECK(registration.context != nullptr);
  C_CALLBACK_CHECK(__atomic_load_n(&env->refs, __ATOMIC_RELAXED) == 2);
  void *published =
      lang_c_registration_finish_v1(registration.context, registration.token);
  C_CALLBACK_CHECK(published == registration.context);
  lang_vec_release_v1(env); // the test's owner; the context root remains
  C_CALLBACK_CHECK(__atomic_load_n(&env->refs, __ATOMIC_RELAXED) == 1);
  return registration;
}

inline void reset_counters() {
  g_calls.store(0);
  g_last.store(0);
  g_env_element_drops.store(0);
  g_env_drop_busy.store(0);
  g_unregister_calls.store(0);
  g_unregister_busy.store(0);
  g_bad_token.store(0);
  g_expected_token.store(nullptr);
  g_on_unregister = nullptr;
  g_on_env_drop = nullptr;
  g_on_env_reentry = nullptr;
  g_observed_env = nullptr;
  g_unregister_dispatch_ctx = nullptr;
}

inline bool wait_for(const std::atomic<bool> &flag, int milliseconds = 5000) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(milliseconds);
  while (!flag.load(std::memory_order_acquire)) {
    if (std::chrono::steady_clock::now() > deadline)
      return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

} // namespace callback_fixture

#endif
