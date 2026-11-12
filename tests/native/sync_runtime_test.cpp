// Host test harness for the freestanding native synchronization runtime.
//
// std::thread and std::barrier-free host synchronization are used only to
// create/join test threads and to capture child-process diagnostics. The
// runtime under test (runtime/native/sync.cpp) uses raw Linux syscalls and
// atomic builtins only.
//
// Handle lifetime and freeing use the real runtime, including its atomic ARC.

#include "../../runtime/native/sync.hpp"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__x86_64__) && defined(__linux__)
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

namespace {

std::atomic<uint64_t> g_created{0};
std::atomic<uint64_t> g_destroyed{0};

extern "C" void __real_lang_free_v1(void *) noexcept;
extern "C" void __wrap_lang_free_v1(void *pointer) noexcept {
  if (pointer) g_destroyed.fetch_add(1, std::memory_order_relaxed);
  __real_lang_free_v1(pointer);
}

static LangHandle *track(LangHandle *handle) {
  if (handle)
    g_created.fetch_add(1, std::memory_order_relaxed);
  return handle;
}

static int fail(const char *what, int code) {
  std::fprintf(stderr, "sync_runtime_test: %s\n", what);
  return code;
}

// ---------------------------------------------------------------------------
// Panic subprocess harness
// ---------------------------------------------------------------------------

struct ChildResult {
  int status;
  std::string error;
};

static ChildResult run_child(void (*body)()) {
  int fds[2];
  if (pipe(fds) != 0)
    return {-1, "pipe failed"};

  const pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    return {-1, "fork failed"};
  }

  if (pid == 0) {
    close(fds[0]);
    if (dup2(fds[1], 2) < 0)
      _exit(97);
    close(fds[1]);
    body();
    _exit(0);
  }

  close(fds[1]);
  std::string output;
  char buffer[1024];
  for (;;) {
    const ssize_t count = read(fds[0], buffer, sizeof(buffer));
    if (count > 0) {
      output.append(buffer, static_cast<size_t>(count));
      continue;
    }
    if (count < 0 && errno == EINTR)
      continue;
    break;
  }
  close(fds[0]);

  int status = 0;
  for (;;) {
    const pid_t waited = waitpid(pid, &status, 0);
    if (waited == pid)
      break;
    if (waited < 0 && errno == EINTR)
      continue;
    return {-1, "waitpid failed"};
  }
  return {status, output};
}

static bool check_panic(const char *name, const ChildResult &result,
                        const char *required_text) {
  if (!WIFEXITED(result.status)) {
    std::fprintf(stderr, "sync_runtime_test: %s did not exit normally\n", name);
    return false;
  }
  if (WEXITSTATUS(result.status) != 101) {
    std::fprintf(stderr, "sync_runtime_test: %s exit %d, expected 101\n", name,
                 WEXITSTATUS(result.status));
    return false;
  }
  if (result.error.find("LANG_NATIVE_PANIC:v1:") == std::string::npos ||
      result.error.find(":0:0:0\n") == std::string::npos) {
    std::fprintf(stderr, "sync_runtime_test: %s panic record missing: %s\n",
                 name, result.error.c_str());
    return false;
  }
  if (required_text && result.error.find(required_text) == std::string::npos) {
    std::fprintf(stderr, "sync_runtime_test: %s missing '%s': %s\n", name,
                 required_text, result.error.c_str());
    return false;
  }
  return true;
}

static bool expect_panic(const char *name, void (*body)(),
                         const char *required_text) {
  return check_panic(name, run_child(body), required_text);
}

static bool expect_handle_misuse(const char *name, void (*body)()) {
  return expect_panic(name, body, "runtime-handle-misuse:");
}

#if defined(__x86_64__) && defined(__linux__)
static bool deny_syscall(uint32_t number, uint32_t error) {
  struct sock_filter filter[] = {
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
               offsetof(struct seccomp_data, arch)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
               offsetof(struct seccomp_data, nr)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, number, 0, 1),
      BPF_STMT(BPF_RET | BPF_K,
               SECCOMP_RET_ERRNO |
                   (error & SECCOMP_RET_DATA)),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
  };
  struct sock_fprog program = {
      static_cast<unsigned short>(sizeof(filter) / sizeof(filter[0])), filter};
  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
    return false;
  return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0;
}
#endif

