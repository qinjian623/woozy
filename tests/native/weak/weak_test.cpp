// Host-side concurrency and lifetime oracle for the native Weak runtime.
//
// runtime/native/weak.cpp is compiled as a separate object with the runtime's
// freestanding flags. This harness (and only this harness) uses libc, pthreads
// and C++ std::thread/std::barrier. Fixture objects are self-managed ARC
// objects whose first field is an atomic u64 strong count. Their final release
// calls lang_weak_detach_v1 and only then destroys and unmaps the object.
//
// The race tests do real concurrent work; there is no "basic success" early
// return that would skip the contention phases.

#include "alloc.hpp"
#include "weak.hpp"

#include <atomic>
#include <barrier>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern "C" uint64_t lang_weak_test_allocations() noexcept;
extern "C" uint64_t lang_weak_test_frees() noexcept;
extern "C" uint64_t lang_weak_test_live_allocations() noexcept;
extern "C" uint64_t lang_weak_test_bad_frees() noexcept;
extern "C" uint64_t lang_weak_test_unexpected_sizes() noexcept;
extern "C" void lang_weak_test_reset_alloc_stats() noexcept;

namespace {

constexpr uint64_t kMaxU64 = ~uint64_t{0};
constexpr uint64_t kFixtureMagic = 0x5745414B46495831ULL; // "WEAKFIX1"
constexpr uint64_t kDeadMagic = 0x4445414446495831ULL;
constexpr std::size_t kPageSize = 4096;
constexpr std::size_t kPayloadBytes = 256;

struct alignas(16) Fixture {
  // The Weak runtime assumes this is the first field of every target.
  uint64_t strong_refs;
  LangWeakControl *weak_slot;
  uint64_t magic;
  unsigned char payload[kPayloadBytes];
};

static_assert(offsetof(Fixture, strong_refs) == 0,
              "fixture strong count must stay at offset zero");
static_assert(sizeof(Fixture) <= kPageSize,
              "fixture must fit in one mapped page");

std::atomic<uint64_t> g_fixture_errors{0};

void record_fixture_error() {
  g_fixture_errors.fetch_add(1, std::memory_order_relaxed);
}

unsigned char payload_byte(std::size_t index) {
  return static_cast<unsigned char>((index * 37u + 11u) ^ (index >> 3));
}

bool fixture_payload_ok(const Fixture *fixture) {
  for (std::size_t index = 0; index < kPayloadBytes; ++index)
    if (fixture->payload[index] != payload_byte(index)) return false;
  return true;
}

bool fixture_intact(const Fixture *fixture) {
  return fixture->magic == kFixtureMagic && fixture_payload_ok(fixture);
}

bool slot_is_closed(const LangWeakControl *slot) {
  return reinterpret_cast<uintptr_t>(slot) == 1;
}

Fixture *fixture_create() {
  void *memory = mmap(nullptr, kPageSize, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (memory == MAP_FAILED) {
    std::fprintf(stderr, "weak_test: fixture mmap failed: %s\n",
                 std::strerror(errno));
    return nullptr;
  }
  auto *fixture = std::construct_at(static_cast<Fixture *>(memory));
  fixture->strong_refs = 1;
  fixture->weak_slot = nullptr;
  fixture->magic = kFixtureMagic;
  for (std::size_t index = 0; index < kPayloadBytes; ++index)
    fixture->payload[index] = payload_byte(index);
  return fixture;
}

// Mirrors generated ARC: every release is atomic; the unique 1 -> 0 transition
// detaches the Weak control block before destroying/unmapping the object.
void fixture_release(Fixture *fixture) noexcept {
  uint64_t refs =
      __atomic_load_n(&fixture->strong_refs, __ATOMIC_RELAXED);
  for (;;) {
    if (!refs) {
      record_fixture_error();
      return;
    }
    const uint64_t next = refs - 1;
    if (__atomic_compare_exchange_n(&fixture->strong_refs, &refs, next, false,
                                    __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
      if (next) return;
      break;
    }
  }

  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  if (!fixture_intact(fixture)) record_fixture_error();
  LangWeakControl *weak =
      __atomic_load_n(&fixture->weak_slot, __ATOMIC_ACQUIRE);
  if (weak) lang_weak_detach_v1(weak);
  fixture->magic = kDeadMagic;
  std::destroy_at(fixture);
  if (munmap(fixture, kPageSize) != 0) record_fixture_error();
}

bool begin_test(const char *name) {
  const uint64_t live = lang_weak_test_live_allocations();
  const uint64_t bad = lang_weak_test_bad_frees();
  const uint64_t sizes = lang_weak_test_unexpected_sizes();
  bool clean = true;
  if (live || bad || sizes) {
    std::fprintf(stderr,
                 "weak_test: %s starts with allocator residue live=%llu "
                 "bad_free=%llu unexpected_size=%llu\n",
                 name, static_cast<unsigned long long>(live),
                 static_cast<unsigned long long>(bad),
                 static_cast<unsigned long long>(sizes));
    clean = false;
  }
  lang_weak_test_reset_alloc_stats();
  return clean;
}

bool allocator_clean(const char *name) {
  const uint64_t live = lang_weak_test_live_allocations();
  const uint64_t bad = lang_weak_test_bad_frees();
  const uint64_t sizes = lang_weak_test_unexpected_sizes();
  bool clean = true;
  if (live) {
    std::fprintf(stderr,
                 "weak_test: %s leaked %llu Weak control block(s)\n", name,
                 static_cast<unsigned long long>(live));
    clean = false;
  }
  if (bad) {
    std::fprintf(stderr, "weak_test: %s observed %llu bad control free(s)\n",
                 name, static_cast<unsigned long long>(bad));
    clean = false;
  }
  if (sizes) {
    std::fprintf(stderr,
                 "weak_test: %s saw %llu allocation(s) with a non-control "
                 "size\n",
                 name, static_cast<unsigned long long>(sizes));
    clean = false;
  }
  return clean;
}

bool check(bool condition, const char *message) {
  if (!condition) std::fprintf(stderr, "weak_test: %s\n", message);
  return condition;
}

bool test_empty_weak() {
  bool ok = begin_test("empty-weak");
  LangWeakControl *empty = nullptr;
  lang_weak_retain_v1(empty);
  lang_weak_release_v1(empty);
  ok &= check(lang_weak_upgrade_v1(empty) == nullptr,
              "empty Weak must not upgrade");
  ok &= check(!lang_weak_alive_v1(empty), "empty Weak must not be alive");
  lang_weak_detach_v1(empty);

  uint64_t dummy = 1;
  ok &= check(lang_weak_downgrade_v1(nullptr, nullptr) == nullptr,
              "downgrade(null target) must return null");
  ok &= check(lang_weak_downgrade_v1(&dummy, nullptr) == nullptr,
              "downgrade(null owner slot) must return null");
  ok &= check(lang_weak_downgrade_v1(nullptr, nullptr) == nullptr,
              "downgrade(null, null) must return null");
  ok &= allocator_clean("empty-weak");
  return ok;
}

bool test_basic_lifecycle() {
  bool ok = begin_test("basic-lifecycle");
  Fixture *fixture = fixture_create();
  if (!fixture) return false;

  LangWeakControl *first =
      lang_weak_downgrade_v1(fixture, &fixture->weak_slot);
  LangWeakControl *second =
      lang_weak_downgrade_v1(fixture, &fixture->weak_slot);
  ok &= check(first != nullptr && second != nullptr,
              "downgrade returned an empty control block");
  ok &= check(first == second,
              "sequential downgrades installed different control blocks");
  ok &= check(second->weak_refs == 3,
              "two Weak owners plus the implicit target owner were not counted");
  ok &= check(lang_weak_alive_v1(first), "live Weak reported dead");

  void *upgraded = lang_weak_upgrade_v1(first);
  if (upgraded) {
    auto *as_fixture = static_cast<Fixture *>(upgraded);
    ok &= check(as_fixture == fixture && fixture_intact(as_fixture),
                "upgrade returned the wrong/invalid target");
    fixture_release(as_fixture);
  } else {
    ok &= check(false, "live Weak failed to upgrade");
  }

  // Copy/drop path: retain creates another real Weak owner, release drops it.
  lang_weak_retain_v1(first);
  lang_weak_release_v1(first);

  // Drop the first original Weak; `second` remains as the keeper.
  lang_weak_release_v1(first);
  ok &= check(second->weak_refs == 2,
              "dropping one Weak lost the implicit target owner");
  fixture_release(fixture); // final strong release, detaches under the lock

  ok &= check(second->weak_refs == 1,
              "detach did not drop exactly the implicit target owner");
  ok &= check(lang_weak_alive_v1(second) == false,
              "dead target still reports alive");
  ok &= check(lang_weak_upgrade_v1(second) == nullptr,
              "dead target upgraded successfully");

  // A dead Weak is still a valid copyable/droppable value.
  lang_weak_retain_v1(second);
  lang_weak_release_v1(second);
  lang_weak_release_v1(second);

  ok &= check(lang_weak_test_allocations() == 1,
              "single control block was not allocated exactly once");
  ok &= check(lang_weak_test_frees() == 1,
              "single control block was not freed exactly once");
  ok &= allocator_clean("basic-lifecycle");
  ok &= check(g_fixture_errors.load(std::memory_order_relaxed) == 0,
              "fixture lifetime protocol observed an error");
  return ok;
}

struct DowngradeRace {
  explicit DowngradeRace(Fixture *target, unsigned participants)
      : fixture(target), start(participants), errors(0), keeper(nullptr) {}

  Fixture *fixture;
  std::barrier<> start;
  std::atomic<uint64_t> errors;
  LangWeakControl *keeper;
};

void run_downgrade_participant(DowngradeRace *race, unsigned id) {
  constexpr uint64_t kIterations = 3000;
  Fixture *fixture = race->fixture;
  LangWeakControl *first_seen = nullptr;

  race->start.arrive_and_wait();
  for (uint64_t index = 0; index < kIterations; ++index) {
    LangWeakControl *control =
        lang_weak_downgrade_v1(fixture, &fixture->weak_slot);
    if (!control) {
      race->errors.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    if (!first_seen)
      first_seen = control;
    else if (first_seen != control)
      race->errors.fetch_add(1, std::memory_order_relaxed);

    if ((index % 7) == 0) {
      void *upgraded = lang_weak_upgrade_v1(control);
      if (!upgraded) {
        race->errors.fetch_add(1, std::memory_order_relaxed);
      } else {
        auto *as_fixture = static_cast<Fixture *>(upgraded);
        if (as_fixture != fixture || !fixture_intact(as_fixture))
          race->errors.fetch_add(1, std::memory_order_relaxed);
        fixture_release(as_fixture);
      }
    }
    if ((index % 5) == 0) {
      lang_weak_retain_v1(control);
      lang_weak_release_v1(control);
    }
    lang_weak_release_v1(control);
  }

  // No participant releases its strong reference until every loop is done.
  race->start.arrive_and_wait();
  if (id == 0) {
    race->keeper = lang_weak_downgrade_v1(fixture, &fixture->weak_slot);
    if (!race->keeper) race->errors.fetch_add(1, std::memory_order_relaxed);
  }
  race->start.arrive_and_wait();
  fixture_release(fixture);
}

bool test_concurrent_downgrade() {
  constexpr unsigned kParticipants = 8;
  bool ok = begin_test("concurrent-downgrade");
  Fixture *fixture = fixture_create();
  if (!fixture) return false;
  __atomic_store_n(&fixture->strong_refs, kParticipants, __ATOMIC_RELAXED);

  DowngradeRace race(fixture, kParticipants);
  std::vector<std::thread> workers;
  workers.reserve(kParticipants - 1);
  for (unsigned id = 1; id < kParticipants; ++id)
    workers.emplace_back(run_downgrade_participant, &race, id);
  run_downgrade_participant(&race, 0);
  for (auto &worker : workers) worker.join();

  ok &= check(race.errors.load(std::memory_order_relaxed) == 0,
              "concurrent downgrade/upgrade/copy race reported an error");
  if (race.keeper) {
    ok &= check(!lang_weak_alive_v1(race.keeper),
                "keeper Weak stayed alive after final strong release");
    ok &= check(lang_weak_upgrade_v1(race.keeper) == nullptr,
                "keeper Weak upgraded after final strong release");
    lang_weak_retain_v1(race.keeper);
    lang_weak_release_v1(race.keeper);
    lang_weak_release_v1(race.keeper);
  } else {
    ok &= check(false, "race keeper Weak was not installed");
  }

  ok &= allocator_clean("concurrent-downgrade");
  ok &= check(g_fixture_errors.load(std::memory_order_relaxed) == 0,
              "concurrent downgrade fixture observed an error");
  return ok;
}

struct UpgradeRace {
  UpgradeRace(Fixture *target, LangWeakControl *control)
      : fixture(target), weak(control), attempts(0), successes(0),
        failures(0), done(0), errors(0), stop(false) {}

  Fixture *fixture;
  LangWeakControl *weak;
  std::atomic<uint64_t> attempts;
  std::atomic<uint64_t> successes;
  std::atomic<uint64_t> failures;
  std::atomic<uint64_t> done;
  std::atomic<uint64_t> errors;
  std::atomic<bool> stop;
};

void run_upgrade_racer(UpgradeRace *race) {
  constexpr unsigned kBatch = 16;
  while (!race->stop.load(std::memory_order_acquire)) {
    for (unsigned index = 0; index < kBatch; ++index) {
      race->attempts.fetch_add(1, std::memory_order_relaxed);
      void *upgraded = lang_weak_upgrade_v1(race->weak);
      if (!upgraded) {
        race->failures.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      auto *as_fixture = static_cast<Fixture *>(upgraded);
      race->successes.fetch_add(1, std::memory_order_relaxed);
      if (as_fixture != race->fixture || !fixture_intact(as_fixture))
        race->errors.fetch_add(1, std::memory_order_relaxed);
      fixture_release(as_fixture);
    }
  }
  race->done.fetch_add(1, std::memory_order_release);
}

void wait_attempts(const UpgradeRace &race, uint64_t target) {
  while (race.attempts.load(std::memory_order_relaxed) < target)
    std::this_thread::yield();
}

bool test_upgrade_vs_final_release() {
  constexpr unsigned kThreads = 8;
  constexpr uint64_t kBeforeReleaseAttempts = 4096;
  constexpr uint64_t kMinPostDeathAttempts = 1024;
  constexpr unsigned kRounds = 80;

  bool ok = begin_test("upgrade-vs-final-release");
  for (unsigned round = 0; round < kRounds; ++round) {
    Fixture *fixture = fixture_create();
    if (!fixture) {
      ok = false;
      break;
    }
    LangWeakControl *keeper =
        lang_weak_downgrade_v1(fixture, &fixture->weak_slot);
    if (!keeper) {
      ok &= check(false, "upgrade race could not create keeper Weak");
      fixture_release(fixture);
      break;
    }

    // Pin one strong reference. The initial release below leaves exactly this
    // pin alive, so every racer starts from a real live target.
    void *pin = lang_weak_upgrade_v1(keeper);
    if (pin != fixture) {
      ok &= check(false, "upgrade race could not pin target");
      fixture_release(fixture);
      lang_weak_release_v1(keeper);
      break;
    }
    fixture_release(fixture); // drop the construction reference; pin remains

    UpgradeRace race(fixture, keeper);
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (unsigned id = 0; id < kThreads; ++id)
      workers.emplace_back(run_upgrade_racer, &race);

    // Let every racer actually enter upgrade before the final strong release.
    wait_attempts(race, kBeforeReleaseAttempts);
    fixture_release(static_cast<Fixture *>(pin)); // final release candidate

    // Wait until the final strong release has actually detached the target,
    // then require more upgrade attempts. Counting attempts before the death is
    // not enough: a preempted racer can hold the last strong reference while
    // the other racers exhaust any fixed budget. Once the target is dead, every
    // further upgrade must fail, so this is also a deterministic UAF/ABA probe.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (lang_weak_alive_v1(keeper)) {
      if (std::chrono::steady_clock::now() >= deadline) break;
      std::this_thread::yield();
    }
    const uint64_t attempts_at_death =
        race.attempts.load(std::memory_order_relaxed);
    while (race.failures.load(std::memory_order_relaxed) == 0 &&
           race.attempts.load(std::memory_order_relaxed) <
               attempts_at_death + kMinPostDeathAttempts &&
           std::chrono::steady_clock::now() < deadline)
      std::this_thread::yield();
    race.stop.store(true, std::memory_order_release);
    while (race.done.load(std::memory_order_acquire) != kThreads)
      std::this_thread::yield();
    for (auto &worker : workers) worker.join();

    ok &= check(race.errors.load(std::memory_order_relaxed) == 0,
                "upgrade/final-release race saw an invalid upgraded target");
    ok &= check(race.successes.load(std::memory_order_relaxed) > 0,
                "upgrade/final-release race never upgraded a live target");
    ok &= check(race.failures.load(std::memory_order_relaxed) > 0,
                "upgrade/final-release race never observed a dead target");
    ok &= check(!lang_weak_alive_v1(keeper),
                "keeper alive after racing final strong release");
    ok &= check(lang_weak_upgrade_v1(keeper) == nullptr,
                "keeper upgraded after racing final strong release");
    lang_weak_release_v1(keeper);

    ok &= allocator_clean("upgrade-vs-final-release round");
    ok &= check(g_fixture_errors.load(std::memory_order_relaxed) == 0,
                "upgrade race fixture observed an error");
    if (!ok) break;
  }
  return ok;
}

bool wait_child_bounded(pid_t pid, int *status) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  for (;;) {
    const pid_t result = waitpid(pid, status, WNOHANG);
    if (result == pid) return true;
    if (result < 0 && errno != EINTR) return false;
    if (std::chrono::steady_clock::now() >= deadline) {
      kill(pid, SIGKILL);
      while (waitpid(pid, status, 0) < 0 && errno == EINTR) {
      }
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

struct ForkRace {
  ForkRace(Fixture *target, LangWeakControl *control)
      : fixture(target), weak(control), attempts(0), errors(0), done(0),
        stop(false) {}

  Fixture *fixture;
  LangWeakControl *weak;
  std::atomic<uint64_t> attempts;
  std::atomic<uint64_t> errors;
  std::atomic<uint64_t> done;
  std::atomic<bool> stop;
};

void run_fork_racer(ForkRace *race) {
  while (!race->stop.load(std::memory_order_acquire)) {
    void *upgraded = lang_weak_upgrade_v1(race->weak);
    race->attempts.fetch_add(1, std::memory_order_relaxed);
    if (!upgraded) continue;
    auto *as_fixture = static_cast<Fixture *>(upgraded);
    if (as_fixture != race->fixture || !fixture_intact(as_fixture))
      race->errors.fetch_add(1, std::memory_order_relaxed);
    fixture_release(as_fixture);
  }
  race->done.fetch_add(1, std::memory_order_release);
}

bool test_fork_hooks() {
  constexpr unsigned kWorkers = 4;
  constexpr unsigned kForks = 24;
  bool ok = begin_test("fork-hooks");
  Fixture *fixture = fixture_create();
  if (!fixture) return false;
  LangWeakControl *keeper =
      lang_weak_downgrade_v1(fixture, &fixture->weak_slot);
  if (!keeper) {
    ok &= check(false, "fork test could not create keeper Weak");
    fixture_release(fixture);
    return ok;
  }

  ForkRace race(fixture, keeper);
  std::vector<std::thread> workers;
  workers.reserve(kWorkers);
  for (unsigned id = 0; id < kWorkers; ++id)
    workers.emplace_back(run_fork_racer, &race);
  while (race.attempts.load(std::memory_order_relaxed) < 1024)
    std::this_thread::yield();

  for (unsigned round = 0; round < kForks; ++round) {
    // Exercise the recommended composition with the existing process hooks:
    // allocator lock -> Weak lock -> fork. The child resets allocator state
    // first and Weak ticket state second, matching the parent's release order.
    lang_alloc_fork_prepare_v1();
    lang_weak_fork_prepare_v1();
    const pid_t pid = fork();
    if (pid == 0) {
      lang_alloc_fork_child_v1();
      lang_weak_fork_child_v1();
      int code = 0;
      for (unsigned iteration = 0; iteration < 64; ++iteration) {
        void *upgraded = lang_weak_upgrade_v1(keeper);
        if (!upgraded) {
          code = 1;
          break;
        }
        auto *as_fixture = static_cast<Fixture *>(upgraded);
        if (as_fixture != fixture || !fixture_intact(as_fixture)) {
          code = 2;
          fixture_release(as_fixture);
          break;
        }
        fixture_release(as_fixture);
      }
      if (!code) {
        LangWeakControl *copy =
            lang_weak_downgrade_v1(fixture, &fixture->weak_slot);
        if (!copy)
          code = 3;
        else
          lang_weak_release_v1(copy);
      }
      _exit(code);
    }
    if (pid < 0) {
      lang_weak_fork_parent_v1();
      lang_alloc_fork_parent_v1();
      ok &= check(false, "fork failed");
      break;
    }
    lang_weak_fork_parent_v1();
    lang_alloc_fork_parent_v1();

    int status = 0;
    if (!wait_child_bounded(pid, &status)) {
      ok &= check(false, "fork child timed out or waitpid failed");
      break;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      std::fprintf(stderr,
                   "weak_test: fork child failed status=0x%x (exit=%d sig=%d)\n",
                   status, WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                   WIFSIGNALED(status) ? WTERMSIG(status) : -1);
      ok = false;
      break;
    }
  }

  race.stop.store(true, std::memory_order_release);
  while (race.done.load(std::memory_order_acquire) != kWorkers)
    std::this_thread::yield();
  for (auto &worker : workers) worker.join();

  ok &= check(race.errors.load(std::memory_order_relaxed) == 0,
              "fork race worker saw an invalid upgraded target");
  fixture_release(fixture);
  ok &= check(!lang_weak_alive_v1(keeper),
              "fork test keeper alive after final strong release");
  ok &= check(lang_weak_upgrade_v1(keeper) == nullptr,
              "fork test keeper upgraded after final strong release");
  lang_weak_release_v1(keeper);

  ok &= allocator_clean("fork-hooks");
  ok &= check(g_fixture_errors.load(std::memory_order_relaxed) == 0,
              "fork test fixture observed an error");
  return ok;
}

bool test_address_reuse() {
  bool ok = begin_test("address-reuse");
  Fixture *first = fixture_create();
  if (!first) return false;
  void *const address = first;
  LangWeakControl *old_weak =
      lang_weak_downgrade_v1(first, &first->weak_slot);
  if (!old_weak) {
    ok &= check(false, "address-reuse could not install first Weak");
    fixture_release(first);
    return ok;
  }

  // Really release the first object, then force a new object at the same
  // address. The old control block must still be dead and must not upgrade to
  // the new object (the ABA hazard).
  fixture_release(first);
  void *remapped =
      mmap(address, kPageSize, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  if (remapped != address) {
    ok &= check(false, "MAP_FIXED did not reuse the fixture address");
    if (remapped != MAP_FAILED) munmap(remapped, kPageSize);
    lang_weak_release_v1(old_weak);
    return ok;
  }

  auto *second = std::construct_at(static_cast<Fixture *>(remapped));
  second->strong_refs = 1;
  second->weak_slot = nullptr;
  second->magic = kFixtureMagic;
  for (std::size_t index = 0; index < kPayloadBytes; ++index)
    second->payload[index] = payload_byte(index);

  LangWeakControl *new_weak =
      lang_weak_downgrade_v1(second, &second->weak_slot);
  ok &= check(new_weak != nullptr && new_weak != old_weak,
              "reused address did not get a distinct control block");
  ok &= check(!lang_weak_alive_v1(old_weak),
              "old Weak became alive after target address reuse");
  ok &= check(lang_weak_upgrade_v1(old_weak) == nullptr,
              "old Weak upgraded through an address-reused target");
  ok &= check(old_weak->target == nullptr,
              "old control block retained a stale target pointer");

  void *upgraded = lang_weak_upgrade_v1(new_weak);
  if (upgraded) {
    auto *as_fixture = static_cast<Fixture *>(upgraded);
    ok &= check(as_fixture == second && fixture_intact(as_fixture),
                "new Weak upgraded to the wrong object");
    fixture_release(as_fixture);
  } else {
    ok &= check(false, "new Weak failed to upgrade");
  }

  fixture_release(second); // final release for the reused object
  ok &= check(!lang_weak_alive_v1(new_weak),
              "new Weak alive after reused target final release");
  ok &= check(lang_weak_upgrade_v1(new_weak) == nullptr,
              "new Weak upgraded after reused target final release");

  // Dead copies of both control blocks remain usable/droppable.
  lang_weak_retain_v1(old_weak);
  lang_weak_release_v1(old_weak);
  lang_weak_retain_v1(new_weak);
  lang_weak_release_v1(new_weak);
  lang_weak_release_v1(old_weak);
  lang_weak_release_v1(new_weak);

  ok &= allocator_clean("address-reuse");
  ok &= check(g_fixture_errors.load(std::memory_order_relaxed) == 0,
              "address reuse fixture observed an error");
  return ok;
}

bool test_invalidate_empty_slot() {
  bool ok = begin_test("invalidate-empty-slot");
  Fixture *fixture = fixture_create();
  if (!fixture) return false;

  lang_weak_invalidate_slot_v1(&fixture->weak_slot);
  ok &= check(slot_is_closed(fixture->weak_slot),
              "invalidate did not install the permanent closed tag");
  ok &= check(lang_weak_test_allocations() == 0,
              "invalidating an empty slot allocated a control block");
  ok &= check(lang_weak_downgrade_v1(fixture, &fixture->weak_slot) == nullptr,
              "downgrade installed a control in a closed slot");
  ok &= check(slot_is_closed(fixture->weak_slot),
              "failed downgrade changed the closed tag");
  lang_weak_invalidate_slot_v1(&fixture->weak_slot); // idempotent
  lang_weak_invalidate_slot_v1(nullptr);             // accepted no-op
  lang_weak_detach_v1(fixture->weak_slot);           // tag is harmless
  ok &= check(lang_weak_test_allocations() == 0,
              "repeated invalidate allocated a control block");

  // Final release sees the tag, detach is a no-op, and the target is freed
  // without ever reading a control block.
  fixture_release(fixture);
  ok &= allocator_clean("invalidate-empty-slot");
  ok &= check(g_fixture_errors.load(std::memory_order_relaxed) == 0,
              "empty-slot invalidation fixture observed an error");
  return ok;
}

bool test_invalidate_live_control() {
  bool ok = begin_test("invalidate-live-control");
  Fixture *fixture = fixture_create();
  if (!fixture) return false;

  LangWeakControl *keeper =
      lang_weak_downgrade_v1(fixture, &fixture->weak_slot);
  if (!keeper) {
    ok &= check(false, "invalidation test could not install Weak");
    fixture_release(fixture);
    return ok;
  }
  ok &= check(lang_weak_alive_v1(keeper), "new Weak was not alive");

  lang_weak_invalidate_slot_v1(&fixture->weak_slot);
  ok &= check(slot_is_closed(fixture->weak_slot),
              "live control invalidation did not close the slot");
  ok &= check(!lang_weak_alive_v1(keeper),
              "invalidated Weak still reported alive");
  ok &= check(lang_weak_upgrade_v1(keeper) == nullptr,
              "invalidated Weak still upgraded");
  ok &= check(keeper->target == nullptr,
              "invalidated control retained a target pointer");
  ok &= check(keeper->weak_refs == 1,
              "invalidation did not release exactly the implicit Weak reference");
  ok &= check(fixture_intact(fixture),
              "invalidation damaged the still strongly-held target");

  // A real Weak owner can still copy/drop the dead control, but the canonical
  // slot can never be reopened.
  lang_weak_retain_v1(keeper);
  lang_weak_release_v1(keeper);
  LangWeakControl *reinstall =
      lang_weak_downgrade_v1(fixture, &fixture->weak_slot);
  ok &= check(reinstall == nullptr && slot_is_closed(fixture->weak_slot),
              "closed slot was reopened by downgrade");
  lang_weak_invalidate_slot_v1(&fixture->weak_slot);
  ok &= check(lang_weak_test_allocations() == 1,
              "closed slot installed a second control block");

  // The tag survives final release and makes detach/target cleanup harmless.
  fixture_release(fixture);
  ok &= check(!lang_weak_alive_v1(keeper) &&
                  lang_weak_upgrade_v1(keeper) == nullptr,
              "dead Weak revived after target release");
  lang_weak_release_v1(keeper);

  ok &= check(lang_weak_test_allocations() == 1,
              "live-control invalidation allocated an extra control block");
  ok &= check(lang_weak_test_frees() == 1,
              "live-control invalidation did not free the control exactly once");
  ok &= allocator_clean("invalidate-live-control");
  ok &= check(g_fixture_errors.load(std::memory_order_relaxed) == 0,
              "live-control invalidation fixture observed an error");
  return ok;
}

bool test_invalidate_address_reuse() {
  bool ok = begin_test("invalidate-address-reuse");
  Fixture *first = fixture_create();
  if (!first) return false;
  void *const address = first;
  LangWeakControl *old_weak =
      lang_weak_downgrade_v1(first, &first->weak_slot);
  if (!old_weak) {
    ok &= check(false, "address-reuse could not install first Weak");
    fixture_release(first);
    return ok;
  }

  lang_weak_invalidate_slot_v1(&first->weak_slot);
  fixture_release(first); // slot is tagged, detach is a harmless no-op

  void *remapped =
      mmap(address, kPageSize, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  if (remapped != address) {
    ok &= check(false, "MAP_FIXED did not reuse the invalidated fixture address");
    if (remapped != MAP_FAILED) munmap(remapped, kPageSize);
    lang_weak_release_v1(old_weak);
    return ok;
  }

  auto *second = std::construct_at(static_cast<Fixture *>(remapped));
  second->strong_refs = 1;
  second->weak_slot = nullptr; // normal reconstruction reopens the slot
  second->magic = kFixtureMagic;
  for (std::size_t index = 0; index < kPayloadBytes; ++index)
    second->payload[index] = payload_byte(index);

  LangWeakControl *new_weak =
      lang_weak_downgrade_v1(second, &second->weak_slot);
  ok &= check(new_weak != nullptr && new_weak != old_weak,
              "reused storage did not get a fresh control block");
  ok &= check(!lang_weak_alive_v1(old_weak) &&
                  lang_weak_upgrade_v1(old_weak) == nullptr,
              "old invalidated Weak revived through address reuse");
  void *upgraded = lang_weak_upgrade_v1(new_weak);
  ok &= check(upgraded == second,
              "new Weak after address reuse failed to upgrade");
  if (upgraded) fixture_release(static_cast<Fixture *>(upgraded));

  fixture_release(second);
  lang_weak_retain_v1(old_weak);
  lang_weak_release_v1(old_weak);
  lang_weak_release_v1(old_weak);
  lang_weak_release_v1(new_weak);

  ok &= allocator_clean("invalidate-address-reuse");
  ok &= check(g_fixture_errors.load(std::memory_order_relaxed) == 0,
              "invalidated address-reuse fixture observed an error");
  return ok;
}

struct InvalidateRace {
  explicit InvalidateRace(Fixture *target)
      : fixture(target), start(2), errors(0) {}

  Fixture *fixture;
  std::barrier<> start;
  std::atomic<uint64_t> errors;
};

void run_invalidate_racer(InvalidateRace *race) {
  race->start.arrive_and_wait();
  for (uint64_t iteration = 0; iteration < 20000; ++iteration) {
    LangWeakControl *control =
        lang_weak_downgrade_v1(race->fixture, &race->fixture->weak_slot);
    if (!control) break; // the slot has been permanently closed
    void *upgraded = lang_weak_upgrade_v1(control);
    if (upgraded) {
      auto *as_fixture = static_cast<Fixture *>(upgraded);
      if (as_fixture != race->fixture || !fixture_intact(as_fixture))
        race->errors.fetch_add(1, std::memory_order_relaxed);
      fixture_release(as_fixture);
    }
    if ((iteration & 7u) == 0) {
      lang_weak_retain_v1(control);
      lang_weak_release_v1(control);
    }
    lang_weak_release_v1(control);
  }
}

bool test_invalidate_concurrent_downgrade_upgrade() {
  constexpr unsigned kRounds = 256;
  bool ok = begin_test("invalidate-concurrent");
  for (unsigned round = 0; round < kRounds; ++round) {
    Fixture *fixture = fixture_create();
    if (!fixture) {
      ok = false;
      break;
    }

    // Install a control with only the implicit alive-target reference.  This
    // is the dangerous downgrade path: after invalidate drops that implicit
    // reference the old control may be freed immediately.
    LangWeakControl *seed =
        lang_weak_downgrade_v1(fixture, &fixture->weak_slot);
    if (!seed) {
      ok &= check(false, "invalidate race could not seed control");
      fixture_release(fixture);
      break;
    }
    lang_weak_release_v1(seed);

    InvalidateRace race(fixture);
    std::thread worker(run_invalidate_racer, &race);
    race.start.arrive_and_wait();
    lang_weak_invalidate_slot_v1(&fixture->weak_slot);
    worker.join();

    ok &= check(slot_is_closed(fixture->weak_slot),
                "invalidate race did not leave the slot closed");
    ok &= check(race.errors.load(std::memory_order_relaxed) == 0,
                "invalidate/downgrade/upgrade race saw an invalid target");
    fixture_release(fixture);
    ok &= allocator_clean("invalidate-concurrent round");
    ok &= check(g_fixture_errors.load(std::memory_order_relaxed) == 0,
                "invalidate race fixture observed an error");
    if (!ok) break;
  }
  return ok;
}

void panic_weak_overflow() {
  LangWeakControl control{};
  control.weak_refs = kMaxU64;
  control.target = nullptr;
  lang_weak_retain_v1(&control);
}

void panic_weak_underflow() {
  LangWeakControl control{};
  control.weak_refs = 0;
  control.target = nullptr;
  lang_weak_release_v1(&control);
}

bool expect_weak_panic(void (*action)()) {
  int fds[2];
  if (pipe(fds) != 0) return false;

  lang_weak_fork_prepare_v1();
  const pid_t pid = fork();
  if (pid == 0) {
    lang_weak_fork_child_v1();
    close(fds[0]);
    if (dup2(fds[1], 2) < 0) _exit(97);
    close(fds[1]);
    alarm(5); // terminate a broken implementation instead of hanging forever
    action();
    _exit(0);
  }
  if (pid < 0) {
    lang_weak_fork_parent_v1();
    close(fds[0]);
    close(fds[1]);
    return false;
  }
  lang_weak_fork_parent_v1();
  close(fds[1]);

  char record[1024];
  std::size_t used = 0;
  for (;;) {
    const ssize_t count =
        read(fds[0], record + used, sizeof(record) - 1 - used);
    if (count > 0) {
      used += static_cast<std::size_t>(count);
      if (used == sizeof(record) - 1) break;
      continue;
    }
    if (count < 0 && errno == EINTR) continue;
    break;
  }
  close(fds[0]);
  record[used] = '\0';

  int status = 0;
  while (waitpid(pid, &status, 0) < 0)
    if (errno != EINTR) return false;
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 101) return false;
  return std::strstr(record, "LANG_NATIVE_PANIC:v1:") != nullptr &&
         std::strstr(record, "reference-count-overflow") != nullptr;
}

bool test_panic_contracts() {
  bool ok = begin_test("panic-contracts");
  ok &= check(expect_weak_panic(panic_weak_overflow),
              "Weak retain overflow did not Panic with cause 7");
  ok &= check(expect_weak_panic(panic_weak_underflow),
              "Weak release underflow did not Panic with cause 7");

  // Strong-count overflow is not a Panic: upgrade simply cannot acquire a
  // valid new strong reference and must return null.
  alignas(16) uint64_t strong_and_padding[2] = {kMaxU64, 0};
  LangWeakControl control{};
  control.weak_refs = 2;
  control.target = strong_and_padding;
  ok &= check(lang_weak_upgrade_v1(&control) == nullptr,
              "upgrade succeeded at the strongest maximum count");
  ok &= check(__atomic_load_n(&strong_and_padding[0], __ATOMIC_RELAXED) ==
                  kMaxU64,
              "failed overflow upgrade changed the strong count");

  alignas(16) uint64_t zero_strong[2] = {0, 0};
  control.target = zero_strong;
  ok &= check(lang_weak_upgrade_v1(&control) == nullptr,
              "upgrade succeeded while the strong count was zero");

  ok &= allocator_clean("panic-contracts");
  return ok;
}

} // namespace

int main() {
  int failures = 0;
  auto run = [&failures](const char *name, bool (*test)()) {
    const auto begin = std::chrono::steady_clock::now();
    const bool ok = test();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin);
    std::printf("%s: %s (%lld ms)\n", name, ok ? "PASS" : "FAIL",
                static_cast<long long>(elapsed.count()));
    if (!ok) ++failures;
  };

  // Always run every phase; a successful early lifecycle check must not skip
  // the contention, fork, or allocation-observer tests.
  run("empty-weak", test_empty_weak);
  run("basic-lifecycle", test_basic_lifecycle);
  run("concurrent-downgrade", test_concurrent_downgrade);
  run("upgrade-vs-final-release", test_upgrade_vs_final_release);
  run("invalidate-empty-slot", test_invalidate_empty_slot);
  run("invalidate-live-control", test_invalidate_live_control);
  run("invalidate-address-reuse", test_invalidate_address_reuse);
  run("invalidate-concurrent", test_invalidate_concurrent_downgrade_upgrade);
  run("address-reuse", test_address_reuse);
  run("fork-hooks", test_fork_hooks);
  run("panic-contracts", test_panic_contracts);

  const bool final_clean = allocator_clean("final");
  if (!final_clean) ++failures;
  if (!failures)
    std::printf("weak_test: PASS (control blocks live=%llu)\n",
                static_cast<unsigned long long>(
                    lang_weak_test_live_allocations()));
  else
    std::printf("weak_test: FAIL (%d failed phase(s))\n", failures);
  return failures ? 1 : 0;
}
