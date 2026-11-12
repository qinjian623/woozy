// Host-side evidence for the low-level timeout ABI in runtime/native.
// The runtime under test still uses raw Linux x86_64 syscalls; std::thread is
// used only to create independent host threads and to drive signals/forks.
#include "../../../runtime/native/sync.hpp"
#include "../../../runtime/native/thread.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <pthread.h>
#include <sched.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static_assert(std::is_same_v<decltype(&lang_thread_wait_for_v1),
                             int64_t (*)(LangHandle *, int64_t) noexcept>);
static_assert(std::is_same_v<decltype(&lang_condvar_wait_for_v1),
                             bool (*)(LangHandle *, LangHandle *,
                                      int64_t) noexcept>);

namespace {
using Clock = std::chrono::steady_clock;

void need(bool value, const std::string &message) {
  if (!value) throw std::runtime_error(message);
}
void pause_cpu() noexcept { __asm__ volatile("pause" ::: "memory"); }
int64_t elapsed_ms(Clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                               start)
      .count();
}
void until(const auto &predicate, const std::string &message,
           int64_t milliseconds = 5000) {
  const auto end = Clock::now() + std::chrono::milliseconds(milliseconds);
  while (!predicate()) {
    need(Clock::now() < end, message);
    sched_yield();
  }
}

struct Gate {
  std::atomic<uint32_t> open{0};
};
int64_t gate_entry(void *pointer) noexcept {
  auto *gate = *static_cast<Gate **>(pointer);
  while (!gate->open.load(std::memory_order_acquire)) pause_cpu();
  return 17;
}
LangHandle *start_gate(Gate &gate) {
  Gate *argument = &gate;
  auto *thread = lang_thread_spawn_v1(gate_entry, &argument, sizeof(argument),
                                      nullptr, nullptr);
  need(lang_thread_error_v1(thread) == 0, "thread creation failed");
  return thread;
}
void finish_gate(LangHandle *thread, Gate &gate) {
  gate.open.store(1, std::memory_order_release);
  need(lang_thread_wait_v1(thread) == 0, "infinite join failed");
  need(lang_thread_result_v1(thread) == 17, "thread result changed");
  lang_handle_release_v1(thread);
}

struct JoinInput {
  LangHandle *target;
  int64_t timeout;
};
int64_t join_entry(void *pointer) noexcept {
  auto &input = *static_cast<JoinInput *>(pointer);
  return lang_thread_wait_for_v1(input.target, input.timeout);
}
void retain_join(void *pointer) {
  lang_handle_retain_v1(static_cast<JoinInput *>(pointer)->target);
}
void drop_join(void *pointer) {
  lang_handle_release_v1(static_cast<JoinInput *>(pointer)->target);
}

struct SelfInput {
  LangHandle **slot;
  int64_t timeout;
};
int64_t self_entry(void *pointer) noexcept {
  auto &input = *static_cast<SelfInput *>(pointer);
  LangHandle *thread;
  while (!(thread = __atomic_load_n(input.slot, __ATOMIC_ACQUIRE))) pause_cpu();
  return lang_thread_wait_for_v1(thread, input.timeout);
}

void install_signal_handler() {
  struct sigaction action {};
  action.sa_handler = [](int) {};
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0; // no SA_RESTART
  need(sigaction(SIGUSR1, &action, nullptr) == 0, "sigaction failed");
}

struct ChildResult {
  int status;
  std::string error;
};
ChildResult run_child(void (*body)()) {
  int fds[2];
  need(pipe(fds) == 0, "pipe failed");
  const pid_t child = fork();
  need(child >= 0, "fork failed");
  if (!child) {
    close(fds[0]);
    if (dup2(fds[1], 2) < 0) _exit(97);
    close(fds[1]);
    body();
    _exit(0);
  }
  close(fds[1]);
  std::string output;
  char buffer[512];
  for (;;) {
    const ssize_t size = read(fds[0], buffer, sizeof(buffer));
    if (size > 0) {
      output.append(buffer, static_cast<size_t>(size));
      continue;
    }
    if (size < 0 && errno == EINTR) continue;
    break;
  }
  close(fds[0]);
  int status = 0;
  need(waitpid(child, &status, 0) == child, "waitpid failed");
  return {status, output};
}
void expect_handle_panic(const char *name, void (*body)()) {
  const auto result = run_child(body);
  need(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 101,
       std::string(name) + " did not exit 101");
  need(result.error.find("LANG_NATIVE_PANIC:v1:runtime-handle-misuse:") !=
           std::string::npos,
       std::string(name) + " missing misuse record: " + result.error);
}