// ---------------------------------------------------------------------------
// Panic cases: constructors must Panic on allocation failure, and misuse must
// Panic with cause 10 and a zero source span.
// ---------------------------------------------------------------------------

static void panic_alloc_mutex() {
#if defined(__x86_64__) && defined(__linux__)
  if (!deny_syscall(__NR_mmap, ENOMEM))
    _exit(98);
#endif
  (void)lang_mutex_new_v1();
  _exit(0);
}

static void panic_alloc_condvar() {
#if defined(__x86_64__) && defined(__linux__)
  if (!deny_syscall(__NR_mmap, ENOMEM))
    _exit(98);
#endif
  (void)lang_condvar_new_v1();
  _exit(0);
}

static void panic_alloc_atomic() {
#if defined(__x86_64__) && defined(__linux__)
  if (!deny_syscall(__NR_mmap, ENOMEM))
    _exit(98);
#endif
  (void)lang_atomic_i64_new_v1(7);
  _exit(0);
}

static void panic_null_mutex() {
  lang_mutex_lock_v1(nullptr);
  _exit(0);
}

static void panic_same_owner_relock() {
  LangHandle *mutex = lang_mutex_new_v1();
  lang_mutex_lock_v1(mutex);
  lang_mutex_lock_v1(mutex);
  _exit(0);
}

static void panic_unlock_unlocked() {
  LangHandle *mutex = lang_mutex_new_v1();
  lang_mutex_unlock_v1(mutex);
  _exit(0);
}

static void panic_wrong_owner_unlock() {
  LangHandle *mutex = lang_mutex_new_v1();
  lang_mutex_lock_v1(mutex);
  std::atomic<int> ready{0};
  std::thread other([&] {
    while (ready.load(std::memory_order_acquire) == 0)
      std::this_thread::yield();
    lang_mutex_unlock_v1(mutex);
    _exit(0);
  });
  ready.store(1, std::memory_order_release);
  other.join();
  _exit(0);
}

static void panic_destroy_locked_mutex() {
  LangHandle *mutex = lang_mutex_new_v1();
  lang_mutex_lock_v1(mutex);
  lang_handle_release_v1(mutex);
  _exit(0);
}

static void panic_wait_not_owned() {
  LangHandle *mutex = lang_mutex_new_v1();
  LangHandle *condvar = lang_condvar_new_v1();
  lang_condvar_wait_v1(condvar, mutex);
  _exit(0);
}

static void panic_destroy_condvar_with_waiters() {
  LangHandle *mutex = lang_mutex_new_v1();
  LangHandle *condvar = lang_condvar_new_v1();
  std::atomic<int> ready{0};
  std::thread waiter([&] {
    lang_mutex_lock_v1(mutex);
    ready.store(1, std::memory_order_release);
    lang_condvar_wait_v1(condvar, mutex);
    _exit(0);
  });
  while (ready.load(std::memory_order_acquire) == 0)
    std::this_thread::yield();
  // The waiter holds the mutex until cv_wait has registered and released it.
  lang_mutex_lock_v1(mutex);
  lang_handle_release_v1(condvar);
  _exit(0);
}

static void panic_mutex_as_condvar() {
  LangHandle *mutex = lang_mutex_new_v1();
  lang_condvar_notify_one_v1(mutex);
  _exit(0);
}

static void panic_condvar_as_mutex() {
  LangHandle *condvar = lang_condvar_new_v1();
  lang_mutex_lock_v1(condvar);
  _exit(0);
}

static void panic_atomic_as_mutex() {
  LangHandle *atomic = lang_atomic_i64_new_v1(0);
  lang_mutex_unlock_v1(atomic);
  _exit(0);
}

static void panic_mutex_as_atomic() {
  LangHandle *mutex = lang_mutex_new_v1();
  (void)lang_atomic_i64_load_v1(mutex);
  _exit(0);
}

// ---------------------------------------------------------------------------
// Mutex mutual exclusion and publication
// ---------------------------------------------------------------------------

constexpr uint32_t kMutexThreads = 8;
constexpr uint32_t kMutexIterations = 5000;

struct MutexShared {
  uint64_t counter;
  uint64_t data[8];
};

