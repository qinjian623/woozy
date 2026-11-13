// Process-isolated checks for the private capturing C registration runtime:
// fork inheritance, terminal self-wait/closed-use Panics and same-thread
// close re-entry. Every case forks from a single-threaded parent, redirects the
// child's stderr into a pipe and inspects the terminal Panic record, so a
// rejected operation never terminates the harness itself.

#include "c_callback_fixture.hpp"

#include "alloc.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>

#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace callback_fixture;

#define CHECK C_CALLBACK_CHECK

struct Outcome {
  int status = -1;
  std::string errors;
};

void *g_inherited_context = nullptr;
LangVec *g_inherited_env = nullptr;
LangCCallbackToken g_inherited_token{};
void *g_pending_foreign_token = nullptr;
void *g_child_context = nullptr;
std::atomic<bool> g_stop_entered{false};
std::atomic<bool> g_release_stop{false};

// ---------------------------------------------------------------------------
// Case A: inherited implicit release is a total no-op (no reference, env,
// token or unregister work), an already-active inherited callback only restores
// TLS on leave, and a new child-created context works normally.

void child_inherited_release_and_new_child() {
  LangVec *env = g_inherited_env;
  CHECK(__atomic_load_n(&env->refs, __ATOMIC_RELAXED) == 1);
  lang_c_registration_release_v1(g_inherited_context);
  CHECK(__atomic_load_n(&env->refs, __ATOMIC_RELAXED) == 1);
  CHECK(g_unregister_calls.load() == 0);
  CHECK(g_env_element_drops.load() == 0);
  // Inherited callback leave: TLS stack only, no counters and no locks.
  lang_c_registration_leave_v1(&g_inherited_token);

  LangVec *child_env = make_env(5);
  Registration child = register_captured(child_env);
  CHECK(dispatch(child.context, 3));
  CHECK(g_last.load() == 8);
  CHECK(g_calls.load() == 1);
  lang_c_registration_close_v1(child.context);
  CHECK(g_unregister_calls.load() == 1);
  CHECK(g_env_element_drops.load() == 1);
  lang_c_registration_release_v1(child.context);
}

void child_inherited_close() {
  lang_c_registration_close_v1(g_inherited_context);
}

void child_inherited_borrow() {
  LangCCallbackToken token{};
  lang_c_registration_borrow_v1(g_inherited_context, &token);
}

void child_inherited_enter() {
  LangCCallbackToken token{};
  lang_c_registration_enter_v1(g_inherited_context, &token);
}

void child_inherited_finish() {
  lang_c_registration_finish_v1(g_inherited_context, g_pending_foreign_token);
}

void child_inherited_finish_null() {
  lang_c_registration_finish_v1(g_inherited_context, nullptr);
}

// ---------------------------------------------------------------------------
// Case E: closing one's own callback is a terminal Panic before any wait.

void child_self_close() {
  reset_counters();
  LangVec *env = make_env(0);
  Registration registration = register_captured(env);
  LangCCallbackToken token{};
  CHECK(lang_c_registration_enter_v1(registration.context, &token) != nullptr);
  lang_c_registration_close_v1(registration.context);
}

// ---------------------------------------------------------------------------
// Case F: close rejects unrelated new leases while the foreign stop is in
// progress (an existing callback is still being waited for).

void child_unrelated_borrow_while_closing() {
  reset_counters();
  LangVec *env = make_env(0);
  Registration registration = register_captured(env);
  g_child_context = registration.context;
  g_unregister_dispatch_ctx = registration.context;
  g_on_unregister = [] {
    g_stop_entered.store(true, std::memory_order_release);
    while (!g_release_stop.load(std::memory_order_acquire))
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  };
  std::thread holder([] {
    LangCCallbackToken token{};
    CHECK(lang_c_registration_enter_v1(g_child_context, &token) != nullptr);
    while (true)
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
  });
  std::thread closer([] { lang_c_registration_close_v1(g_child_context); });
  while (!g_stop_entered.load(std::memory_order_acquire))
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  LangCCallbackToken token{};
  lang_c_registration_borrow_v1(g_child_context, &token);
  (void)holder;
  (void)closer;
}

