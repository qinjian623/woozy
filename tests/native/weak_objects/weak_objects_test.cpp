// Real-object Weak lifecycle tests for the Native runtime.
//
// No source-level Weak[T] syntax is used. Each test takes the canonical slot
// from a real LangVec or LangHandle and exercises the already accepted
// lang_weak_* ABI. The same translation unit is built against the native
// freestanding archive, the hosted archive, ASAN/UBSAN, and a -nostdlib static
// ELF. Its only test-only hooks are allocator link wrappers plus a private
// observation of the final allocator balance.

#include "handle.hpp"
#include "ipc.hpp"
#include "process.hpp"
#include "sync.hpp"
#include "thread.hpp"
#include "vec.hpp"
#include "weak.hpp"

#include <cstddef>
#include <cstdint>

extern "C" uint64_t lang_weak_objects_allocations() noexcept;
extern "C" uint64_t lang_weak_objects_frees() noexcept;
extern "C" uint64_t lang_weak_objects_live_allocations() noexcept;
extern "C" uint64_t lang_weak_objects_bad_frees() noexcept;
extern "C" uint64_t lang_weak_objects_control_allocations() noexcept;
extern "C" void lang_weak_objects_reset_stats() noexcept;

static_assert(sizeof(LangVec) == 80, "LangVec must keep the trailing Weak slot");
static_assert(offsetof(LangVec, refs) == 0, "LangVec refs offset changed");
static_assert(offsetof(LangVec, capacity) == 64,
              "LangVec capacity offset changed");
static_assert(offsetof(LangVec, weak_control) == 72,
              "LangVec Weak slot must follow capacity");
static_assert(sizeof(LangHandle) == 32,
              "LangHandle must keep the trailing Weak slot");
static_assert(offsetof(LangHandle, refs) == 0, "LangHandle refs offset changed");
static_assert(offsetof(LangHandle, kind) == 8, "LangHandle kind offset changed");
static_assert(offsetof(LangHandle, destroy) == 16,
              "LangHandle destroy offset changed");
static_assert(offsetof(LangHandle, weak_control) == 24,
              "LangHandle Weak slot must follow destroy");
static_assert(((sizeof(LangVec) + 15) & ~uint64_t{15}) == 80,
              "Vec payload header must stay 80 bytes");