MutexShared g_mutex_shared;
std::atomic<uint64_t> g_mutex_errors{0};
LangHandle *g_mutex_handle = nullptr;

static int test_mutex_mutual_exclusion() {
  g_mutex_shared.counter = 0;
  for (uint32_t index = 0; index < 8; ++index)
    g_mutex_shared.data[index] = index;
  g_mutex_errors.store(0, std::memory_order_relaxed);

  g_mutex_handle = track(lang_mutex_new_v1());
  if (!g_mutex_handle)
    return fail("mutex allocation failed", 10);

  std::vector<std::thread> threads;
  threads.reserve(kMutexThreads);
  for (uint32_t thread = 0; thread < kMutexThreads; ++thread) {
    threads.emplace_back([] {
      for (uint32_t iteration = 0; iteration < kMutexIterations; ++iteration) {
        lang_mutex_lock_v1(g_mutex_handle);

        const uint64_t current = g_mutex_shared.counter;
        for (uint32_t index = 0; index < 8; ++index)
          if (g_mutex_shared.data[index] != current + index)
            g_mutex_errors.fetch_add(1, std::memory_order_relaxed);

        ++g_mutex_shared.counter;
        for (uint32_t index = 0; index < 8; ++index)
          g_mutex_shared.data[index] = g_mutex_shared.counter + index;
        for (uint32_t index = 0; index < 8; ++index)
          if (g_mutex_shared.data[index] != g_mutex_shared.counter + index)
            g_mutex_errors.fetch_add(1, std::memory_order_relaxed);

        lang_mutex_unlock_v1(g_mutex_handle);
      }
    });
  }
  for (std::thread &thread : threads)
    thread.join();

  const uint64_t expected =
      static_cast<uint64_t>(kMutexThreads) * kMutexIterations;
  const uint64_t errors = g_mutex_errors.load(std::memory_order_relaxed);
  const uint64_t observed = g_mutex_shared.counter;
  lang_handle_release_v1(g_mutex_handle);
  g_mutex_handle = nullptr;

  if (errors != 0)
    return fail("mutex observed interleaved critical sections", 11);
  if (observed != expected)
    return fail("mutex lost increments", 12);
  return 0;
}

// ---------------------------------------------------------------------------
// Condvar producer/consumer stress
// ---------------------------------------------------------------------------

constexpr uint32_t kQueueCapacity = 8;
constexpr uint32_t kProducerCount = 3;
constexpr uint32_t kConsumerCount = 5;
constexpr uint32_t kItemsPerProducer = 4000;
constexpr uint32_t kTotalItems = kProducerCount * kItemsPerProducer;

struct QueueShared {
  int64_t values[kQueueCapacity];
  uint32_t head;
  uint32_t tail;
  uint32_t count;
  uint32_t producers_done;
  uint64_t produced;
  uint64_t consumed;
  bool done;
};

QueueShared g_queue;
std::atomic<uint32_t> g_seen[kTotalItems];
std::atomic<uint64_t> g_queue_errors{0};
LangHandle *g_queue_mutex = nullptr;
LangHandle *g_not_empty = nullptr;
LangHandle *g_not_full = nullptr;

static void producer(uint32_t producer_index) {
  for (uint32_t sequence = 0; sequence < kItemsPerProducer; ++sequence) {
    const int64_t value =
        static_cast<int64_t>(producer_index * kItemsPerProducer + sequence);
    lang_mutex_lock_v1(g_queue_mutex);
    while (g_queue.count == kQueueCapacity)
      lang_condvar_wait_v1(g_not_full, g_queue_mutex);
    g_queue.values[g_queue.tail] = value;
    g_queue.tail = (g_queue.tail + 1) % kQueueCapacity;
    ++g_queue.count;
    ++g_queue.produced;
    lang_mutex_unlock_v1(g_queue_mutex);
    // Notify after releasing the mutex: a waiter that registers between the
    // unlock and this call must still observe the changed predicate, and a
    // waiter that registered earlier must not lose this wake.
    lang_condvar_notify_one_v1(g_not_empty);
  }

  lang_mutex_lock_v1(g_queue_mutex);
  ++g_queue.producers_done;
  const bool finished = g_queue.producers_done == kProducerCount;
  if (finished)
    g_queue.done = true;
  lang_mutex_unlock_v1(g_queue_mutex);
  if (finished)
    lang_condvar_notify_all_v1(g_not_empty);
}

