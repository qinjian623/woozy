#include "../native/process.hpp"
#include "../native/darwin.hpp"
#include "../native/gc_program.hpp"
#include "posix.hpp"

#include <pthread.h>
#include <time.h>

namespace {
namespace darwin = lang::native::darwin;
using lang::native::gc_program::SafeGuard;
constexpr auto &signals = darwin::linux_signal_to_darwin;
constexpr unsigned kSignalCount = sizeof(signals) / sizeof(signals[0]);

uint32_t signal_lock;
unsigned blocking_waiters[kSignalCount];

class SignalGuard {
 public:
  SignalGuard() noexcept {
    while (__atomic_exchange_n(&signal_lock, 1u, __ATOMIC_ACQUIRE)) {
      while (__atomic_load_n(&signal_lock, __ATOMIC_RELAXED)) darwin::cpu_pause();
    }
  }
  ~SignalGuard() noexcept { __atomic_store_n(&signal_lock, 0u, __ATOMIC_RELEASE); }
  SignalGuard(const SignalGuard &) = delete;
  SignalGuard &operator=(const SignalGuard &) = delete;
};

constexpr uint64_t bit(unsigned n) noexcept { return uint64_t{1} << (n - 1); }

int64_t to_host(uint64_t mask, sigset_t &set) noexcept {
  sigemptyset(&set);
  for (unsigned n = 1; n <= 64; ++n) {
    if (!(mask & bit(n))) continue;
    if (n >= kSignalCount || !signals[n]) return -95;
    sigaddset(&set, signals[n]);
  }
  return 0;
}

uint64_t from_host(const sigset_t &set) noexcept {
  uint64_t mask = 0;
  for (unsigned n = 1; n < kSignalCount; ++n) {
    if (signals[n] && sigismember(&set, signals[n]) == 1) mask |= bit(n);
  }
  return mask;
}

struct BlockingWait {
  explicit BlockingWait(uint64_t mask) noexcept : mask_(mask) {
    SignalGuard guard;
    for (unsigned n = 1; n < kSignalCount; ++n) {
      if (mask_ & bit(n)) ++blocking_waiters[n];
    }
  }
  ~BlockingWait() {
    SignalGuard guard;
    for (unsigned n = 1; n < kSignalCount; ++n) {
      if (mask_ & bit(n)) --blocking_waiters[n];
    }
  }
  BlockingWait(const BlockingWait &) = delete;
  BlockingWait &operator=(const BlockingWait &) = delete;

 private:
  uint64_t mask_;
};

int64_t publish(LangSignalInfo *out, int received) noexcept {
  for (unsigned n = 1; n < kSignalCount; ++n) {
    if (signals[n] != received) continue;
    // Darwin sigwait supplies no siginfo. Unavailable fields are zero.
    *out = {static_cast<int32_t>(n), 0, 0, 0, 0, 0, 0};
    return 0;
  }
  return -95;
}

int64_t try_wait(LangSignalInfo *out, uint64_t mask) noexcept {
  // Serialize our waiters and disposition changes between checking pending
  // and consuming it. Blocking sigwait callers own their whole mask until
  // the call returns and can consume a pending signal from another thread.
  SignalGuard guard;
  sigset_t pending;
  if (sigpending(&pending) < 0) return darwin::error(errno);
  auto ready = from_host(pending) & mask;
  for (unsigned n = 1; n < kSignalCount; ++n) {
    if (!(ready & bit(n)) || blocking_waiters[n] != 0) continue;
    sigset_t one;
    sigemptyset(&one);
    sigaddset(&one, signals[n]);
    int received;
    const int status = sigwait(&one, &received);
    return status == 0 ? publish(out, received) : darwin::error(status);
  }
  return -11;
}
} // namespace

extern "C" void lang_signal_fork_child_v1() noexcept {
  for (auto &count : blocking_waiters) count = 0;
  __atomic_store_n(&signal_lock, 0u, __ATOMIC_RELAXED);
}

