// Standalone C++ harness for the hosted thread runtime.  It links the hosted
// archive through a normal CRT and exercises the same handle/context ABI used
// by generated language code.  The fixture itself is not generated Woozy code;
// it deliberately calls libc, host TLS and OpenSSL from both main and runtime
// threads.
#include "../../../runtime/native/thread.hpp"
#include "../../../runtime/native/alloc.hpp"
#include "../../../runtime/native/os.hpp"
#include "../../../runtime/hosted/thread.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <limits>
#include <openssl/evp.h>
#include <pthread.h>
#include <sched.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
constexpr unsigned kWorkers = 4;
constexpr unsigned kJoiners = 8;
constexpr unsigned kRetainers = 8;

thread_local int g_tls = 0;

struct WaitProbe {
  bool observe = false, inject = false, saturated_clock = false;
  unsigned calls = 0;
  timespec first{};
  bool changed_deadline = false;
};
thread_local WaitProbe g_wait_probe;
std::atomic<unsigned> g_nonmonotonic_conditions{0};

void need(bool value, const std::string &message) {
  if (!value) throw std::runtime_error(message);
}

void until(const auto &predicate, const std::string &message) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!predicate()) {
    need(std::chrono::steady_clock::now() < end, message);
    sched_yield();
  }
}

uint64_t fs_base() {
  uint64_t value = 0;
  __asm__ volatile("movq %%fs:0, %0" : "=r"(value));
  return value;
}

uint64_t canary() {
  uint64_t value = 0;
  __asm__ volatile("movq %%fs:0x28, %0" : "=r"(value));
  return value;
}

int thread_count() {
  DIR *directory = opendir("/proc/self/task");
  need(directory != nullptr, "opendir(/proc/self/task) failed");
  int count = 0;
  while (dirent *entry = readdir(directory)) {
    if (std::strcmp(entry->d_name, ".") && std::strcmp(entry->d_name, ".."))
      ++count;
  }
  closedir(directory);
  return count;
}

// The hosted objects allocate their state/context through the existing native
// allocator.  Wrapping free lets the detached test prove that the final state
// allocation was released even though nobody joined the handle.
constexpr size_t kFreedCapacity = 8192;
uintptr_t g_freed[kFreedCapacity];
std::atomic<size_t> g_freed_count{0};

bool was_freed(uintptr_t address) {
  const size_t count = g_freed_count.load(std::memory_order_acquire);
  for (size_t i = 0; i < count && i < kFreedCapacity; ++i)
    if (__atomic_load_n(&g_freed[i], __ATOMIC_ACQUIRE) == address) return true;
  return false;
}

struct Observation {
  uint64_t fs = 0;
  uint64_t stack = 0;
  uint64_t canary = 0;
};

struct Counts {
  std::atomic<uint64_t> copies{0};
  std::atomic<uint64_t> drops{0};
};

struct Shared {
  std::atomic<uint32_t> started{0};
  std::atomic<uint32_t> gate{0};
  std::atomic<uint32_t> errors{0};
  std::atomic<int64_t> detached_result{0};
  Counts counts;
};

void fail(Shared *shared, const char *message) {
  std::fprintf(stderr, "hosted worker error: %s\n", message);
  shared->errors.fetch_add(1, std::memory_order_release);
}

struct Work {
  LangVec *values;
  int64_t bonus;
  uint32_t id;
  Shared *shared;
  Observation *observation;
};

void copy_work(void *pointer) {
  auto *work = static_cast<Work *>(pointer);
  lang_vec_retain_v1(work->values);
  work->shared->counts.copies.fetch_add(1, std::memory_order_relaxed);
}

void drop_work(void *pointer) {
  auto *work = static_cast<Work *>(pointer);
  lang_vec_release_v1(work->values);
  work->shared->counts.drops.fetch_add(1, std::memory_order_release);
}