namespace {

#if !defined(__x86_64__) || !defined(__linux__)
#error "weak_objects_test.cpp targets Linux x86_64"
#endif

long raw_syscall1(long number, long first) noexcept {
  long result;
  __asm__ volatile("syscall"
                   : "=a"(result)
                   : "a"(number), "D"(first)
                   : "rcx", "r11", "memory", "cc");
  return result;
}

long raw_syscall3(long number, long first, long second, long third) noexcept {
  long result;
  __asm__ volatile("syscall"
                   : "=a"(result)
                   : "a"(number), "D"(first), "S"(second), "d"(third)
                   : "rcx", "r11", "memory", "cc");
  return result;
}

void write_stderr(const char *bytes, uint64_t count) noexcept {
  while (count) {
    const long result =
        raw_syscall3(1, 2, reinterpret_cast<long>(bytes),
                     static_cast<long>(count));
    if (result == -4) continue; // EINTR
    if (result <= 0) return;
    bytes += result;
    count -= static_cast<uint64_t>(result);
  }
}

[[noreturn]] void raw_exit(int code) noexcept {
  raw_syscall1(60, code);
  __builtin_unreachable();
}

void cpu_relax() noexcept {
  __asm__ volatile("pause" ::: "memory");
}

int g_failures;

void check(bool condition, int line) noexcept {
  if (condition) return;
  ++g_failures;
  char text[64];
  const char *prefix = "WEAK_OBJECTS_FAIL:";
  uint64_t length = 0;
  while (*prefix) text[length++] = *prefix++;
  char digits[24];
  uint64_t count = 0;
  unsigned value = static_cast<unsigned>(line);
  if (!value) {
    digits[count++] = '0';
  } else {
    while (value) {
      digits[count++] = static_cast<char>('0' + value % 10);
      value /= 10;
    }
  }
  while (count) text[length++] = digits[--count];
  text[length++] = '\n';
  write_stderr(text, length);
}

#define CHECK(condition) check((condition), __LINE__)

constexpr uintptr_t kClosedTag = 1;

bool slot_closed(const LangWeakControl *slot) noexcept {
  return reinterpret_cast<uintptr_t>(slot) == kClosedTag;
}

LangVec *make_vec(uint64_t first, uint64_t second) noexcept {
  LangVec *vec = lang_vec_new_v1(2, sizeof(uint64_t), nullptr, nullptr);
  if (!vec) return nullptr;
  lang_vec_init_v1(vec, &first);
  lang_vec_init_v1(vec, &second);
  return vec;
}

bool wait_until_dead(LangWeakControl *weak) noexcept {
  for (uint64_t attempt = 0; attempt < 200000000ULL; ++attempt) {
    if (!lang_weak_alive_v1(weak)) return true;
    cpu_relax();
  }
  return false;
}

bool wait_until_thread_self_released(LangHandle *handle) noexcept {
  for (uint64_t attempt = 0; attempt < 200000000ULL; ++attempt) {
    if (__atomic_load_n(&handle->refs, __ATOMIC_ACQUIRE) == 1) return true;
    cpu_relax();
  }
  return false;
}

bool wait_value(uint32_t *word, uint32_t expected) noexcept {
  for (uint64_t attempt = 0; attempt < 200000000ULL; ++attempt) {
    if (__atomic_load_n(word, __ATOMIC_ACQUIRE) >= expected) return true;
    cpu_relax();
  }
  return false;
}

bool wait_live_zero() noexcept {
  for (uint64_t attempt = 0; attempt < 200000000ULL; ++attempt) {
    if (lang_weak_objects_live_allocations() == 0) return true;
    cpu_relax();
  }
  return false;
}

bool test_vec_lifecycle() {
  LangVec *vec = make_vec(11, 22);
  CHECK(vec != nullptr);
  if (!vec) return false;
  CHECK(vec->weak_control == nullptr);
  CHECK(lang_vec_at_v1(vec, 0) != nullptr);
  CHECK(*static_cast<uint64_t *>(lang_vec_at_v1(vec, 0)) == 11);
  CHECK(*static_cast<uint64_t *>(lang_vec_at_v1(vec, 1)) == 22);

  LangWeakControl *first =
      lang_weak_downgrade_v1(vec, &vec->weak_control);
  CHECK(first != nullptr);
  CHECK(vec->weak_control == first);

  // Repeated downgrade on one canonical slot observes the same control.
  LangWeakControl *second =
      lang_weak_downgrade_v1(vec, &vec->weak_control);
  CHECK(second == first);

  // A copied Vec shares the descriptor and therefore the slot/control.
  lang_vec_retain_v1(vec);
  LangVec *alias = vec;
  CHECK(alias->weak_control == first);

  void *upgraded = lang_weak_upgrade_v1(first);
  CHECK(upgraded == vec);
  if (upgraded) lang_vec_release_v1(static_cast<LangVec *>(upgraded));
  lang_vec_release_v1(alias);

  // Weak copies retain/release the shared control, not the target.
  lang_weak_retain_v1(second);
  lang_weak_release_v1(second);
  lang_weak_release_v1(second);

  lang_vec_release_v1(vec); // final strong release detaches in the runtime
  CHECK(!lang_weak_alive_v1(first));
  CHECK(lang_weak_upgrade_v1(first) == nullptr);
  lang_weak_release_v1(first); // last Weak owner frees the control
  return true;
}

bool test_closed_slot_and_reuse() {
  LangVec *vec = make_vec(1, 2);
  CHECK(vec != nullptr);
  if (!vec) return false;
  LangWeakControl *weak =
      lang_weak_downgrade_v1(vec, &vec->weak_control);
  CHECK(weak != nullptr);
  if (!weak) {
    lang_vec_release_v1(vec);
    return false;
  }

  lang_weak_invalidate_slot_v1(&vec->weak_control);
  CHECK(slot_closed(vec->weak_control));
  CHECK(lang_weak_downgrade_v1(vec, &vec->weak_control) == nullptr);
  CHECK(slot_closed(vec->weak_control));
  CHECK(!lang_weak_alive_v1(weak));
  CHECK(lang_weak_upgrade_v1(weak) == nullptr);
  lang_vec_release_v1(vec); // closed tag makes detach a no-op
  lang_weak_release_v1(weak);

  // Reusing the freed storage for a new Vec must start with a null slot; a
  // closed tag is never inherited and a new downgrade installs a new control.
  LangVec *reused = lang_vec_new_v1(0, 1, nullptr, nullptr);
  CHECK(reused != nullptr);
  if (!reused) return false;
  CHECK(reused->weak_control == nullptr);
  LangWeakControl *fresh =
      lang_weak_downgrade_v1(reused, &reused->weak_control);
  CHECK(fresh != nullptr);
  CHECK(lang_weak_alive_v1(fresh));
  lang_vec_release_v1(reused);
  CHECK(!lang_weak_alive_v1(fresh));
  CHECK(lang_weak_upgrade_v1(fresh) == nullptr);
  lang_weak_release_v1(fresh);
  return true;
}

bool test_box_cell_projection() {
  // Box[T] projects to its private one-element Vec cell. Two Box copies share
  // that cell and one Weak control; mutation through either copy is visible.
  LangVec *cell = lang_vec_new_v1(1, sizeof(uint64_t), nullptr, nullptr);
  CHECK(cell != nullptr);
  if (!cell) return false;
  uint64_t value = 7;
  lang_vec_init_v1(cell, &value);
  LangVec *box_a = cell;
  lang_vec_retain_v1(box_a);
  LangVec *box_b = cell;
  lang_vec_retain_v1(box_b);
  LangWeakControl *weak =
      lang_weak_downgrade_v1(cell, &cell->weak_control);
  CHECK(weak != nullptr);
  *static_cast<uint64_t *>(lang_vec_at_v1(box_a, 0)) = 99;
  CHECK(*static_cast<uint64_t *>(lang_vec_at_v1(box_b, 0)) == 99);
  lang_vec_release_v1(box_a);
  lang_vec_release_v1(box_b);
  lang_vec_release_v1(cell); // original Box cell owner
  CHECK(!lang_weak_alive_v1(weak));
  CHECK(lang_weak_upgrade_v1(weak) == nullptr);
  lang_weak_release_v1(weak);
  return true;
}

bool test_handle_lifecycle() {
  LangHandle *mutex = lang_mutex_new_v1();
  CHECK(mutex != nullptr);
  if (!mutex) return false;
  CHECK(mutex->weak_control == nullptr);
  lang_mutex_lock_v1(mutex);
  lang_mutex_unlock_v1(mutex);
  LangWeakControl *weak =
      lang_weak_downgrade_v1(mutex, &mutex->weak_control);
  CHECK(weak != nullptr);
  void *upgraded = lang_weak_upgrade_v1(weak);
  CHECK(upgraded == mutex);
  if (upgraded) lang_handle_release_v1(static_cast<LangHandle *>(upgraded));
  lang_handle_release_v1(mutex);
  CHECK(!lang_weak_alive_v1(weak));
  CHECK(lang_weak_upgrade_v1(weak) == nullptr);
  lang_weak_release_v1(weak);

  LangHandle *atomic = lang_atomic_i64_new_v1(41);
  CHECK(atomic != nullptr);
  if (!atomic) return false;
  CHECK(atomic->weak_control == nullptr);
  LangWeakControl *atomic_weak =
      lang_weak_downgrade_v1(atomic, &atomic->weak_control);
  CHECK(atomic_weak != nullptr);
  CHECK(lang_atomic_i64_fetch_add_v1(atomic, 1) == 41);
  CHECK(lang_atomic_i64_load_v1(atomic) == 42);
  lang_handle_release_v1(atomic);
  CHECK(!lang_weak_alive_v1(atomic_weak));
  lang_weak_release_v1(atomic_weak);
  return true;
}

bool test_condvar_and_ipc_slots() {
  LangHandle *condvar = lang_condvar_new_v1();
  CHECK(condvar != nullptr);
  if (!condvar) return false;
  CHECK(condvar->weak_control == nullptr);
  LangWeakControl *condvar_weak =
      lang_weak_downgrade_v1(condvar, &condvar->weak_control);
  CHECK(condvar_weak != nullptr);
  lang_handle_release_v1(condvar);
  CHECK(!lang_weak_alive_v1(condvar_weak));
  CHECK(lang_weak_upgrade_v1(condvar_weak) == nullptr);
  lang_weak_release_v1(condvar_weak);

  LangHandle *mapping = nullptr;
  CHECK(lang_ipc_map_anon_v1(&mapping, 4096, 3) == 0);
  CHECK(mapping != nullptr);
  if (mapping) {
    CHECK(mapping->weak_control == nullptr);
    LangWeakControl *mapping_control =
        lang_weak_downgrade_v1(mapping, &mapping->weak_control);
    CHECK(mapping_control != nullptr);
    lang_handle_release_v1(mapping);
    CHECK(!lang_weak_alive_v1(mapping_control));
    CHECK(lang_weak_upgrade_v1(mapping_control) == nullptr);
    lang_weak_release_v1(mapping_control);
  }
  return true;
}

struct GateContext {
  uint32_t *entered;
  uint32_t *release;
};

int64_t gated_entry(void *context) noexcept {
  auto *gate = static_cast<GateContext *>(context);
  __atomic_store_n(gate->entered, 1u, __ATOMIC_RELEASE);
  while (!__atomic_load_n(gate->release, __ATOMIC_ACQUIRE)) cpu_relax();
  return 17;
}

struct ExactContext {
  uint32_t *start;
  uint64_t rounds;
};

int64_t exact_entry(void *context) noexcept {
  auto *exact = static_cast<ExactContext *>(context);
  while (!__atomic_load_n(exact->start, __ATOMIC_ACQUIRE)) cpu_relax();
  for (uint64_t index = 0; index < exact->rounds; ++index) {
    void *pointer = lang_alloc_v1(16, 8);
    if (!pointer) return 1;
    lang_free_v1(pointer);
  }
  return 0;
}

struct RaceContext {
  LangWeakControl *weak;
  uint32_t *start;
  uint32_t *success;
  uint32_t *holder_ready;
  uint32_t *release_holder;
  uint32_t *holder_released;
  uint64_t attempts;
};

int64_t race_entry(void *context) noexcept {
  auto *race = static_cast<RaceContext *>(context);
  while (!__atomic_load_n(race->start, __ATOMIC_ACQUIRE)) cpu_relax();
  for (uint64_t index = 0; index < race->attempts; ++index) {
    auto *vec = static_cast<LangVec *>(lang_weak_upgrade_v1(race->weak));
    if (!vec) continue;
    if (*static_cast<uint64_t *>(lang_vec_at_v1(vec, 0)) != 123) return 2;
    __atomic_fetch_add(race->success, 1u, __ATOMIC_RELEASE);
    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(race->holder_ready, &expected, 1u, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
      // Hold one successful upgrade across the main thread's final release.
      while (!__atomic_load_n(race->release_holder, __ATOMIC_ACQUIRE))
        cpu_relax();
      lang_vec_release_v1(vec);
      __atomic_store_n(race->holder_released, 1u, __ATOMIC_RELEASE);
    } else {
      lang_vec_release_v1(vec);
    }
  }
  return 0;
}

bool test_observer_exact_count() {
  // Permanent shared-counter oracle: 4 workers x 20000 alloc/free pairs.
  // This is the first phase, so reset runs before any worker exists.
  lang_weak_objects_reset_stats();
  constexpr uint32_t kWorkers = 4;
  constexpr uint64_t kRounds = 20000;
  uint32_t start = 0;
  ExactContext context{&start, kRounds};
  LangHandle *handles[kWorkers] = {};
  for (uint32_t index = 0; index < kWorkers; ++index)
    handles[index] = lang_thread_spawn_v1(
        exact_entry, &context, sizeof(context), nullptr, nullptr);
  for (uint32_t index = 0; index < kWorkers; ++index)
    CHECK(handles[index] != nullptr);
  if (!handles[0] || !handles[1] || !handles[2] || !handles[3]) {
    for (auto *handle : handles)
      if (handle) lang_handle_release_v1(handle);
    return false;
  }

  // Spawn-time thread state (hosted) is part of the baseline. The worker loop
  // delta must then be exactly 80000/80000, not merely alloc == free.
  const uint64_t allocations_before = lang_weak_objects_allocations();
  const uint64_t frees_before = lang_weak_objects_frees();
  const uint64_t live_before = lang_weak_objects_live_allocations();
  __atomic_store_n(&start, 1u, __ATOMIC_RELEASE);
  for (LangHandle *handle : handles)
    CHECK(lang_thread_wait_v1(handle) == 0);

  const uint64_t allocations_delta =
      lang_weak_objects_allocations() - allocations_before;
  const uint64_t frees_delta = lang_weak_objects_frees() - frees_before;
  const uint64_t live_delta =
      lang_weak_objects_live_allocations() - live_before;
  CHECK(allocations_delta == kWorkers * kRounds);
  CHECK(frees_delta == kWorkers * kRounds);
  CHECK(live_delta == 0);
  CHECK(lang_weak_objects_bad_frees() == 0);
  if (allocations_delta == kWorkers * kRounds &&
      frees_delta == kWorkers * kRounds && live_delta == 0) {
    constexpr char message[] = "WEAK_OBJECTS_OBSERVER_EXACT=80000/80000 live=0\n";
    write_stderr(message, sizeof(message) - 1);
  }

  for (LangHandle *handle : handles) {
    CHECK(wait_until_thread_self_released(handle));
    lang_handle_release_v1(handle);
  }
  CHECK(wait_live_zero());
  return true;
}

bool test_thread_runner_last_release() {
  uint32_t entered = 0;
  uint32_t release = 0;
  GateContext context{&entered, &release};
  LangHandle *handle = lang_thread_spawn_v1(
      gated_entry, &context, sizeof(context), nullptr, nullptr);
  CHECK(handle != nullptr);
  if (!handle) return false;
  CHECK(wait_value(&entered, 1));
  CHECK(handle->weak_control == nullptr);
  LangWeakControl *weak =
      lang_weak_downgrade_v1(handle, &handle->weak_control);
  CHECK(weak != nullptr);

  // A caller-owned upgrade must also survive while the runner is gated.
  void *upgraded = lang_weak_upgrade_v1(weak);
  CHECK(upgraded == handle);
  if (upgraded) lang_handle_release_v1(static_cast<LangHandle *>(upgraded));

  // Drop the user's strong reference while the runner is still blocked at the
  // gate. Only the runner's self-reference remains when the gate opens.
  lang_handle_release_v1(handle);
  CHECK(lang_weak_alive_v1(weak));
  __atomic_store_n(&release, 1u, __ATOMIC_RELEASE);

  CHECK(wait_until_dead(weak));
  CHECK(lang_weak_upgrade_v1(weak) == nullptr);
  lang_weak_release_v1(weak);
  // Weak death proves detach, not hosted destroy/free completion. Do not read
  // the freed handle: wait for the actual allocator balance instead.
  CHECK(wait_live_zero());
  return true;
}

bool test_thread_waiter_last_release() {
  uint32_t entered = 0;
  uint32_t release = 0;
  GateContext context{&entered, &release};
  LangHandle *handle = lang_thread_spawn_v1(
      gated_entry, &context, sizeof(context), nullptr, nullptr);
  CHECK(handle != nullptr);
  if (!handle) return false;
  CHECK(wait_value(&entered, 1));
  LangWeakControl *weak =
      lang_weak_downgrade_v1(handle, &handle->weak_control);
  CHECK(weak != nullptr);
  __atomic_store_n(&release, 1u, __ATOMIC_RELEASE);
  CHECK(lang_thread_wait_v1(handle) == 0);

  // Wait for the runner's self-reference to go away while the caller still
  // owns a reference, then make the caller the final strong owner.
  CHECK(wait_until_thread_self_released(handle));
  lang_handle_release_v1(handle);
  CHECK(!lang_weak_alive_v1(weak));
  CHECK(lang_weak_upgrade_v1(weak) == nullptr);
  lang_weak_release_v1(weak);
  CHECK(wait_live_zero());
  return true;
}

bool test_upgrade_final_release_race() {
  LangVec *vec = make_vec(123, 4);
  CHECK(vec != nullptr);
  if (!vec) return false;
  LangWeakControl *weak =
      lang_weak_downgrade_v1(vec, &vec->weak_control);
  CHECK(weak != nullptr);
  if (!weak) {
    lang_vec_release_v1(vec);
    return false;
  }

  uint32_t start = 0;
  uint32_t success = 0;
  uint32_t holder_ready = 0;
  uint32_t release_holder = 0;
  uint32_t holder_released = 0;
  RaceContext race{weak, &start, &success, &holder_ready, &release_holder,
                   &holder_released, 30000};
  LangHandle *handles[4] = {nullptr, nullptr, nullptr, nullptr};
  for (unsigned index = 0; index < 4; ++index) {
    handles[index] = lang_thread_spawn_v1(
        race_entry, &race, sizeof(race), nullptr, nullptr);
    CHECK(handles[index] != nullptr);
  }
  __atomic_store_n(&start, 1u, __ATOMIC_RELEASE);

  // Require real successful upgrades before the final release, and hold one
  // upgraded reference across it. This makes the final release happen while
  // a worker still owns a strong reference, not after every worker saw null.
  CHECK(wait_value(&success, 128));
  CHECK(wait_value(&holder_ready, 1));
  lang_vec_release_v1(vec); // the main thread's original strong reference
  CHECK(lang_weak_alive_v1(weak));
  __atomic_store_n(&release_holder, 1u, __ATOMIC_RELEASE);
  CHECK(wait_value(&holder_released, 1));
  CHECK(wait_until_dead(weak));

  for (LangHandle *handle : handles) {
    if (!handle) continue;
    CHECK(lang_thread_wait_v1(handle) == 0);
    lang_handle_release_v1(handle);
  }
  CHECK(lang_weak_upgrade_v1(weak) == nullptr);
  lang_weak_release_v1(weak);
  CHECK(wait_live_zero());
  return true;
}

bool wait_for_child(int64_t pid, int32_t &status) noexcept {
  if (pid <= 0) return false;
  const int64_t reaped =
      lang_process_waitpid_v1(static_cast<int32_t>(pid), 0, &status);
  return reaped > 0;
}

bool test_fork_success() {
  // Fork before the slot is installed. The child's inherited slot is null and
  // its first downgrade installs a private control; the parent stays untouched.
  LangVec *before = make_vec(5, 6);
  CHECK(before != nullptr);
  if (!before) return false;
  int64_t pid = lang_process_fork_v1();
  CHECK(pid >= 0);
  if (pid == 0) {
    LangWeakControl *weak =
        lang_weak_downgrade_v1(before, &before->weak_control);
    if (!weak) raw_exit(3);
    void *upgraded = lang_weak_upgrade_v1(weak);
    if (upgraded != before) raw_exit(4);
    lang_vec_release_v1(static_cast<LangVec *>(upgraded));
    lang_vec_release_v1(before);
    if (lang_weak_alive_v1(weak)) raw_exit(5);
    lang_weak_release_v1(weak);
    raw_exit(g_failures ? 1 : 0);
  }
  int32_t status = -1;
  CHECK(wait_for_child(pid, status));
  CHECK(status == 0);
  CHECK(before->weak_control == nullptr);
  LangWeakControl *parent_weak =
      lang_weak_downgrade_v1(before, &before->weak_control);
  CHECK(parent_weak != nullptr);
  lang_vec_release_v1(before);
  CHECK(!lang_weak_alive_v1(parent_weak));
  lang_weak_release_v1(parent_weak);

  // Fork after installation. The child can upgrade the living inherited target
  // and its child-side final release closes only its own slot copy.
  LangVec *after = make_vec(7, 8);
  CHECK(after != nullptr);
  if (!after) return false;
  LangWeakControl *weak =
      lang_weak_downgrade_v1(after, &after->weak_control);
  CHECK(weak != nullptr);
  pid = lang_process_fork_v1();
  CHECK(pid >= 0);
  if (pid == 0) {
    if (!lang_weak_alive_v1(weak)) raw_exit(6);
    void *upgraded = lang_weak_upgrade_v1(weak);
    if (upgraded != after) raw_exit(7);
    if (*static_cast<uint64_t *>(lang_vec_at_v1(after, 0)) != 7) raw_exit(8);
    lang_vec_release_v1(static_cast<LangVec *>(upgraded));
    lang_vec_release_v1(after);
    if (lang_weak_alive_v1(weak)) raw_exit(9);
    lang_weak_release_v1(weak);
    raw_exit(g_failures ? 1 : 0);
  }
  status = -1;
  CHECK(wait_for_child(pid, status));
  CHECK(status == 0);
  CHECK(lang_weak_alive_v1(weak));
  void *upgraded = lang_weak_upgrade_v1(weak);
  CHECK(upgraded == after);
  if (upgraded) lang_vec_release_v1(static_cast<LangVec *>(upgraded));
  lang_vec_release_v1(after);
  CHECK(!lang_weak_alive_v1(weak));
  lang_weak_release_v1(weak);
  return true;
}

bool run_all() {
  test_observer_exact_count();
  test_vec_lifecycle();
  test_closed_slot_and_reuse();
  test_box_cell_projection();
  test_handle_lifecycle();
  test_condvar_and_ipc_slots();
  test_thread_runner_last_release();
  test_thread_waiter_last_release();
  test_upgrade_final_release_race();
  test_fork_success();

  CHECK(wait_live_zero());
  const uint64_t allocations = lang_weak_objects_allocations();
  const uint64_t frees = lang_weak_objects_frees();
  const uint64_t live = lang_weak_objects_live_allocations();
  const uint64_t bad_frees = lang_weak_objects_bad_frees();
  const uint64_t controls = lang_weak_objects_control_allocations();
  CHECK(allocations == frees);
  CHECK(live == 0);
  CHECK(bad_frees == 0);
  CHECK(controls > 0); // every lifecycle test installed at least one control

  if (!g_failures) write_stderr("WEAK_OBJECTS_PASS\n", 18);
  return g_failures == 0;
}

} // namespace

#if defined(LANG_WEAK_OBJECTS_FREESTANDING)
extern "C" int main() {
#else
int main() {
#endif
  return run_all() ? 0 : 1;
}