static void consumer() {
  for (;;) {
    lang_mutex_lock_v1(g_queue_mutex);
    while (g_queue.count == 0 && !g_queue.done)
      lang_condvar_wait_v1(g_not_empty, g_queue_mutex);
    if (g_queue.count == 0 && g_queue.done) {
      lang_mutex_unlock_v1(g_queue_mutex);
      return;
    }

    const int64_t value = g_queue.values[g_queue.head];
    g_queue.head = (g_queue.head + 1) % kQueueCapacity;
    --g_queue.count;
    ++g_queue.consumed;
    lang_mutex_unlock_v1(g_queue_mutex);
    lang_condvar_notify_one_v1(g_not_full);

    if (value < 0 || value >= static_cast<int64_t>(kTotalItems))
      g_queue_errors.fetch_add(1, std::memory_order_relaxed);
    else
      g_seen[static_cast<uint32_t>(value)].fetch_add(
          1, std::memory_order_relaxed);
  }
}

static int test_condvar_producer_consumer() {
  g_queue = QueueShared{};
  for (uint32_t index = 0; index < kTotalItems; ++index)
    g_seen[index].store(0, std::memory_order_relaxed);
  g_queue_errors.store(0, std::memory_order_relaxed);
  g_queue_mutex = track(lang_mutex_new_v1());
  g_not_empty = track(lang_condvar_new_v1());
  g_not_full = track(lang_condvar_new_v1());
  if (!g_queue_mutex || !g_not_empty || !g_not_full)
    return fail("condvar test allocation failed", 20);

  std::vector<std::thread> threads;
  threads.reserve(kProducerCount + kConsumerCount);
  for (uint32_t index = 0; index < kProducerCount; ++index)
    threads.emplace_back(producer, index);
  for (uint32_t index = 0; index < kConsumerCount; ++index)
    threads.emplace_back(consumer);
  for (std::thread &thread : threads)
    thread.join();

  uint64_t missing = 0;
  uint64_t duplicated = 0;
  for (uint32_t index = 0; index < kTotalItems; ++index) {
    const uint32_t count = g_seen[index].load(std::memory_order_relaxed);
    if (count == 0)
      ++missing;
    else if (count != 1)
      ++duplicated;
  }
  const uint64_t errors = g_queue_errors.load(std::memory_order_relaxed);
  const uint64_t produced = g_queue.produced;
  const uint64_t consumed = g_queue.consumed;
  const uint32_t remaining = g_queue.count;

  lang_handle_release_v1(g_not_full);
  lang_handle_release_v1(g_not_empty);
  lang_handle_release_v1(g_queue_mutex);
  g_not_full = nullptr;
  g_not_empty = nullptr;
  g_queue_mutex = nullptr;

  if (errors != 0)
    return fail("condvar delivered an out-of-range item", 21);
  if (produced != kTotalItems || consumed != kTotalItems || remaining != 0)
    return fail("condvar lost or duplicated items", 22);
  if (missing != 0 || duplicated != 0)
    return fail("condvar item coverage mismatch", 23);
  return 0;
}

// ---------------------------------------------------------------------------
// notify_all and predicate re-check
// ---------------------------------------------------------------------------

constexpr uint32_t kNotifyThreads = 8;

std::atomic<uint32_t> g_notify_ready{0};
std::atomic<uint32_t> g_notify_woken{0};
std::atomic<bool> g_notify_gate{false};
LangHandle *g_notify_mutex = nullptr;
LangHandle *g_notify_condvar = nullptr;