constexpr unsigned char kExpectedSha256[kWorkers][32] = {
    {0xa9,0xe3,0xce,0xa0,0xf4,0x2c,0x2a,0xec,0xf7,0x0d,0x45,0xc0,0x35,0xcf,0x1d,0x72,
     0xfa,0x02,0xbc,0xe0,0x16,0xe9,0x5b,0xc9,0xd0,0xac,0x9a,0x85,0x91,0xec,0xdc,0xed},
    {0x68,0x73,0x37,0x6b,0x69,0x9f,0x3a,0x99,0xa7,0x0f,0x89,0x5d,0x3d,0xc0,0xb0,0x14,
     0x77,0xc9,0x7e,0xce,0x25,0x88,0x64,0xdd,0xb1,0x33,0xa2,0x8b,0xa2,0xf0,0xb2,0xd6},
    {0x1f,0x28,0x66,0xf3,0xf6,0x48,0x15,0x7b,0xe9,0x03,0x66,0x02,0x1b,0x22,0xce,0xcf,
     0x79,0x6a,0xf5,0xc2,0xf8,0x80,0xea,0xff,0x51,0x4f,0x32,0x55,0xb7,0x11,0xf5,0xe0},
    {0x63,0x67,0x9c,0x4c,0xf2,0x11,0xe8,0xf6,0xe4,0x8d,0x48,0x74,0xe2,0x1d,0x17,0x2d,
     0xa8,0xbd,0x4a,0xa9,0xe2,0x69,0x33,0x3a,0x4d,0x72,0xd0,0xda,0xc9,0xb1,0x2b,0x8c}};

int64_t work_entry(void *pointer) noexcept {
  const Work work = *static_cast<Work *>(pointer);
  Shared *shared = work.shared;
  g_tls = 1000 + static_cast<int>(work.id);
  thread_local int function_tls = 0;
  function_tls = 2000 + static_cast<int>(work.id);

  char text[32];
  const int text_length =
      std::snprintf(text, sizeof(text), "woozy-hosted-%u", work.id);
  if (text_length <= 0 || static_cast<size_t>(text_length) >= sizeof(text))
    fail(shared, "snprintf failed");
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_length = 0;
  if (EVP_Digest(text, static_cast<size_t>(text_length), digest, &digest_length,
                 EVP_sha256(), nullptr) != 1 ||
      digest_length != 32 ||
      std::memcmp(digest, kExpectedSha256[work.id], 32) != 0)
    fail(shared, "OpenSSL SHA256 failed");

  const char *environment = std::getenv("HOSTED_TEST_ENV");
  if (!environment || std::strcmp(environment, "hosted-env-value") != 0)
    fail(shared, "getenv failed");

  // The existing freestanding OS object remains in the hosted archive and must
  // keep working from a pthread-managed thread.
  const int32_t null_fd = static_cast<int32_t>(
      lang_os_open_v1("/dev/null", 9, 1 /* O_WRONLY */, 0));
  if (null_fd >= 0) {
    const char byte = 'x';
    if (lang_os_write_v1(null_fd, &byte, 1) != 1)
      fail(shared, "lang_os_write_v1 failed");
    if (lang_os_close_v1(null_fd) != 0)
      fail(shared, "lang_os_close_v1 failed");
  } else {
    fail(shared, "lang_os_open_v1 failed");
  }
  if (lang_os_getpid_v1() != static_cast<int64_t>(getpid()))
    fail(shared, "lang_os_getpid_v1 failed");

  for (int i = 0; i < 512; ++i) {
    const size_t size = 32 + static_cast<size_t>(i % 97);
    void *allocation = std::malloc(size);
    if (!allocation) {
      fail(shared, "malloc failed");
      break;
    }
    std::memset(allocation, static_cast<int>(work.id), size);
    std::free(allocation);
  }

  if (work.observation) {
    uint64_t local = 0;
    work.observation->fs = fs_base();
    work.observation->stack = reinterpret_cast<uint64_t>(&local);
    work.observation->canary = canary();
  }
  // Set a unique errno only after the libc/OpenSSL work above; the other
  // workers are held at the gate so any cross-thread TLS reuse is visible.
  errno = 3000 + static_cast<int>(work.id);
  shared->started.fetch_add(1, std::memory_order_release);
  while (!shared->gate.load(std::memory_order_acquire)) sched_yield();

  if (g_tls != 1000 + static_cast<int>(work.id) ||
      function_tls != 2000 + static_cast<int>(work.id) ||
      errno != 3000 + static_cast<int>(work.id))
    fail(shared, "thread-local state was polluted by another thread");

  int64_t sum = 0;
  for (uint64_t i = 0; i < work.values->length; ++i)
    sum += *static_cast<const int64_t *>(lang_vec_at_v1(work.values, i));
  shared->detached_result.store(sum + work.bonus, std::memory_order_release);
  return sum + work.bonus;
}

struct JoinInput {
  LangHandle *target;
  int64_t extra;
};