void test_thread_timeouts() {
  install_signal_handler();

  {
    Gate gate;
    auto *thread = start_gate(gate);
    need(lang_thread_wait_for_v1(thread, -2) == -22,
         "timeout below -1 must be a parameter error");
    need(lang_thread_wait_for_v1(thread, 0) == -110,
         "nonblocking wait of a running thread must time out");
    finish_gate(thread, gate);
  }

  // The previous block releases the handle, so repeat completion checks with a
  // live handle before releasing it.
  {
    Gate gate;
    auto *thread = start_gate(gate);
    gate.open.store(1, std::memory_order_release);
    until([&] { return lang_thread_wait_for_v1(thread, 0) == 0; },
          "completed thread was not visible");
    need(lang_thread_wait_for_v1(thread, 0) == 0,
         "zero wait on completed thread must not sleep");
    need(lang_thread_wait_for_v1(thread, 150000000) == 0,
         "positive wait on completed thread failed");
    need(lang_thread_result_v1(thread) == 17, "completed result changed");
    lang_handle_release_v1(thread);
  }

  {
    Gate gate;
    auto *thread = start_gate(gate);
    const auto start = Clock::now();
    need(lang_thread_wait_for_v1(thread, 150000000) == -110,
         "positive thread timeout did not return -110");
    const auto elapsed = elapsed_ms(start);
    need(elapsed >= 100 && elapsed < 2000,
         "thread timeout was not monotonic/approximately 150ms: " +
             std::to_string(elapsed));
    finish_gate(thread, gate);
  }

  {
    Gate gate;
    auto *thread = start_gate(gate);
    std::thread opener([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      gate.open.store(1, std::memory_order_release);
    });
    need(lang_thread_wait_for_v1(thread, -1) == 0,
         "infinite timed wait did not complete");
    opener.join();
    need(lang_thread_result_v1(thread) == 17, "infinite timed result changed");
    lang_handle_release_v1(thread);
  }

  {
    Gate gate;
    auto *thread = start_gate(gate);
    std::thread opener([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      gate.open.store(1, std::memory_order_release);
    });
    need(lang_thread_wait_for_v1(thread, INT64_MAX) == 0,
         "saturated huge timeout failed");
    opener.join();
    lang_handle_release_v1(thread);
  }

  {
    Gate gate;
    auto *target = start_gate(gate);
    std::vector<LangHandle *> joiners;
    for (int index = 0; index < 8; ++index) {
      JoinInput input{target, 5000000000LL};
      auto *joiner = lang_thread_spawn_v1(join_entry, &input, sizeof(input),
                                          retain_join, drop_join);
      need(lang_thread_error_v1(joiner) == 0, "joiner creation failed");
      joiners.push_back(joiner);
    }
    lang_handle_release_v1(target);
    gate.open.store(1, std::memory_order_release);
    for (auto *joiner : joiners) {
      need(lang_thread_wait_v1(joiner) == 0, "joiner wait failed");
      need(lang_thread_result_v1(joiner) == 0, "concurrent timed join failed");
      lang_handle_release_v1(joiner);
    }
  }

  {
    LangHandle *self = nullptr;
    auto **slot = &self;
    SelfInput input{slot, 1000000000LL};
    auto *thread = lang_thread_spawn_v1(self_entry, &input, sizeof(input),
                                        nullptr, nullptr);
    need(lang_thread_error_v1(thread) == 0, "self timed join creation failed");
    __atomic_store_n(&self, thread, __ATOMIC_RELEASE);
    need(lang_thread_wait_v1(thread) == 0 &&
             lang_thread_result_v1(thread) == -35,
         "self timed join did not return EDEADLK");
    lang_handle_release_v1(thread);
  }

  {
    Gate gate;
    auto *thread = start_gate(gate);
    std::atomic<bool> stop{false};
    auto main_thread = pthread_self();
    std::thread storm([&] {
      while (!stop.load(std::memory_order_acquire)) {
        pthread_kill(main_thread, SIGUSR1);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    });
    const auto start = Clock::now();
    need(lang_thread_wait_for_v1(thread, 200000000) == -110,
         "interrupted thread wait did not time out");
    const auto elapsed = elapsed_ms(start);
    stop.store(true, std::memory_order_release);
    storm.join();
    need(elapsed < 1500,
         "EINTR restarted the full thread timeout: " + std::to_string(elapsed));
    finish_gate(thread, gate);
  }
}

struct CondShared {
  LangHandle *condition;
  LangHandle *mutex;
  int64_t timeout;
  std::atomic<uint32_t> entered{0};
  std::atomic<uint32_t> holds{0};
  std::atomic<uint32_t> release{0};
  std::atomic<uint32_t> notified{0};
  std::atomic<int> tid{0};
};
struct CondInput {
  CondShared *shared;
};

int64_t cond_timeout_entry(void *pointer) noexcept {
  auto *shared = static_cast<CondInput *>(pointer)->shared;
  shared->tid.store(static_cast<int>(syscall(SYS_gettid)),
                    std::memory_order_release);
  lang_mutex_lock_v1(shared->mutex);
  shared->entered.store(1, std::memory_order_release);
  const bool notified = lang_condvar_wait_for_v1(
      shared->condition, shared->mutex, shared->timeout);
  shared->notified.store(notified ? 1u : 0u, std::memory_order_release);
  shared->holds.store(1, std::memory_order_release);
  while (!shared->release.load(std::memory_order_acquire)) pause_cpu();
  lang_mutex_unlock_v1(shared->mutex);
  return 0;
}

int64_t cond_notify_entry(void *pointer) noexcept {
  auto *shared = static_cast<CondInput *>(pointer)->shared;
  shared->tid.store(static_cast<int>(syscall(SYS_gettid)),
                    std::memory_order_release);
  lang_mutex_lock_v1(shared->mutex);
  shared->entered.store(1, std::memory_order_release);
  const bool notified = lang_condvar_wait_for_v1(
      shared->condition, shared->mutex, shared->timeout);
  shared->notified.store(notified ? 1u : 0u, std::memory_order_release);
  shared->holds.store(1, std::memory_order_release);
  lang_mutex_unlock_v1(shared->mutex);
  return 0;
}

void test_condvar_timeouts() {
  {
    auto *mutex = lang_mutex_new_v1();
    auto *condition = lang_condvar_new_v1();
    CondShared shared{condition, mutex, 150000000};
    CondInput input{&shared};
    auto *waiter = lang_thread_spawn_v1(cond_timeout_entry, &input,
                                        sizeof(input), nullptr, nullptr);
    need(lang_thread_error_v1(waiter) == 0, "cond waiter creation failed");
    until([&] { return shared.entered.load(std::memory_order_acquire) != 0; },
          "cond waiter did not start");

    std::atomic<uint32_t> during_wait{0};
    std::thread contender([&] {
      lang_mutex_lock_v1(mutex);
      during_wait.store(1, std::memory_order_release);
      lang_mutex_unlock_v1(mutex);
    });
    until([&] { return during_wait.load(std::memory_order_acquire) != 0; },
          "cond wait did not release the external mutex", 2000);
    contender.join();

    until([&] { return shared.holds.load(std::memory_order_acquire) != 0; },
          "cond waiter did not return");
    need(shared.notified.load(std::memory_order_acquire) == 0,
         "cond wait_for timeout reported a notification");

    std::atomic<uint32_t> after_return{0};
    std::thread after([&] {
      lang_mutex_lock_v1(mutex);
      after_return.store(1, std::memory_order_release);
      lang_mutex_unlock_v1(mutex);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    need(after_return.load(std::memory_order_acquire) == 0,
         "cond wait_for did not reacquire the external mutex");
    shared.release.store(1, std::memory_order_release);
    after.join();
    need(after_return.load(std::memory_order_acquire) == 1,
         "cond waiter did not eventually release the mutex");
    need(lang_thread_wait_v1(waiter) == 0, "cond waiter join failed");
    lang_handle_release_v1(waiter);
    lang_handle_release_v1(condition);
    lang_handle_release_v1(mutex);
  }

  {
    auto *mutex = lang_mutex_new_v1();
    auto *condition = lang_condvar_new_v1();
    lang_mutex_lock_v1(mutex);
    need(!lang_condvar_wait_for_v1(condition, mutex, 0),
         "zero cond wait must report timeout");
    lang_mutex_unlock_v1(mutex);

    CondShared shared{condition, mutex, -1};
    CondInput input{&shared};
    auto *waiter = lang_thread_spawn_v1(cond_notify_entry, &input,
                                        sizeof(input), nullptr, nullptr);
    need(lang_thread_error_v1(waiter) == 0, "infinite cond waiter failed");
    until([&] { return shared.entered.load(std::memory_order_acquire) != 0; },
          "infinite cond waiter did not start");
    // Taking the same mutex proves the waiter registered before releasing it,
    // so this notification cannot be lost.
    lang_mutex_lock_v1(mutex);
    lang_condvar_notify_one_v1(condition);
    lang_mutex_unlock_v1(mutex);
    need(lang_thread_wait_v1(waiter) == 0 &&
             shared.notified.load(std::memory_order_acquire) == 1,
         "infinite cond wait was not notified");
    need(shared.holds.load(std::memory_order_acquire) == 1,
         "notified cond wait did not hold the mutex");
    lang_handle_release_v1(waiter);
    lang_handle_release_v1(condition);
    lang_handle_release_v1(mutex);
  }

  {
    auto *mutex = lang_mutex_new_v1();
    auto *condition = lang_condvar_new_v1();
    std::vector<LangHandle *> waiters;
    std::vector<CondShared *> shareds;
    for (int index = 0; index < 12; ++index) {
      auto *shared = new CondShared{condition, mutex, 30000000};
      shareds.push_back(shared);
      CondInput input{shared};
      auto *waiter = lang_thread_spawn_v1(cond_notify_entry, &input,
                                          sizeof(input), nullptr, nullptr);
      need(lang_thread_error_v1(waiter) == 0, "batch cond waiter failed");
      waiters.push_back(waiter);
    }
    for (auto *waiter : waiters) {
      need(lang_thread_wait_v1(waiter) == 0, "batch cond waiter join failed");
      lang_handle_release_v1(waiter);
    }
    for (auto *shared : shareds) {
      need(shared->holds.load(std::memory_order_acquire) == 1,
           "batch cond waiter did not return");
      need(shared->notified.load(std::memory_order_acquire) == 0,
           "batch cond waiter reported a notification");
      delete shared;
    }
    lang_handle_release_v1(condition);
    lang_handle_release_v1(mutex);
  }

  {
    auto *mutex = lang_mutex_new_v1();
    auto *condition = lang_condvar_new_v1();
    CondShared shared{condition, mutex, 200000000};
    CondInput input{&shared};
    auto *waiter = lang_thread_spawn_v1(cond_timeout_entry, &input,
                                        sizeof(input), nullptr, nullptr);
    need(lang_thread_error_v1(waiter) == 0, "EINTR cond waiter failed");
    until([&] { return shared.entered.load(std::memory_order_acquire) != 0; },
          "EINTR cond waiter did not start");
    until([&] { return shared.tid.load(std::memory_order_acquire) != 0; },
          "EINTR cond waiter did not publish its tid");
    const int waiter_tid = shared.tid.load(std::memory_order_acquire);
    const pid_t process = getpid();
    std::atomic<bool> stop{false};
    std::thread storm([&] {
      while (!stop.load(std::memory_order_acquire)) {
        syscall(SYS_tgkill, process, waiter_tid, SIGUSR1);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    });
    const auto start = Clock::now();
    until([&] { return shared.holds.load(std::memory_order_acquire) != 0; },
          "EINTR cond waiter did not time out", 2000);
    const auto elapsed = elapsed_ms(start);
    stop.store(true, std::memory_order_release);
    storm.join();
    need(shared.notified.load(std::memory_order_acquire) == 0,
         "EINTR cond wait reported notification");
    need(elapsed < 1500,
         "EINTR restarted the full cond timeout: " + std::to_string(elapsed));
    shared.release.store(1, std::memory_order_release);
    need(lang_thread_wait_v1(waiter) == 0, "EINTR cond waiter join failed");
    lang_handle_release_v1(waiter);
    lang_handle_release_v1(condition);
    lang_handle_release_v1(mutex);
  }
}

void panic_cond_invalid_timeout() {
  auto *mutex = lang_mutex_new_v1();
  auto *condition = lang_condvar_new_v1();
  lang_mutex_lock_v1(mutex);
  (void)lang_condvar_wait_for_v1(condition, mutex, -2);
  _exit(0);
}
void panic_cond_not_owned() {
  auto *mutex = lang_mutex_new_v1();
  auto *condition = lang_condvar_new_v1();
  (void)lang_condvar_wait_for_v1(condition, mutex, 0);
  _exit(0);
}

} // namespace

int main() {
  try {
    test_thread_timeouts();
    test_condvar_timeouts();
    expect_handle_panic("cond-invalid-timeout", panic_cond_invalid_timeout);
    expect_handle_panic("cond-not-owned", panic_cond_not_owned);
    std::cout << "passed: thread wait_for timeout/0/-1/selfjoin/huge/EINTR/"
                 "concurrent joins and condvar timeout/notify/relock/EINTR "
                 "with misuse Panic\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