extern "C" int64_t lang_signal_mask_v1(uint64_t *old, int32_t how, uint64_t mask) noexcept {
  if (!old) return -14;
  constexpr int operations[] = {SIG_BLOCK, SIG_UNBLOCK, SIG_SETMASK};
  if (how < 0 || how > 2) return -22;
  sigset_t new_mask;
  const auto converted = to_host(mask, new_mask);
  if (converted < 0) return converted;
  if (how == 2) {
    sigset_t before;
    const int status = pthread_sigmask(SIG_SETMASK, nullptr, &before);
    if (status != 0) return darwin::error(status);
    sigset_t supported;
    sigemptyset(&supported);
    for (const int signal : signals) if (signal) sigaddset(&supported, signal);
    for (int signal = 1; signal < NSIG; ++signal) {
      if (sigismember(&supported, signal) == 0 && sigismember(&before, signal) == 1)
        sigaddset(&new_mask, signal);
    }
  }
  sigset_t previous;
  const int status = pthread_sigmask(operations[how], &new_mask, &previous);
  if (status != 0) return darwin::error(status);
  *old = from_host(previous);
  return 0;
}

extern "C" int64_t lang_signal_pending_v1(uint64_t *out) noexcept {
  if (!out) return -14;
  sigset_t pending;
  if (sigpending(&pending) < 0) return darwin::error(errno);
  *out = from_host(pending);
  return 0;
}

extern "C" int64_t lang_signal_wait_v1(LangSignalInfo *out, uint64_t mask,
                                        int64_t timeout_ns) noexcept {
  if (!out) return -14;
  if (timeout_ns < -1) return -22;
  mask &= ~(bit(9) | bit(19)); // SIGKILL and SIGSTOP cannot be blocked.
  // A stop signal can clear a pending SIGCONT (and vice versa) in the
  // kernel between sigpending and sigwait, despite our user-space lock.
  constexpr auto job_control = bit(18) | bit(20) | bit(21) | bit(22);
  if (timeout_ns >= 0 && (mask & job_control)) return -95;
  sigset_t set;
  const auto converted = to_host(mask, set);
  if (converted < 0) return converted;
  sigset_t blocked;
  const int mask_status = pthread_sigmask(SIG_SETMASK, nullptr, &blocked);
  if (mask_status != 0) return darwin::error(mask_status);
  if (mask & ~from_host(blocked)) return -22;
  if (timeout_ns == -1) {
    if (!mask) return -22;
    BlockingWait waiter(mask);
    SafeGuard safe;
    int received;
    const int status = sigwait(&set, &received);
    return status == 0 ? publish(out, received) : darwin::error(status);
  }

  // shortcut: Darwin has no timed sigwait, so bounded polling consumes only
  // the calling thread's pending signals. Use kernel timed waits if added.
  const auto start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  for (;;) {
    const auto status = try_wait(out, mask);
    if (status != -11) return status;
    const auto elapsed = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start;
    if (elapsed >= static_cast<uint64_t>(timeout_ns)) return -11;
    const auto remaining = static_cast<uint64_t>(timeout_ns) - elapsed;
    const timespec delay{0, static_cast<long>(remaining < 1000000 ? remaining : 1000000)};
    SafeGuard safe;
    if (nanosleep(&delay, nullptr) < 0) return darwin::error(errno);
  }
}

extern "C" int64_t lang_signal_disposition_v1(int32_t number, int32_t action) noexcept {
  if (number < 1 || number > 64 || number == 9 || number == 19 || (action != 0 && action != 1))
    return -22;
  if (static_cast<unsigned>(number) >= kSignalCount || !signals[number]) return -95;
  struct sigaction value{};
  value.sa_handler = action ? SIG_IGN : SIG_DFL;
  sigemptyset(&value.sa_mask);
  SignalGuard guard;
  return darwin::result(sigaction(signals[number], &value, nullptr));
}