static int test_notify_all() {
  g_notify_ready.store(0, std::memory_order_relaxed);
  g_notify_woken.store(0, std::memory_order_relaxed);
  g_notify_gate.store(false, std::memory_order_relaxed);
  g_notify_mutex = track(lang_mutex_new_v1());
  g_notify_condvar = track(lang_condvar_new_v1());
  if (!g_notify_mutex || !g_notify_condvar)
    return fail("notify_all test allocation failed", 30);

  std::vector<std::thread> threads;
  threads.reserve(kNotifyThreads);
  for (uint32_t index = 0; index < kNotifyThreads; ++index) {
    threads.emplace_back([] {
      lang_mutex_lock_v1(g_notify_mutex);
      g_notify_ready.fetch_add(1, std::memory_order_release);
      while (!g_notify_gate.load(std::memory_order_acquire))
        lang_condvar_wait_v1(g_notify_condvar, g_notify_mutex);
      lang_mutex_unlock_v1(g_notify_mutex);
      g_notify_woken.fetch_add(1, std::memory_order_release);
    });
  }

  while (g_notify_ready.load(std::memory_order_acquire) != kNotifyThreads)
    std::this_thread::yield();

  // Change the predicate under the mutex, then notify. A waiter that has not
  // yet registered must see the predicate and skip the wait.
  lang_mutex_lock_v1(g_notify_mutex);
  g_notify_gate.store(true, std::memory_order_release);
  lang_mutex_unlock_v1(g_notify_mutex);
  lang_condvar_notify_all_v1(g_notify_condvar);

  for (std::thread &thread : threads)
    thread.join();

  const uint32_t woken = g_notify_woken.load(std::memory_order_acquire);
  lang_handle_release_v1(g_notify_condvar);
  lang_handle_release_v1(g_notify_mutex);
  g_notify_condvar = nullptr;
  g_notify_mutex = nullptr;

  if (woken != kNotifyThreads)
    return fail("notify_all did not wake every waiter", 31);
  return 0;
}

// ---------------------------------------------------------------------------
// AtomicI64
// ---------------------------------------------------------------------------

static int test_atomic_sequential() {
  LangHandle *atomic = track(lang_atomic_i64_new_v1(INT64_MAX));
  if (!atomic)
    return fail("atomic allocation failed", 40);
  if (atomic->refs != 1 || atomic->kind != LangHandleKind::atomic_i64 ||
      atomic->destroy == nullptr)
    return fail("atomic handle initialization mismatch", 41);

  if (lang_atomic_i64_load_v1(atomic) != INT64_MAX)
    return fail("atomic initial load", 42);

  int64_t previous = lang_atomic_i64_fetch_add_v1(atomic, 1);
  if (previous != INT64_MAX)
    return fail("fetch_add did not return the old value", 43);
  if (static_cast<uint64_t>(lang_atomic_i64_load_v1(atomic)) !=
      0x8000000000000000ULL)
    return fail("fetch_add did not wrap in two's complement", 44);

  previous = lang_atomic_i64_fetch_add_v1(atomic, -1);
  if (previous != INT64_MIN)
    return fail("fetch_add wrap return value", 45);
  if (lang_atomic_i64_load_v1(atomic) != INT64_MAX)
    return fail("fetch_add wrap did not round-trip", 46);

  lang_atomic_i64_store_v1(atomic, 5);
  if (lang_atomic_i64_load_v1(atomic) != 5)
    return fail("atomic store/load", 47);

  if (lang_atomic_i64_compare_exchange_v1(atomic, 6, 7))
    return fail("CAS succeeded with a mismatched expected value", 48);
  if (lang_atomic_i64_load_v1(atomic) != 5)
    return fail("failed CAS stored a value", 49);

  if (!lang_atomic_i64_compare_exchange_v1(atomic, 5, 7))
    return fail("CAS failed with a matching expected value", 50);
  if (lang_atomic_i64_load_v1(atomic) != 7)
    return fail("successful CAS did not store the desired value", 51);

  lang_handle_release_v1(atomic);
  return 0;
}

constexpr uint32_t kAtomicThreads = 8;
constexpr uint32_t kAtomicIterations = 20000;