void copy_join(void *pointer) {
  lang_handle_retain_v1(static_cast<JoinInput *>(pointer)->target);
}
void drop_join(void *pointer) {
  lang_handle_release_v1(static_cast<JoinInput *>(pointer)->target);
}
int64_t join_entry(void *pointer) noexcept {
  const auto input = *static_cast<JoinInput *>(pointer);
  const int64_t status = lang_thread_wait_v1(input.target);
  return status ? status : lang_thread_result_v1(input.target) + input.extra;
}

struct SelfInput {
  LangHandle **slot;
};
int64_t self_entry(void *pointer) noexcept {
  auto **target = static_cast<SelfInput *>(pointer)->slot;
  LangHandle *handle = nullptr;
  while (!(handle = __atomic_load_n(target, __ATOMIC_ACQUIRE))) sched_yield();
  return lang_thread_wait_v1(handle);
}

struct RetainInput {
  LangHandle *target;
};
int64_t retain_entry(void *pointer) noexcept {
  auto *handle = static_cast<RetainInput *>(pointer)->target;
  for (size_t i = 0; i < 20000; ++i) {
    lang_handle_retain_v1(handle);
    lang_handle_release_v1(handle);
  }
  return 1;
}

void no_destroy(LangHandle *) noexcept {}
std::atomic<uint64_t> g_destroyed{0};
void count_destroy(LangHandle *) noexcept {
  g_destroyed.fetch_add(1, std::memory_order_release);
}

void expect_panic(const char *name, void (*operation)()) {
  int descriptors[2];
  need(pipe(descriptors) == 0, "pipe failed");
  const pid_t child = fork();
  need(child >= 0, "fork failed");
  if (!child) {
    close(descriptors[0]);
    dup2(descriptors[1], 2);
    close(descriptors[1]);
    operation();
    _exit(77);
  }
  close(descriptors[1]);
  std::string error;
  char bytes[256];
  for (ssize_t size; (size = read(descriptors[0], bytes, sizeof(bytes))) > 0;)
    error.append(bytes, static_cast<size_t>(size));
  close(descriptors[0]);
  int status = 0;
  need(waitpid(child, &status, 0) == child, "waitpid failed");
  need(WIFEXITED(status) && WEXITSTATUS(status) == 101,
       std::string(name) + " did not exit with the Panic status");
  need(error.find("LANG_NATIVE_PANIC:v1:runtime-handle-misuse:") !=
           std::string::npos,
       std::string(name) + " did not emit a Panic record: " + error);
}

LangVec *make_values() {
  LangVec *values = lang_vec_new_v1(3, sizeof(int64_t), nullptr, nullptr);
  need(values != nullptr, "lang_vec_new_v1 failed");
  for (int64_t i = 1; i <= 3; ++i) lang_vec_init_v1(values, &i);
  return values;
}

int64_t expected_sum() { return 1 + 2 + 3; }

void check_main_thread_libc() {
  g_tls = 1;
  errno = 111;
  char text[64];
  need(std::snprintf(text, sizeof(text), "%s", "woozy-hosted") > 0,
       "main snprintf failed");
  void *allocation = std::malloc(513);
  need(allocation != nullptr, "main malloc failed");
  std::free(allocation);
  const char *environment = std::getenv("HOSTED_TEST_ENV");
  need(environment && std::strcmp(environment, "hosted-env-value") == 0,
       "main getenv failed");
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int length = 0;
  need(EVP_Digest("woozy-hosted-0", 14, digest, &length, EVP_sha256(),
                  nullptr) == 1 &&
           length == 32 &&
           std::memcmp(digest, kExpectedSha256[0], 32) == 0,
       "main OpenSSL SHA256 failed");
  errno = 111;
  need(g_tls == 1 && errno == 111, "main thread-local state changed");
}

struct WaitGate {
  std::atomic<bool> started{false}, release{false};
};
int64_t gate_entry(void *pointer) noexcept {
  auto *gate = *static_cast<WaitGate **>(pointer);
  gate->started.store(true, std::memory_order_release);
  while (!gate->release.load(std::memory_order_acquire)) sched_yield();
  return 42;
}
LangHandle *wait_target(WaitGate &gate) {
  auto *pointer = &gate;
  auto *handle = lang_thread_spawn_v1(gate_entry, &pointer, sizeof(pointer),
                                     nullptr, nullptr);
  need(lang_thread_error_v1(handle) == 0, "timed target creation failed");
  until([&] { return gate.started.load(std::memory_order_acquire); },
        "timed target did not start");
  return handle;
}