// ---------------------------------------------------------------------------
// Entering a null or already-closed registration is a terminal misuse (the
// bridge must never skip a real callback silently).

void child_closed_enter() {
  reset_counters();
  LangVec *env = make_env(0);
  Registration registration = register_captured(env);
  lang_c_registration_close_v1(registration.context);
  LangCCallbackToken token{};
  lang_c_registration_enter_v1(registration.context, &token);
}

void child_null_enter() {
  LangCCallbackToken token{};
  lang_c_registration_enter_v1(nullptr, &token);
}

// ---------------------------------------------------------------------------
// The TLS token stack is LIFO: leaving/ending an outer callback/lease while a
// nested one is still on top is a terminal misuse.

void child_token_order_leave() {
  reset_counters();
  LangVec *env = make_env(0);
  Registration registration = register_captured(env);
  LangCCallbackToken outer{};
  LangCCallbackToken inner{};
  CHECK(lang_c_registration_enter_v1(registration.context, &outer) != nullptr);
  CHECK(lang_c_registration_enter_v1(registration.context, &inner) != nullptr);
  lang_c_registration_leave_v1(&outer);
}

void child_token_order_borrow() {
  reset_counters();
  LangVec *env = make_env(0);
  Registration registration = register_captured(env);
  LangCCallbackToken outer{};
  LangCCallbackToken inner{};
  CHECK(lang_c_registration_borrow_v1(registration.context, &outer) != nullptr);
  CHECK(lang_c_registration_borrow_v1(registration.context, &inner) != nullptr);
  lang_c_registration_end_borrow_v1(&outer);
}

// ---------------------------------------------------------------------------
// Case G: re-entering close on an inherited/unfinished foreign stop Panics.

void child_reentry_close() {
  reset_counters();
  LangVec *env = make_env(0);
  Registration registration = register_captured(env);
  g_child_context = registration.context;
  g_on_unregister = [] { lang_c_registration_close_v1(g_child_context); };
  lang_c_registration_close_v1(registration.context);
}

Outcome run_child(void (*body)()) {
  int fds[2];
  CHECK(pipe(fds) == 0);
  std::fflush(nullptr);
  // The parent is single-threaded here, so the allocator ticket lock is free at
  // fork. The hosted atfork hooks (when linked) perform the full prepare/parent
  // sequence themselves; calling them here as well would double-acquire.
  const pid_t pid = fork();
  if (pid == 0) {
    (void)close(fds[0]);
    if (dup2(fds[1], 2) < 0)
      _exit(121);
    (void)close(fds[1]);
    // Idempotent child reset for the inherited allocator ticket words.
    lang_alloc_fork_child_v1();
    body();
    _exit(0);
  }
  CHECK(pid > 0);
  (void)close(fds[1]);
  Outcome outcome{};
  char buffer[1024];
  ssize_t count;
  while ((count = ::read(fds[0], buffer, sizeof buffer)) > 0)
    outcome.errors.append(buffer, static_cast<size_t>(count));
  (void)close(fds[0]);
  int status = 0;
  CHECK(waitpid(pid, &status, 0) == pid);
  outcome.status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return outcome;
}

void expect_panic(const Outcome &outcome, int marker) {
  char needle[128];
  std::snprintf(needle, sizeof needle,
                "LANG_NATIVE_PANIC:v1:runtime-handle-misuse:%d:0:0", marker);
  const bool found = outcome.errors.find(needle) != std::string::npos;
  if (!found)
    std::fprintf(stderr, "child status=%d stderr:\n%s\n", outcome.status,
                 outcome.errors.c_str());
  CHECK(outcome.status == 101);
  CHECK(found);
}

} // namespace