static int test_atomic_concurrent() {
  LangHandle *counter = track(lang_atomic_i64_new_v1(0));
  LangHandle *cas_counter = track(lang_atomic_i64_new_v1(0));
  if (!counter || !cas_counter)
    return fail("concurrent atomic allocation failed", 60);

  std::atomic<uint64_t> old_sum{0};
  std::vector<std::thread> threads;
  threads.reserve(kAtomicThreads);
  for (uint32_t thread = 0; thread < kAtomicThreads; ++thread) {
    threads.emplace_back([&] {
      int64_t local_sum = 0;
      for (uint32_t iteration = 0; iteration < kAtomicIterations; ++iteration)
        local_sum += lang_atomic_i64_fetch_add_v1(counter, 1);
      old_sum.fetch_add(static_cast<uint64_t>(local_sum),
                        std::memory_order_relaxed);

      for (uint32_t iteration = 0; iteration < kAtomicIterations; ++iteration) {
        int64_t expected = lang_atomic_i64_load_v1(cas_counter);
        while (!lang_atomic_i64_compare_exchange_v1(cas_counter, expected,
                                                    expected + 1))
          expected = lang_atomic_i64_load_v1(cas_counter);
      }
    });
  }
  for (std::thread &thread : threads)
    thread.join();

  const uint64_t total =
      static_cast<uint64_t>(kAtomicThreads) * kAtomicIterations;
  const uint64_t expected_sum = total * (total - 1) / 2;
  const uint64_t counter_value =
      static_cast<uint64_t>(lang_atomic_i64_load_v1(counter));
  const uint64_t cas_value =
      static_cast<uint64_t>(lang_atomic_i64_load_v1(cas_counter));
  const uint64_t observed_sum = old_sum.load(std::memory_order_relaxed);

  lang_handle_release_v1(cas_counter);
  lang_handle_release_v1(counter);

  if (counter_value != total)
    return fail("concurrent fetch_add lost increments", 61);
  if (observed_sum != expected_sum)
    return fail("concurrent fetch_add returned duplicate old values", 62);
  if (cas_value != total)
    return fail("concurrent CAS increment lost updates", 63);
  return 0;
}

static void panic_gettid_failure() {
  auto *mutex = lang_mutex_new_v1();
  if (!deny_syscall(__NR_gettid, EPERM)) _exit(98);
  lang_mutex_lock_v1(mutex);
  _exit(0);
}
static void panic_futex_wait_failure() {
  auto *mutex = lang_mutex_new_v1();
  std::atomic<bool> owned{false};
  std::thread holder([&] {
    lang_mutex_lock_v1(mutex);
    owned.store(true, std::memory_order_release);
    for (;;) __asm__ volatile("pause");
  });
  while (!owned.load(std::memory_order_acquire)) std::this_thread::yield();
  if (!deny_syscall(__NR_futex, EPERM)) _exit(98);
  lang_mutex_lock_v1(mutex);
  _exit(0);
}
static void panic_futex_wake_failure() {
  auto *mutex = lang_mutex_new_v1();
  auto *condition = lang_condvar_new_v1();
  std::atomic<bool> ready{false};
  std::thread waiter([&] {
    lang_mutex_lock_v1(mutex);
    ready.store(true, std::memory_order_release);
    lang_condvar_wait_v1(condition, mutex);
    lang_mutex_unlock_v1(mutex);
  });
  while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();
  lang_mutex_lock_v1(mutex); // The waiter registered before releasing it.
  if (!deny_syscall(__NR_futex, EPERM)) _exit(98);
  lang_condvar_notify_one_v1(condition);
  _exit(0);
}

// ---------------------------------------------------------------------------
// Handle lifecycle / cleanup
// ---------------------------------------------------------------------------

