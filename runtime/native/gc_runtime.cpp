#include "gc_runtime.hpp"

#include "alloc.hpp"
#include "gc_world.hpp"
#include "gc_system.hpp"

// Automatic controller for lang-gc/1 + lang-gc-world/1.
//
// The platform supplies thread IDs and panic I/O. Controller state uses atomics
// and the two accepted runtime layers, with no dynamic initialization.

namespace {

namespace system = lang::native::gc_system;
using system::cpu_pause;
using system::ThreadId;

constexpr int kEintr = -4;

constexpr int64_t kAutoStopNs = 10 * 1000 * 1000; // 10 ms
constexpr uint64_t kInitialPressure = 128;
constexpr uint64_t kMaxU64 = ~uint64_t{0};
// Bits 0..62 of lang_gc_safepoint_word_v1; bit 63 is the world request mirror.
constexpr uint64_t kSafepointBudgetMask = LANG_GC_SAFEPOINT_REQUEST - 1;
static_assert(kInitialPressure == LANG_GC_SAFEPOINT_INITIAL_BUDGET,
              "safepoint budget must initialize to the pressure limit");

struct DeferredRelease {
  DeferredRelease *next;
  void *payload;
  void (*release)(void *) noexcept;
};

// Controller phase is advisory/statistical except for the fork callback guard.
uint32_t g_phase = LANG_GC_RUNTIME_PHASE_IDLE;
uint32_t g_busy = 0;
uint32_t g_enqueue_open = 0;
ThreadId g_collector_tid = 0;
uint64_t g_generation = 0;

uint64_t g_queue_lock_next = 0;
uint64_t g_queue_lock_serving = 0;
DeferredRelease *g_queue_head = nullptr;
DeferredRelease *g_queue_tail = nullptr;
uint64_t g_queue_depth = 0;
uint32_t g_fork_prepared = 0;

uint64_t g_last_live_after = 0;
uint64_t g_queue_enqueued = 0;
uint64_t g_queue_drained = 0;
uint64_t g_collect_ok = 0;
uint64_t g_collect_busy = 0;
uint64_t g_collect_timeout = 0;
uint64_t g_collect_invalid = 0;
uint64_t g_kernel_busy = 0;
uint64_t g_total_registered_scanned = 0;
uint64_t g_total_traced_edges = 0;

void write_all(int fd, const char *bytes, uint64_t size) noexcept {
  while (size) {
    const int64_t result = system::write(fd, bytes, size);
    if (result == kEintr)
      continue;
    if (result <= 0)
      return;
    bytes += result;
    size -= static_cast<uint64_t>(result);
  }
}

[[noreturn]] __attribute__((no_stack_protector)) void
controller_panic_with_tid(const char *reason, int64_t tid) noexcept {
  char record[192];
  uint64_t length = 0;
  const auto append = [&](const char *text) {
    while (*text && length < sizeof(record) - 1)
      record[length++] = *text++;
  };
  const auto append_unsigned = [&](uint64_t value) {
    char digits[24];
    uint64_t count = 0;
    if (value == 0) {
      digits[count++] = '0';
    } else {
      while (value) {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
      }
    }
    while (count && length < sizeof(record) - 1)
      record[length++] = digits[--count];
  };

  append("LANG_NATIVE_PANIC:v1:gc-runtime:");
  append(reason);
  append(":");
  append_unsigned(tid > 0 ? static_cast<uint64_t>(tid) : 0u);
  append(":0:0\n");
  write_all(2, record, length);
  system::exit_panic();
}

[[noreturn]] __attribute__((no_stack_protector)) void
controller_panic(const char *reason) noexcept {
  const int64_t tid = system::raw_current_tid();
  controller_panic_with_tid(reason, tid > 0 ? tid : 0);
}

ThreadId current_tid() noexcept {
  const int64_t tid = system::raw_current_tid();
  if (tid <= 0)
    controller_panic_with_tid("gettid-failed", 0);
  return static_cast<ThreadId>(tid);
}

void queue_lock() noexcept {
  const uint64_t ticket =
      __atomic_fetch_add(&g_queue_lock_next, 1u, __ATOMIC_RELAXED);
  while (__atomic_load_n(&g_queue_lock_serving, __ATOMIC_ACQUIRE) != ticket)
    cpu_pause();
}

void queue_unlock() noexcept {
  __atomic_fetch_add(&g_queue_lock_serving, 1u, __ATOMIC_RELEASE);
}

bool pressure_pending() noexcept {
  const uint64_t limit =
      __atomic_load_n(&lang_gc_runtime_pressure_limit_v1, __ATOMIC_ACQUIRE);
  const uint64_t accum =
      __atomic_load_n(&lang_gc_runtime_pressure_accum_v1, __ATOMIC_ACQUIRE);
  return accum >= limit;
}

void saturating_increment(uint64_t *value) noexcept {
  uint64_t current = __atomic_load_n(value, __ATOMIC_SEQ_CST);
  for (;;) {
    if (current == kMaxU64)
      return;
    if (__atomic_compare_exchange_n(value, &current, current + 1, false,
                                    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
      return;
  }
}

// One-load safepoint fast path support.
//
// lang_gc_safepoint_word_v1 bit 63 mirrors the world request (gc_world.cpp);
// bits 0..62 are the remaining registration budget of the current pressure
// epoch, and 0 advises pending pressure. A generated safepoint loads the word
// once and enters lang_gc_runtime_poll_v1 when the signed value is <= 0.
//
// The budget is a hint, never the authority. The controller poll re-reads
// pressure_accum and pressure_limit and only collects under
// pressure_accum >= pressure_limit, so a wrong hint can cost an idle call but
// can never collect early or park a thread. Exactly two divergences are
// possible, and both are bounded:
//
// (a) Early while a registration is in flight. register() decrements the
//     budget *before* it increments the accumulator, so between those two CAS
//     loops the hint reads one unit lower than limit - accum. The hint can
//     therefore be 0 while accum == limit - 1; a safepoint in that window
//     enters the poll, which re-reads the request word (none), then evaluates
//     the authoritative pressure predicate itself: it collects only if that
//     predicate says pending and the thread is collectible, otherwise it
//     returns unchanged. With n threads concurrently inside register() the hint
//     is at most n units early. This order is deliberate: the common transient is
//     the benign direction, so a threshold is never skipped by an in-flight
//     registration (increment-first would make the common transient late).
//
// (b) Late across a reset, only for a registration that overlaps the reset.
//     If the decrement is overwritten by the publication and the accumulator
//     increment landed after the reset read the accumulator, the registration
//     is forgiven: it is in neither the subtracted accumulator nor the new
//     budget, leaving the hint one unit above limit - accum for that epoch.
//     It is recovered by that many further registrations (each decrements the
//     budget) and republished exactly by the next successful collection, so
//     the threshold is delayed, never lost.
//
// Which writers can overlap a reset? A successful stop requires every other
// node to be SAFE or PARKED (all_stopped_locked):
//   * parked threads are blocked inside the world runtime and cannot register;
//   * SAFE forbids graph writes by contract ("all graph writes must be
//     complete before enter");
//   * a critical region, a fork-stable no-park interval and a foreign callback
//     keep the node ACTIVE, so the stop cannot succeed (timeout or -16) and
//     update_pressure never runs concurrently with them;
//   * the fork child is single-threaded;
//   * the collector's own kernel clear/release callbacks may register
//     (register_on_clear), but they run before the accumulator subtraction, so
//     they are in `pending` and their overwritten decrement is exactly right.
// Under the world contracts therefore only (a) is observable; (b) remains a
// bounded defence in depth for a SAFE thread that writes the graph anyway.
//
// The four mutations (budget decrement, accumulator increment, accumulator
// subtract, budget publication) are seq_cst, so they are totally ordered
// consistently with each thread's program order. For a registration D < I and
// a reset C1 (accumulator CAS) < C2 (budget CAS):
//   * C2 < D => C1 < C2 < D < I: C1 did not observe I, so I is not in
//     `pending` and D applies to the new budget, counted exactly once;
//   * D < C2 and I < C1: I is in `pending` and excluded from the new budget,
//     so the overwritten decrement is exactly right;
//   * D < C2 < I: forgiven, one unit late, case (b).
// A double-counted registration would need C2 < D and C1 observing I, i.e.
// C2 < D < I < C1 < C2, a cycle, so the accounting can never be early through
// the reset path. The subtraction itself never loses a registration: it
// subtracts only the snapshot, so everything registered after the snapshot
// stays pending.
void consume_pressure_budget() noexcept {
  uint64_t current =
      __atomic_load_n(&lang_gc_safepoint_word_v1, __ATOMIC_SEQ_CST);
  for (;;) {
    if ((current & kSafepointBudgetMask) == 0)
      return; // Already advising pressure; saturation keeps it advised.
    if (__atomic_compare_exchange_n(&lang_gc_safepoint_word_v1, &current,
                                    current - 1, false, __ATOMIC_SEQ_CST,
                                    __ATOMIC_SEQ_CST))
      return;
  }
}

// Republish the advisory budget for the epoch a successful collection just
// established, from the exact accumulator value that collection subtracted.
// The CAS preserves the world request mirror.
void publish_pressure_budget(uint64_t limit, uint64_t pending) noexcept {
  uint64_t budget = pending < limit ? limit - pending : 0;
  if (budget > kSafepointBudgetMask)
    budget = kSafepointBudgetMask;
  uint64_t current =
      __atomic_load_n(&lang_gc_safepoint_word_v1, __ATOMIC_SEQ_CST);
  for (;;) {
    if (__atomic_compare_exchange_n(
            &lang_gc_safepoint_word_v1, &current,
            (current & LANG_GC_SAFEPOINT_REQUEST) | budget, false,
            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
      return;
  }
}

void begin_enqueue(ThreadId tid) noexcept {
  queue_lock();
  __atomic_store_n(&g_collector_tid, tid, __ATOMIC_RELAXED);
  __atomic_store_n(&g_enqueue_open, 1u, __ATOMIC_RELEASE);
  __atomic_store_n(&g_phase, LANG_GC_RUNTIME_PHASE_KERNEL, __ATOMIC_RELEASE);
  queue_unlock();
}

DeferredRelease *detach_batch() noexcept {
  queue_lock();
  __atomic_store_n(&g_enqueue_open, 0u, __ATOMIC_RELEASE);
  DeferredRelease *batch = g_queue_head;
  g_queue_head = nullptr;
  g_queue_tail = nullptr;
  __atomic_store_n(&g_queue_depth, 0u, __ATOMIC_RELAXED);
  __atomic_store_n(&g_collector_tid, 0, __ATOMIC_RELAXED);
  queue_unlock();
  return batch;
}

void drain_batch(DeferredRelease *batch) noexcept {
  while (batch) {
    DeferredRelease *item = batch;
    batch = item->next;
    const uint64_t before =
        __atomic_load_n(&g_generation, __ATOMIC_ACQUIRE);
    item->release(item->payload);
    const bool forked =
        __atomic_load_n(&g_generation, __ATOMIC_ACQUIRE) != before;
    __atomic_add_fetch(&g_queue_drained, 1u, __ATOMIC_RELAXED);
    lang_free_v1(item);
    if (forked) {
      // The child inherited this local drain list. It must not execute any
      // further external releases or fake drops for abandoned items.
      return;
    }
  }
}

void update_pressure(uint64_t snapshot, uint64_t live_after) noexcept {
  uint64_t limit;
  if (live_after > kMaxU64 / 2) {
    limit = kMaxU64;
  } else {
    limit = live_after * 2;
    if (limit < kInitialPressure)
      limit = kInitialPressure;
  }

  // Subtract only the registrations captured before this collection. Anything
  // registered after the snapshot remains pending and is not lost.
  uint64_t current =
      __atomic_load_n(&lang_gc_runtime_pressure_accum_v1, __ATOMIC_SEQ_CST);
  uint64_t pending = 0;
  for (;;) {
    pending = current > snapshot ? current - snapshot : 0;
    if (__atomic_compare_exchange_n(&lang_gc_runtime_pressure_accum_v1,
                                    &current, pending, false,
                                    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
      break;
  }
  __atomic_store_n(&lang_gc_runtime_pressure_limit_v1, limit, __ATOMIC_RELEASE);
  publish_pressure_budget(limit, pending);
  __atomic_store_n(&g_last_live_after, live_after, __ATOMIC_RELAXED);
}

int64_t map_world_failure(int64_t status) noexcept {
  if (status == -16)
    __atomic_add_fetch(&g_collect_busy, 1u, __ATOMIC_RELAXED);
  else if (status == -110)
    __atomic_add_fetch(&g_collect_timeout, 1u, __ATOMIC_RELAXED);
  else if (status == -22)
    __atomic_add_fetch(&g_collect_invalid, 1u, __ATOMIC_RELAXED);
  else
    controller_panic("world-stop-status");
  return status;
}

} // namespace

extern "C" {
uint64_t lang_gc_runtime_pressure_accum_v1 = 0;
uint64_t lang_gc_runtime_pressure_limit_v1 = kInitialPressure;
}

extern "C" LangGcStatus lang_gc_runtime_register_v1(
    void *object, const LangGcOps *ops) noexcept {
  const LangGcStatus status = lang_gc_register_v1(object, ops);
  if (status == LANG_GC_OK) {
    // Advisory budget first: see consume_pressure_budget() for why the
    // registration order relative to the accumulator increment matters.
    consume_pressure_budget();
    saturating_increment(&lang_gc_runtime_pressure_accum_v1);
  }
  return status;
}

extern "C" LangGcStatus lang_gc_runtime_unregister_v1(void *object) noexcept {
  return lang_gc_unregister_v1(object);
}

extern "C" void lang_gc_runtime_poll_v1(void) noexcept {
  // Fast path, in order: one atomic world-request load, then pressure loads.
  // No gettid, lock, allocation, futex, or kernel call occurs when neither is
  // pending.
  if (lang_gc_world_poll_required_v1())
    lang_gc_world_poll_v1();
  if (pressure_pending())
    lang_gc_runtime_poll_pressure_v1();
}

extern "C" void lang_gc_runtime_poll_pressure_v1(void) noexcept {
  if (!lang_gc_world_can_collect_v1())
    return;
  (void)lang_gc_runtime_collect_v1(kAutoStopNs, nullptr);
}

extern "C" int64_t lang_gc_runtime_collect_v1(
    int64_t timeout_ns, LangGcStats *out) noexcept {
  if (out)
    *out = LangGcStats{};
  if (timeout_ns < -1) {
    __atomic_add_fetch(&g_collect_invalid, 1u, __ATOMIC_RELAXED);
    return -22;
  }

  uint32_t expected = 0;
  if (!__atomic_compare_exchange_n(&g_busy, &expected, 1u, false,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    __atomic_add_fetch(&g_collect_busy, 1u, __ATOMIC_RELAXED);
    return -16;
  }

  const ThreadId tid = current_tid();
  const uint64_t pressure_snapshot =
      __atomic_load_n(&lang_gc_runtime_pressure_accum_v1, __ATOMIC_ACQUIRE);

  __atomic_store_n(&g_phase, LANG_GC_RUNTIME_PHASE_STOPPING,
                   __ATOMIC_RELEASE);
  const int64_t stop_status = lang_gc_world_stop_v1(timeout_ns);
  if (stop_status != 0) {
    __atomic_store_n(&g_busy, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_collector_tid, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_phase, LANG_GC_RUNTIME_PHASE_IDLE,
                     __ATOMIC_RELEASE);
    return map_world_failure(stop_status);
  }

  // The kernel collection runs with the world stopped. The queue is opened
  // only for this collector and only for the kernel clear/release phase.
  begin_enqueue(tid);
  LangGcStats local{};
  const LangGcStatus kernel_status = lang_gc_collect_stopped_v1(&local);
  if (kernel_status != LANG_GC_OK && kernel_status != LANG_GC_BUSY)
    controller_panic("kernel-status");

  // Detach before resume. From this point new defer calls return false and
  // release their own payload; the old batch remains owned by this stack.
  DeferredRelease *batch = detach_batch();

  const bool kernel_ok = kernel_status == LANG_GC_OK;
  if (kernel_ok) {
    update_pressure(pressure_snapshot, local.registry_live_after);
    __atomic_add_fetch(&g_collect_ok, 1u, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_total_registered_scanned, local.registered,
                       __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_total_traced_edges, local.traced_edges,
                       __ATOMIC_RELAXED);
    __atomic_store_n(&g_last_live_after, local.registry_live_after,
                     __ATOMIC_RELAXED);
  } else {
    __atomic_add_fetch(&g_kernel_busy, 1u, __ATOMIC_RELAXED);
  }

  // The world stop must be released before any external release callback. The
  // controller busy state is cleared before draining as well, so callbacks may
  // re-enter register/collect and create a fresh queue.
  lang_gc_world_resume_v1();
  __atomic_store_n(&g_busy, 0u, __ATOMIC_RELEASE);
  __atomic_store_n(&g_phase,
                   kernel_ok ? LANG_GC_RUNTIME_PHASE_DRAINING
                             : LANG_GC_RUNTIME_PHASE_IDLE,
                   __ATOMIC_RELEASE);

  if (out && kernel_ok)
    *out = local;

  // The queue should be empty on a kernel BUSY result (no clear callbacks ran
  // for this attempt). Draining an unexpected batch after resume at least
  // releases transferred references without running anything under a stop.
  drain_batch(batch);
  __atomic_store_n(&g_phase, LANG_GC_RUNTIME_PHASE_IDLE, __ATOMIC_RELEASE);
  return kernel_ok ? 0 : -16;
}

extern "C" bool lang_gc_runtime_defer_release_v1(
    void *payload, void (*release)(void *) noexcept) noexcept {
  if (!payload || !release)
    return false;
  if (!__atomic_load_n(&g_enqueue_open, __ATOMIC_ACQUIRE))
    return false;

  const ThreadId tid = current_tid();
  if (tid != __atomic_load_n(&g_collector_tid, __ATOMIC_ACQUIRE))
    return false;

  auto *item = static_cast<DeferredRelease *>(
      lang_alloc_v1(sizeof(DeferredRelease), alignof(DeferredRelease)));
  if (!item) {
    // This is the eligible collector path: returning false would force an
    // external release while the world is still stopped. The queue cannot
    // honor its transfer contract, so terminate instead of faking a drop.
    controller_panic("defer-allocation-failed");
  }
  item->next = nullptr;
  item->payload = payload;
  item->release = release;

  queue_lock();
  const bool accepted =
      __atomic_load_n(&g_enqueue_open, __ATOMIC_RELAXED) != 0 &&
      tid == __atomic_load_n(&g_collector_tid, __ATOMIC_RELAXED);
  if (accepted) {
    if (g_queue_tail)
      g_queue_tail->next = item;
    else
      g_queue_head = item;
    g_queue_tail = item;
    __atomic_add_fetch(&g_queue_depth, 1u, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_queue_enqueued, 1u, __ATOMIC_RELAXED);
  }
  queue_unlock();

  if (!accepted)
    lang_free_v1(item);
  return accepted;
}

extern "C" void lang_gc_runtime_stats_v1(LangGcRuntimeStats *out) noexcept {
  if (!out)
    return;
  *out = LangGcRuntimeStats{};
  out->pressure_accum =
      __atomic_load_n(&lang_gc_runtime_pressure_accum_v1, __ATOMIC_ACQUIRE);
  out->pressure_limit =
      __atomic_load_n(&lang_gc_runtime_pressure_limit_v1, __ATOMIC_ACQUIRE);
  out->last_live_after =
      __atomic_load_n(&g_last_live_after, __ATOMIC_RELAXED);
  out->queue_depth = __atomic_load_n(&g_queue_depth, __ATOMIC_RELAXED);
  out->queue_enqueued = __atomic_load_n(&g_queue_enqueued, __ATOMIC_RELAXED);
  out->queue_drained = __atomic_load_n(&g_queue_drained, __ATOMIC_RELAXED);
  out->collect_ok = __atomic_load_n(&g_collect_ok, __ATOMIC_RELAXED);
  out->collect_busy = __atomic_load_n(&g_collect_busy, __ATOMIC_RELAXED);
  out->collect_timeout = __atomic_load_n(&g_collect_timeout, __ATOMIC_RELAXED);
  out->collect_invalid = __atomic_load_n(&g_collect_invalid, __ATOMIC_RELAXED);
  out->kernel_busy = __atomic_load_n(&g_kernel_busy, __ATOMIC_RELAXED);
  out->total_registered_scanned =
      __atomic_load_n(&g_total_registered_scanned, __ATOMIC_RELAXED);
  out->total_traced_edges =
      __atomic_load_n(&g_total_traced_edges, __ATOMIC_RELAXED);
  out->generation = __atomic_load_n(&g_generation, __ATOMIC_RELAXED);
  out->phase = __atomic_load_n(&g_phase, __ATOMIC_ACQUIRE);
  out->busy = __atomic_load_n(&g_busy, __ATOMIC_ACQUIRE);
  out->collector_tid = system::diagnostic_tid(
      __atomic_load_n(&g_collector_tid, __ATOMIC_ACQUIRE));
  out->enqueue_open =
      __atomic_load_n(&g_enqueue_open, __ATOMIC_ACQUIRE);
}

extern "C" void lang_gc_runtime_fork_prepare_v1(void) noexcept {
  const ThreadId tid = current_tid();
  queue_lock();
  if (__atomic_load_n(&g_phase, __ATOMIC_ACQUIRE) ==
          LANG_GC_RUNTIME_PHASE_KERNEL &&
      __atomic_load_n(&g_collector_tid, __ATOMIC_ACQUIRE) == tid) {
    queue_unlock();
    controller_panic("fork-in-gc-callback");
  }
  if (__atomic_load_n(&g_fork_prepared, __ATOMIC_RELAXED) != 0) {
    queue_unlock();
    controller_panic("fork-hook-misuse");
  }
  __atomic_store_n(&g_fork_prepared, 1u, __ATOMIC_RELAXED);
  // The queue lock stays held across the caller's fork.
}

extern "C" void lang_gc_runtime_fork_parent_v1(void) noexcept {
  if (__atomic_load_n(&g_fork_prepared, __ATOMIC_RELAXED) == 0)
    controller_panic("fork-hook-misuse");
  __atomic_store_n(&g_fork_prepared, 0u, __ATOMIC_RELAXED);
  queue_unlock();
}

extern "C" void lang_gc_runtime_fork_child_v1(void) noexcept {
  if (__atomic_load_n(&g_fork_prepared, __ATOMIC_RELAXED) == 0)
    controller_panic("fork-hook-misuse");

  // The queue lock is inherited held. Do not touch any inherited queue node or
  // invoke its release callback: abandoned references are not fake drops.
  __atomic_add_fetch(&g_generation, 1u, __ATOMIC_RELEASE);
  g_queue_head = nullptr;
  g_queue_tail = nullptr;
  __atomic_store_n(&g_queue_depth, 0u, __ATOMIC_RELAXED);
  __atomic_store_n(&g_enqueue_open, 0u, __ATOMIC_RELEASE);
  __atomic_store_n(&g_collector_tid, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&g_busy, 0u, __ATOMIC_RELEASE);
  __atomic_store_n(&g_phase, LANG_GC_RUNTIME_PHASE_IDLE, __ATOMIC_RELEASE);
  // Re-derive the advisory budget from the inherited authoritative state. A
  // fork taken while another thread was between the accumulator subtraction and
  // the budget publication would otherwise hand the child a hint that disagrees
  // with the child's own accumulator/limit; the child is single-threaded here,
  // so one exact republication is enough.
  publish_pressure_budget(
      __atomic_load_n(&lang_gc_runtime_pressure_limit_v1, __ATOMIC_ACQUIRE),
      __atomic_load_n(&lang_gc_runtime_pressure_accum_v1, __ATOMIC_ACQUIRE));
  __atomic_store_n(&g_fork_prepared, 0u, __ATOMIC_RELAXED);
  __atomic_store_n(&g_queue_lock_next, 0u, __ATOMIC_RELAXED);
  __atomic_store_n(&g_queue_lock_serving, 0u, __ATOMIC_RELAXED);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
}