int main() {
  static_assert(sizeof(LangCCallbackToken) == 16);

  // Case A: inherited implicit release/leave plus a working child context.
  {
    reset_counters();
    LangVec *env = make_env(0);
    Registration registration = register_captured(env);
    g_inherited_context = registration.context;
    g_inherited_env = env;
    CHECK(lang_c_registration_enter_v1(registration.context,
                                       &g_inherited_token) != nullptr);
    const Outcome outcome = run_child(&child_inherited_release_and_new_child);
    if (outcome.status != 0)
      std::fprintf(stderr, "case A stderr:\n%s\n", outcome.errors.c_str());
    CHECK(outcome.status == 0);
    // The parent's registration and env root are untouched by the child.
    CHECK(g_unregister_calls.load() == 0);
    CHECK(g_env_element_drops.load() == 0);
    CHECK(__atomic_load_n(&env->refs, __ATOMIC_RELAXED) == 1);
    lang_c_registration_leave_v1(&g_inherited_token);
    lang_c_registration_close_v1(registration.context);
    CHECK(g_unregister_calls.load() == 1);
    CHECK(g_env_element_drops.load() == 1);
    lang_c_registration_release_v1(registration.context);
    std::printf("C_CALLBACK_FORK_INHERITED_OK\n");
  }

  // Cases B/C/D: inherited explicit close/borrow/begin callback Panic with the
  // inherited marker before any context lock.
  {
    reset_counters();
    LangVec *env = make_env(0);
    Registration registration = register_captured(env);
    g_inherited_context = registration.context;
    g_inherited_env = env;
    expect_panic(run_child(&child_inherited_close), 1);
    expect_panic(run_child(&child_inherited_borrow), 1);
    expect_panic(run_child(&child_inherited_enter), 1);
    // The parent still owns a healthy registration.
    CHECK(dispatch(registration.context, 4));
    CHECK(g_calls.load() == 1);
    lang_c_registration_close_v1(registration.context);
    CHECK(g_unregister_calls.load() == 1);
    CHECK(g_env_element_drops.load() == 1);
    lang_c_registration_release_v1(registration.context);
    std::printf("C_CALLBACK_FORK_INHERITED_USE_OK\n");
  }

  // A synchronous callback may fork before the C constructor returns.
  // Neither successful nor failed finish may touch the inherited context.
  {
    reset_counters();
    LangVec *env = make_env(0);
    g_inherited_context = lang_c_registration_new_v1(
        reinterpret_cast<void *>(&captured_code), env, &fake_unregister);
    lang_vec_release_v1(env);
    g_pending_foreign_token = make_token();
    g_expected_token.store(g_pending_foreign_token);
    expect_panic(run_child(&child_inherited_finish), 1);
    expect_panic(run_child(&child_inherited_finish_null), 1);
    CHECK(__atomic_load_n(&env->refs, __ATOMIC_RELAXED) == 1);
    CHECK(g_env_element_drops.load() == 0);
    CHECK(lang_c_registration_finish_v1(g_inherited_context,
                                        g_pending_foreign_token) ==
          g_inherited_context);
    lang_c_registration_release_v1(g_inherited_context);
    CHECK(g_unregister_calls.load() == 1);
    CHECK(g_env_element_drops.load() == 1);
    std::printf("C_CALLBACK_FORK_PENDING_FINISH_OK\n");
  }

  // Case E: self-close inside one's own callback.
  expect_panic(run_child(&child_self_close), 2);
  std::printf("C_CALLBACK_SELF_WAIT_PANIC_OK\n");

  // Case F: unrelated new lease while close is pending.
  expect_panic(run_child(&child_unrelated_borrow_while_closing), 3);
  std::printf("C_CALLBACK_CLOSING_BORROW_PANIC_OK\n");

  // Case G: close re-entry before the foreign stop finishes.
  expect_panic(run_child(&child_reentry_close), 5);
  std::printf("C_CALLBACK_REENTRY_PANIC_OK\n");

  // Case H: entering a null/closed registration must Panic, not skip.
  expect_panic(run_child(&child_closed_enter), 3);
  expect_panic(run_child(&child_null_enter), 3);
  std::printf("C_CALLBACK_CLOSED_ENTER_PANIC_OK\n");

  // Case I: out-of-order TLS token pop (callback and lease stacks).
  expect_panic(run_child(&child_token_order_leave), 8);
  expect_panic(run_child(&child_token_order_borrow), 8);
  std::printf("C_CALLBACK_TOKEN_ORDER_PANIC_OK\n");

  std::printf("C_CALLBACK_PROCESS_OK\n");
  return 0;
}