struct TimedJoin {
  LangHandle *target;
  int64_t timeout;
  std::atomic<unsigned> *ready;
};
void copy_timed_join(void *pointer) {
  lang_handle_retain_v1(static_cast<TimedJoin *>(pointer)->target);
}
void drop_timed_join(void *pointer) {
  lang_handle_release_v1(static_cast<TimedJoin *>(pointer)->target);
}
int64_t timed_join_entry(void *pointer) noexcept {
  const auto input = *static_cast<TimedJoin *>(pointer);
  input.ready->fetch_add(1, std::memory_order_release);
  const int64_t status = lang_thread_wait_for_v1(input.target, input.timeout);
  return status ? status : lang_thread_result_v1(input.target);
}
struct TimedSelf {
  LangHandle **slot;
  int64_t timeout;
};
int64_t timed_self_entry(void *pointer) noexcept {
  const auto input = *static_cast<TimedSelf *>(pointer);
  LangHandle *handle = nullptr;
  while (!(handle = __atomic_load_n(input.slot, __ATOMIC_ACQUIRE))) sched_yield();
  return lang_thread_wait_for_v1(handle, input.timeout);
}

void check_timed_waits() {
  constexpr int64_t huge = std::numeric_limits<int64_t>::max();
  WaitGate gate;
  auto *target = wait_target(gate);
  need(lang_thread_wait_for_v1(target, -2) == -22, "invalid timeout accepted");
  need(lang_thread_wait_for_v1(target, 0) == -110, "pending zero poll succeeded");
  auto begin = std::chrono::steady_clock::now();
  need(lang_thread_wait_for_v1(target, 40000000) == -110,
       "finite pending wait did not time out");
  auto elapsed = std::chrono::steady_clock::now() - begin;
  need(elapsed >= std::chrono::milliseconds(30) &&
           elapsed < std::chrono::seconds(2),
       "finite wait duration is outside its deadline");

  g_wait_probe = {};
  g_wait_probe.observe = g_wait_probe.inject = true;
  begin = std::chrono::steady_clock::now();
  const auto retried = lang_thread_wait_for_v1(target, 40000000);
  elapsed = std::chrono::steady_clock::now() - begin;
  const auto retry_probe = g_wait_probe;
  g_wait_probe = {};
  need(retried == -110 && retry_probe.calls >= 2 &&
           !retry_probe.changed_deadline,
       "EINTR/spurious retry changed the fixed deadline");
  need(elapsed >= std::chrono::milliseconds(30) && elapsed < std::chrono::seconds(2),
       "EINTR/spurious retry did not honor the deadline");

  const pid_t child = fork();
  need(child >= 0, "timed wait fork failed");
  if (!child) {
    alarm(3);
    const bool ok = lang_thread_wait_for_v1(target, 0) == -3 &&
                    lang_thread_wait_for_v1(target, 1000000) == -3 &&
                    lang_thread_wait_for_v1(target, -1) == -3 &&
                    lang_thread_wait_for_v1(target, -2) == -22;
    _exit(ok ? 0 : 61);
  }
  int child_status = 0;
  need(waitpid(child, &child_status, 0) == child && WIFEXITED(child_status) &&
           WEXITSTATUS(child_status) == 0,
       "inherited timed wait did not return ESRCH before locking");

  std::atomic<unsigned> ready{0};
  const int64_t timeouts[] = {20000000, 20000000, 2000000000, huge};
  LangHandle *waiters[4];
  for (unsigned i = 0; i < 4; ++i) {
    TimedJoin input{target, timeouts[i], &ready};
    waiters[i] = lang_thread_spawn_v1(timed_join_entry, &input, sizeof(input),
                                     copy_timed_join, drop_timed_join);
    need(lang_thread_error_v1(waiters[i]) == 0, "timed waiter creation failed");
  }
  until([&] { return ready.load(std::memory_order_acquire) == 4; },
        "timed waiters did not enter");
  std::thread release([&] {
    usleep(60000);
    gate.release.store(true, std::memory_order_release);
  });
  g_wait_probe = {};
  g_wait_probe.observe = true;
  const auto huge_status = lang_thread_wait_for_v1(target, huge);
  const auto huge_probe = g_wait_probe;
  g_wait_probe = {};
  release.join();
  need(huge_status == 0 && huge_probe.calls > 0 && huge_probe.first.tv_sec > 0,
       "huge timeout overflowed or did not wait for completion");
  for (unsigned i = 0; i < 4; ++i) {
    need(lang_thread_wait_v1(waiters[i]) == 0, "timed waiter join failed");
    need(lang_thread_result_v1(waiters[i]) == (i < 2 ? -110 : 42),
         "concurrent timed waiters saw inconsistent completion");
    lang_handle_release_v1(waiters[i]);
  }
  need(lang_thread_result_v1(target) == 42 &&
           lang_thread_wait_for_v1(target, 0) == 0 &&
           lang_thread_wait_for_v1(target, 1) == 0 &&
           lang_thread_wait_for_v1(target, huge) == 0 &&
           lang_thread_wait_for_v1(target, -1) == 0 &&
           lang_thread_wait_for_v1(target, -2) == -22,
       "completed timed wait returned the wrong status");
  lang_handle_release_v1(target);

  for (bool saturate : {false, true}) {
    WaitGate pending;
    auto *handle = wait_target(pending);
    std::thread release_pending([&] {
      usleep(20000);
      pending.release.store(true, std::memory_order_release);
    });
    g_wait_probe = {};
    g_wait_probe.observe = g_wait_probe.saturated_clock = saturate;
    const auto status = lang_thread_wait_for_v1(handle, saturate ? huge : -1);
    const auto probe = g_wait_probe;
    g_wait_probe = {};
    release_pending.join();
    need(status == 0 && lang_thread_result_v1(handle) == 42,
         "infinite/saturated timed wait failed");
    if (saturate)
      need(probe.calls > 0 && probe.first.tv_sec == std::numeric_limits<time_t>::max() &&
               probe.first.tv_nsec == 999999999 && !probe.changed_deadline,
           "deadline did not saturate at the largest timespec");
    lang_handle_release_v1(handle);
  }

  const int64_t self_timeouts[] = {-2, -1, 0, 1000000, huge};
  for (int64_t timeout : self_timeouts) {
    LangHandle *self = nullptr;
    TimedSelf input{&self, timeout};
    auto *handle = lang_thread_spawn_v1(timed_self_entry, &input, sizeof(input),
                                       nullptr, nullptr);
    need(lang_thread_error_v1(handle) == 0, "timed self waiter creation failed");
    __atomic_store_n(&self, handle, __ATOMIC_RELEASE);
    need(lang_thread_wait_for_v1(handle, 2000000000) == 0 &&
             lang_thread_result_v1(handle) == (timeout < -1 ? -22 : -35),
         "timed self wait did not report EDEADLK/invalid timeout");
    lang_handle_release_v1(handle);
  }
  need(g_nonmonotonic_conditions.load() == 0,
       "hosted thread condition variable did not use CLOCK_MONOTONIC");
  std::puts("passed: monotonic wait_for, poll/deadline/retry/saturation, concurrent waiters, self/fork errors");
}
} // namespace