static int test_handle_lifecycle() {
  const uint64_t base_created = g_created.load(std::memory_order_relaxed);
  const uint64_t base_destroyed = g_destroyed.load(std::memory_order_relaxed);

  LangHandle *mutex = track(lang_mutex_new_v1());
  LangHandle *condvar = track(lang_condvar_new_v1());
  LangHandle *atomic = track(lang_atomic_i64_new_v1(11));
  if (!mutex || !condvar || !atomic)
    return fail("lifecycle allocation failed", 70);

  if (mutex->refs != 1 || mutex->kind != LangHandleKind::mutex ||
      mutex->destroy == nullptr)
    return fail("mutex handle fields", 71);
  if (condvar->refs != 1 || condvar->kind != LangHandleKind::condvar ||
      condvar->destroy == nullptr)
    return fail("condvar handle fields", 72);
  if (atomic->refs != 1 || atomic->kind != LangHandleKind::atomic_i64 ||
      atomic->destroy == nullptr)
    return fail("atomic handle fields", 73);
  if (lang_atomic_i64_load_v1(atomic) != 11)
    return fail("atomic constructor initial value", 74);

  lang_handle_retain_v1(mutex);
  if (mutex->refs != 2)
    return fail("handle retain", 75);
  lang_handle_release_v1(mutex);
  if (g_destroyed.load(std::memory_order_relaxed) != base_destroyed)
    return fail("handle destroyed while still referenced", 76);
  lang_handle_release_v1(mutex);
  if (g_destroyed.load(std::memory_order_relaxed) != base_destroyed + 1)
    return fail("handle not destroyed at zero refs", 77);

  lang_handle_release_v1(condvar);
  lang_handle_release_v1(atomic);

  constexpr uint32_t kBatch = 300;
  for (uint32_t index = 0; index < kBatch; ++index) {
    LangHandle *batch_mutex = track(lang_mutex_new_v1());
    LangHandle *batch_condvar = track(lang_condvar_new_v1());
    LangHandle *batch_atomic = track(lang_atomic_i64_new_v1(index));
    if (!batch_mutex || !batch_condvar || !batch_atomic)
      return fail("batch allocation failed", 78);
    lang_handle_release_v1(batch_atomic);
    lang_handle_release_v1(batch_condvar);
    lang_handle_release_v1(batch_mutex);
  }

  const uint64_t created =
      g_created.load(std::memory_order_relaxed) - base_created;
  const uint64_t destroyed =
      g_destroyed.load(std::memory_order_relaxed) - base_destroyed;
  if (created != destroyed)
    return fail("handle cleanup count mismatch", 79);
  return 0;
}

} // namespace

int main() {
  static_assert(sizeof(LangHandle) == 32, "LangHandle ABI changed");

  // Allocation-failure Panic is tested first, before this process creates any
  // pooled allocation, so denying mmap forces the constructor's allocation to
  // fail rather than reusing an existing pool page.
  if (!expect_panic("alloc-mutex", panic_alloc_mutex, "allocation-failed"))
    return 1;
  if (!expect_panic("alloc-condvar", panic_alloc_condvar, "allocation-failed"))
    return 1;
  if (!expect_panic("alloc-atomic", panic_alloc_atomic, "allocation-failed"))
    return 1;

  if (!expect_handle_misuse("null-mutex", panic_null_mutex))
    return 2;
  if (!expect_handle_misuse("same-owner-relock", panic_same_owner_relock))
    return 3;
  if (!expect_handle_misuse("unlock-unlocked", panic_unlock_unlocked))
    return 4;
  if (!expect_handle_misuse("wrong-owner-unlock", panic_wrong_owner_unlock))
    return 5;
  if (!expect_handle_misuse("destroy-locked-mutex", panic_destroy_locked_mutex))
    return 6;
  if (!expect_handle_misuse("wait-not-owned", panic_wait_not_owned))
    return 7;
  if (!expect_handle_misuse("destroy-condvar-with-waiters",
                    panic_destroy_condvar_with_waiters))
    return 8;
  if (!expect_handle_misuse("mutex-as-condvar", panic_mutex_as_condvar))
    return 9;
  if (!expect_handle_misuse("condvar-as-mutex", panic_condvar_as_mutex))
    return 10;
  if (!expect_handle_misuse("atomic-as-mutex", panic_atomic_as_mutex))
    return 11;
  if (!expect_handle_misuse("mutex-as-atomic", panic_mutex_as_atomic))
    return 12;

  if (!expect_handle_misuse("gettid-failure", panic_gettid_failure)) return 13;
  if (!expect_handle_misuse("futex-wait-failure", panic_futex_wait_failure)) return 14;
  if (!expect_handle_misuse("futex-wake-failure", panic_futex_wake_failure)) return 15;

  int result = test_mutex_mutual_exclusion();
  if (result)
    return result;
  result = test_condvar_producer_consumer();
  if (result)
    return result;
  result = test_notify_all();
  if (result)
    return result;
  result = test_atomic_sequential();
  if (result)
    return result;
  result = test_atomic_concurrent();
  if (result)
    return result;
  result = test_handle_lifecycle();
  if (result)
    return result;

  if (g_created.load(std::memory_order_relaxed) !=
      g_destroyed.load(std::memory_order_relaxed))
    return fail("final handle cleanup count mismatch", 80);

  std::puts("sync-runtime-ok");
  return 0;
}