extern "C" int __real_pthread_cond_init(pthread_cond_t *, const pthread_condattr_t *);
extern "C" int __wrap_pthread_cond_init(pthread_cond_t *condition,
                                         const pthread_condattr_t *attributes) {
  clockid_t clock = CLOCK_REALTIME;
  if (!attributes || pthread_condattr_getclock(attributes, &clock) != 0 ||
      clock != CLOCK_MONOTONIC)
    g_nonmonotonic_conditions.fetch_add(1);
  return __real_pthread_cond_init(condition, attributes);
}
extern "C" int __real_clock_gettime(clockid_t, timespec *);
extern "C" int __wrap_clock_gettime(clockid_t clock, timespec *value) {
  if (clock == CLOCK_MONOTONIC && g_wait_probe.saturated_clock) {
    *value = {std::numeric_limits<time_t>::max() - 1, 900000000};
    return 0;
  }
  return __real_clock_gettime(clock, value);
}
extern "C" int __real_pthread_cond_timedwait(pthread_cond_t *, pthread_mutex_t *,
                                             const timespec *);
extern "C" int __wrap_pthread_cond_timedwait(pthread_cond_t *condition,
                                              pthread_mutex_t *mutex,
                                              const timespec *deadline) {
  auto &probe = g_wait_probe;
  if (probe.observe) {
    if (!probe.calls) probe.first = *deadline;
    else if (probe.first.tv_sec != deadline->tv_sec ||
             probe.first.tv_nsec != deadline->tv_nsec)
      probe.changed_deadline = true;
    ++probe.calls;
    if (probe.inject && probe.calls <= 32) {
      usleep(8000);
      return probe.calls % 2 ? EINTR : 0;
    }
  }
  return __real_pthread_cond_timedwait(condition, mutex, deadline);
}

extern "C" void *__real_lang_alloc_v1(uint64_t, uint64_t);
extern "C" void __real_lang_free_v1(void *);
extern "C" void *__wrap_lang_alloc_v1(uint64_t size, uint64_t alignment) {
  return __real_lang_alloc_v1(size, alignment);
}
extern "C" void __wrap_lang_free_v1(void *pointer) {
  if (pointer) {
    const size_t index = g_freed_count.fetch_add(1, std::memory_order_acq_rel);
    if (index < kFreedCapacity)
      __atomic_store_n(&g_freed[index], reinterpret_cast<uintptr_t>(pointer),
                       __ATOMIC_RELEASE);
  }
  __real_lang_free_v1(pointer);
}

int main() {
  try {
    check_main_thread_libc();
    const uint64_t parent_fs = fs_base();
    const uint64_t parent_canary = canary();
    need(parent_fs != 0 && parent_canary != 0, "host TLS is unavailable");
    g_tls = 1;
    errno = 111;

    // Context copy/drop and the existing Vec allocator are exercised through
    // the same ABI shape generated language code uses.
    LangVec *values = make_values();
    Shared shared;
    Observation observations[kWorkers];
    LangHandle *threads[kWorkers];
    for (unsigned i = 0; i < kWorkers; ++i) {
      Work work{values, 100 + static_cast<int64_t>(i), i, &shared,
                &observations[i]};
      threads[i] = lang_thread_spawn_v1(work_entry, &work, sizeof(work),
                                        copy_work, drop_work);
      need(lang_thread_error_v1(threads[i]) == 0, "worker creation failed");
    }
    until([&] { return shared.started.load(std::memory_order_acquire) == kWorkers; },
          "workers did not start");
    need(g_tls == 1 && errno == 111, "main TLS was changed by workers");
    for (unsigned i = 0; i < kWorkers; ++i) {
      need(observations[i].fs != parent_fs && observations[i].canary == parent_canary,
           "worker did not use distinct libc TLS / canary");
      need(observations[i].stack != 0, "worker stack observation is invalid");
      for (unsigned j = 0; j < i; ++j) {
        need(observations[i].fs != observations[j].fs,
             "workers share a host TLS block");
        need(observations[i].stack != observations[j].stack,
             "workers share a host stack");
      }
    }
    shared.gate.store(1, std::memory_order_release);
    for (unsigned i = 0; i < kWorkers; ++i) {
      need(lang_thread_wait_v1(threads[i]) == 0, "worker wait failed");
      need(lang_thread_result_v1(threads[i]) ==
               expected_sum() + 100 + static_cast<int64_t>(i),
           "worker result is invalid");
      need(lang_thread_wait_v1(threads[i]) == 0, "repeat worker wait failed");
      lang_handle_release_v1(threads[i]);
    }
    need(shared.errors.load(std::memory_order_acquire) == 0,
         "worker reported a libc/TLS error");
    need(shared.counts.copies.load(std::memory_order_acquire) == kWorkers &&
             shared.counts.drops.load(std::memory_order_acquire) == kWorkers,
         "worker context copy/drop count is invalid");
    need(__atomic_load_n(&values->refs, __ATOMIC_ACQUIRE) == 1,
         "worker context leaked Vec references");
    const uintptr_t values_address = reinterpret_cast<uintptr_t>(values);
    lang_vec_release_v1(values);
    until([&] { return was_freed(values_address); }, "worker Vec was not freed");

    // Concurrent aliases of one thread handle must all see the same result.
    Shared alias_shared;
    LangVec *alias_values = make_values();
    Work alias_work{alias_values, 7, 0, &alias_shared, nullptr};
    LangHandle *target = lang_thread_spawn_v1(
        work_entry, &alias_work, sizeof(alias_work), copy_work, drop_work);
    need(lang_thread_error_v1(target) == 0, "alias target creation failed");
    const uintptr_t target_address = reinterpret_cast<uintptr_t>(target);
    LangHandle *joiners[kJoiners];
    for (unsigned i = 0; i < kJoiners; ++i) {
      JoinInput input{target, static_cast<int64_t>(i)};
      joiners[i] = lang_thread_spawn_v1(join_entry, &input, sizeof(input),
                                        copy_join, drop_join);
      need(lang_thread_error_v1(joiners[i]) == 0, "joiner creation failed");
    }
    lang_handle_release_v1(target);
    alias_shared.gate.store(1, std::memory_order_release);
    for (unsigned i = 0; i < kJoiners; ++i) {
      need(lang_thread_wait_v1(joiners[i]) == 0,
           "concurrent alias join failed");
      need(lang_thread_result_v1(joiners[i]) ==
               expected_sum() + 7 + static_cast<int64_t>(i),
           "concurrent alias result is inconsistent");
      lang_handle_release_v1(joiners[i]);
    }
    need(alias_shared.counts.drops.load(std::memory_order_acquire) == 1,
         "alias target context drop count is invalid");
    until([&] { return was_freed(target_address); },
          "alias target state was not freed");
    lang_vec_release_v1(alias_values);

    // A manager thread waiting on itself reports EDEADLK rather than hanging.
    LangHandle *self = nullptr;
    auto **self_slot = &self;
    SelfInput self_input{self_slot};
    LangHandle *self_thread =
        lang_thread_spawn_v1(self_entry, &self_input, sizeof(self_input),
                             nullptr, nullptr);
    need(lang_thread_error_v1(self_thread) == 0, "self-join creation failed");
    __atomic_store_n(&self, self_thread, __ATOMIC_RELEASE);
    need(lang_thread_wait_v1(self_thread) == 0, "self-join manager wait failed");
    need(lang_thread_result_v1(self_thread) == -35,
         "self-join did not report EDEADLK");
    lang_handle_release_v1(self_thread);

    // Handle ARC updates from many runtime threads.
    LangHandle sample{1, LangHandleKind::atomic_i64, count_destroy};
    LangHandle *retainers[kRetainers];
    for (auto &retainer : retainers) {
      RetainInput input{&sample};
      retainer = lang_thread_spawn_v1(retain_entry, &input, sizeof(input),
                                      nullptr, nullptr);
      need(lang_thread_error_v1(retainer) == 0, "retainer creation failed");
    }
    for (auto *retainer : retainers) {
      need(lang_thread_wait_v1(retainer) == 0, "retainer wait failed");
      lang_handle_release_v1(retainer);
    }
    need(__atomic_load_n(&sample.refs, __ATOMIC_ACQUIRE) == 1,
         "handle ARC lost updates");
    lang_handle_release_v1(&sample);
    need(g_destroyed.load(std::memory_order_acquire) == 1,
         "handle destroy ran more than once");

    // Releasing the final handle without joining must still let the entry,
    // context drop and pthread cleanup complete.
    Shared detached_shared;
    LangVec *detached_values = make_values();
    Work detached_work{detached_values, 42, 3, &detached_shared, nullptr};
    LangHandle *detached = lang_thread_spawn_v1(
        work_entry, &detached_work, sizeof(detached_work), copy_work, drop_work);
    need(lang_thread_error_v1(detached) == 0, "detached creation failed");
    const uintptr_t detached_address = reinterpret_cast<uintptr_t>(detached);
    lang_handle_release_v1(detached);
    detached_shared.gate.store(1, std::memory_order_release);
    until([&] {
      return detached_shared.counts.drops.load(std::memory_order_acquire) == 1 &&
             detached_shared.detached_result.load(std::memory_order_acquire) ==
                 expected_sum() + 42 &&
             was_freed(detached_address) && thread_count() == 1;
    }, "last-handle cleanup did not finish and release the pthread state");
    lang_vec_release_v1(detached_values);

    check_timed_waits();

    expect_panic("wrong handle kind", [] {
      LangHandle handle{1, LangHandleKind::mutex, no_destroy};
      lang_thread_wait_v1(&handle);
    });
    expect_panic("zero-reference retain", [] {
      LangHandle handle{0, LangHandleKind::atomic_i64, no_destroy};
      lang_handle_retain_v1(&handle);
    });
    expect_panic("hosted unmap epilogue", [] {
      lang_thread_unmap_exit_v1(nullptr, 0);
    });

    std::printf("passed: pthread TLS/libc/OpenSSL, Vec context, alias joins, "
                "self-join, detached cleanup and Panic paths\n");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "hosted_thread_test: %s\n", error.what());
    return 1;
  }
}
