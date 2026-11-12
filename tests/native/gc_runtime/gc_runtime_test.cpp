// Hosted controller suite for lang-gc-runtime/1. It exercises the accepted
// lang-gc/1 cycle collector, lang-gc-world/1 stop-the-world registry, Weak and
// the automatic controller with real C++ mutator objects.

#include "alloc.hpp"
#include "gc.hpp"
#include "gc_runtime.hpp"
#include "gc_world.hpp"
#include "weak.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

extern "C" uint64_t gc_test_alloc_live() noexcept;
extern "C" uint64_t gc_test_alloc_total() noexcept;

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint64_t kNodeMagic = 0x4e4f44455f4d454dULL;
constexpr uint64_t kExternalMagic = 0x4558545f4d454d31ULL;
constexpr uint64_t kMaxU64 = ~uint64_t{0};

[[noreturn]] void fail_at(const char *file, int line, const char *what) {
  std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, what);
  std::fflush(stderr);
  std::_Exit(1);
}

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition))                                                          \
      fail_at(__FILE__, __LINE__, #condition);                                 \
  } while (0)

void sleep_ms(int milliseconds) {
  std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

bool wait_for(const std::function<bool()> &predicate, int timeout_ms) {
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  while (Clock::now() < deadline) {
    if (predicate())
      return true;
    sleep_ms(1);
  }
  return predicate();
}

struct External {
  uint64_t magic;
  uint64_t kind;
  uint64_t action; // 1 = trigger a re-entrant nested collection
  uint64_t reserved;
};

struct Node {
  uint64_t refs; // must remain first and naturally aligned
  Node *edge;
  External *ext;
  External *ext2;
  LangWeakControl *weak_slot;
  uint64_t sentinel;
  uint64_t block_trace;
  uint64_t register_on_clear;
  uint64_t block_clear;
  uint64_t payload[115];
  uint64_t generation;
};

static_assert(offsetof(Node, refs) == 0, "strong count must be first");
static_assert(sizeof(Node) >= 1000, "graph node should be around 1 KiB");

std::atomic<uint64_t> g_live_objects{0};
std::atomic<uint64_t> g_live_bytes{0};
std::atomic<uint64_t> g_ext_created{0};
std::atomic<uint64_t> g_ext_released[64] = {};
std::atomic<uint64_t> g_callback_bad_world{0};
std::atomic<uint64_t> g_half_active{0};
std::atomic<int64_t> g_nested_status{-9999};
std::atomic<bool> g_nested_done{false};
std::atomic<bool> g_trace_entered{false};
std::atomic<bool> g_trace_release{false};
std::atomic<bool> g_clear_entered{false};
std::atomic<bool> g_clear_release{false};
std::atomic<bool> g_safe_defer_false{false};
std::atomic<bool> g_child_after_fork{false};
std::atomic<int32_t> g_fork_pid{0};
int g_drain_fork_pipe[2] = {-1, -1};

LangGcRuntimeStats runtime_stats() {
  LangGcRuntimeStats stats{};
  lang_gc_runtime_stats_v1(&stats);
  return stats;
}

uint64_t expected_pressure_limit(uint64_t live_after) {
  if (live_after > kMaxU64 / 2)
    return kMaxU64;
  const uint64_t doubled = live_after * 2;
  return doubled < 128 ? 128 : doubled;
}

External *external_new(uint64_t kind, uint64_t action = 0) {
  auto *external = static_cast<External *>(
      lang_alloc_v1(sizeof(External), alignof(External)));
  CHECK(external != nullptr);
  external->magic = kExternalMagic;
  external->kind = kind;
  external->action = action;
  external->reserved = 0;
  g_ext_created.fetch_add(1, std::memory_order_relaxed);
  return external;
}

// Forward declarations of the normal graph helpers.
const LangGcOps *node_ops();
Node *node_new(External *ext, External *ext2);
void fork_from_drain_callback() noexcept;

void nested_release_action() {
  Node *node = node_new(external_new(2, 0), nullptr);
  node->edge = node;
  __atomic_add_fetch(&node->refs, 1u, __ATOMIC_RELAXED);
  CHECK(lang_gc_runtime_register_v1(node, node_ops()) == LANG_GC_OK);
  // Drop the external reference; the self-edge is the only strong edge.
  if (__atomic_sub_fetch(&node->refs, 1u, __ATOMIC_ACQ_REL) == 0)
    fail_at(__FILE__, __LINE__, "nested node released too early");
  LangGcStats stats{};
  g_nested_status.store(lang_gc_runtime_collect_v1(2000000000LL, &stats),
                        std::memory_order_release);
  g_nested_done.store(true, std::memory_order_release);
}

extern "C" void release_external(void *payload) noexcept {
  auto *external = static_cast<External *>(payload);
  if (external->magic != kExternalMagic)
    fail_at(__FILE__, __LINE__, "external payload magic");
  const LangGcRuntimeStats stats = runtime_stats();
  if (stats.busy != 0 || stats.enqueue_open != 0 ||
      lang_gc_world_poll_required_v1() != 0)
    g_callback_bad_world.fetch_add(1, std::memory_order_relaxed);
  if (g_half_active.load(std::memory_order_acquire) != 0)
    g_callback_bad_world.fetch_add(1, std::memory_order_relaxed);
  external->magic = 0;
  g_ext_released[external->kind & 63].fetch_add(1, std::memory_order_relaxed);
  if (external->action == 1)
    nested_release_action();
  else if (external->action == 2)
    fork_from_drain_callback();
  lang_free_v1(external);
}

Node *node_new(External *ext = nullptr, External *ext2 = nullptr) {
  auto *node = static_cast<Node *>(lang_alloc_v1(sizeof(Node), alignof(Node)));
  CHECK(node != nullptr);
  std::memset(node, 0, sizeof(*node));
  node->refs = 1;
  node->edge = nullptr;
  node->ext = ext;
  node->ext2 = ext2;
  node->weak_slot = nullptr;
  node->sentinel = kNodeMagic;
  node->block_trace = 0;
  node->register_on_clear = 0;
  node->block_clear = 0;
  node->payload[0] = kNodeMagic;
  node->payload[114] = kNodeMagic;
  node->generation = g_ext_created.load(std::memory_order_relaxed);
  g_live_objects.fetch_add(1, std::memory_order_relaxed);
  g_live_bytes.fetch_add(sizeof(Node), std::memory_order_relaxed);
  return node;
}

void node_retain(Node *node) {
  CHECK(__atomic_load_n(&node->refs, __ATOMIC_RELAXED) != 0);
  CHECK(__atomic_load_n(&node->refs, __ATOMIC_RELAXED) != kMaxU64);
  __atomic_add_fetch(&node->refs, 1u, __ATOMIC_RELAXED);
}

void node_release(Node *node) {
  if (__atomic_sub_fetch(&node->refs, 1u, __ATOMIC_ACQ_REL) != 0)
    return;

  // Normal final strong release: remove from the kernel registry first, then
  // clean up external references and payload.
  (void)lang_gc_runtime_unregister_v1(node);
  if (node->ext) {
    External *external = node->ext;
    node->ext = nullptr;
    if (!lang_gc_runtime_defer_release_v1(external, release_external))
      release_external(external);
  }
  if (node->ext2) {
    External *external = node->ext2;
    node->ext2 = nullptr;
    if (!lang_gc_runtime_defer_release_v1(external, release_external))
      release_external(external);
  }
  if (node->weak_slot) {
    lang_weak_detach_v1(node->weak_slot);
    node->weak_slot = nullptr;
  }
  g_live_objects.fetch_sub(1, std::memory_order_relaxed);
  g_live_bytes.fetch_sub(sizeof(Node), std::memory_order_relaxed);
  lang_free_v1(node);
}

void register_node(Node *node);

void node_trace(void *object, LangGcVisitor visitor, void *context) noexcept {
  auto *node = static_cast<Node *>(object);
  if (__atomic_load_n(&node->block_trace, __ATOMIC_RELAXED) != 0) {
    g_trace_entered.store(true, std::memory_order_release);
    while (!g_trace_release.load(std::memory_order_acquire))
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (node->edge)
    visitor(node->edge, context);
}

void release_probe(void *payload) noexcept {
  lang_free_v1(payload);
}

void node_clear(void *object) noexcept {
  auto *node = static_cast<Node *>(object);
  if (node->block_clear) {
    g_clear_entered.store(true, std::memory_order_release);
    while (!g_clear_release.load(std::memory_order_acquire))
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (node->register_on_clear) {
    node->register_on_clear = 0;
    Node *late = node_new(nullptr, nullptr);
    late->edge = late;
    node_retain(late);
    register_node(late);
    node_release(late); // drop external; it is not in this kernel snapshot
  }
  if (node->ext) {
    External *external = node->ext;
    node->ext = nullptr;
    if (!lang_gc_runtime_defer_release_v1(external, release_external))
      release_external(external);
  }
  if (node->ext2) {
    External *external = node->ext2;
    node->ext2 = nullptr;
    if (!lang_gc_runtime_defer_release_v1(external, release_external))
      release_external(external);
  }
  // Pure Wz strong edges are removed synchronously, never via the external
  // release queue.
  Node *edge = node->edge;
  node->edge = nullptr;
  if (edge)
    node_release(edge);
}

void node_retain_op(void *object) noexcept {
  node_retain(static_cast<Node *>(object));
}

void node_release_op(void *object) noexcept {
  node_release(static_cast<Node *>(object));
}

void node_invalidate_op(void *object) noexcept {
  auto *node = static_cast<Node *>(object);
  lang_weak_invalidate_slot_v1(&node->weak_slot);
}

const LangGcOps kNodeOps = {
    node_trace,
    node_clear,
    node_retain_op,
    node_release_op,
    node_invalidate_op,
};

const LangGcOps *node_ops() { return &kNodeOps; }

void register_node(Node *node) {
  CHECK(lang_gc_runtime_register_v1(node, &kNodeOps) == LANG_GC_OK);
}

Node *make_garbage_self(External *ext = nullptr, External *ext2 = nullptr) {
  Node *node = node_new(ext, ext2);
  node->edge = node;
  node_retain(node);
  register_node(node);
  node_release(node); // drop the external reference; self-edge remains
  return node;
}

Node *make_rooted_self(External *ext = nullptr, External *ext2 = nullptr) {
  Node *node = node_new(ext, ext2);
  node->edge = node;
  node_retain(node);
  register_node(node);
  return node;
}

void check_root(Node *root) {
  CHECK(root->sentinel == kNodeMagic);
  CHECK(root->edge == root);
  CHECK(root->payload[0] == kNodeMagic);
  CHECK(root->payload[114] == kNodeMagic);
}

void expect_live(uint64_t expected) {
  CHECK(gc_test_alloc_live() == expected);
}

void test_auto_reclaim_8192_garbage_cycles() {
  expect_live(0);
  CHECK(lang_gc_world_register_v1());

  Node *root = make_rooted_self();
  check_root(root);

  // A tracked Weak reference on one garbage node must be dead after the
  // automatic collection, before payload reuse.
  Node *weak_node = node_new();
  weak_node->edge = weak_node;
  node_retain(weak_node);
  register_node(weak_node);
  LangWeakControl *weak = lang_weak_downgrade_v1(weak_node, &weak_node->weak_slot);
  CHECK(weak != nullptr);
  node_release(weak_node); // drop external; Weak does not keep it alive

  uint64_t max_live_objects = 0;
  uint64_t max_live_bytes = 0;
  constexpr int kGarbage = 8192;
  for (int index = 0; index < kGarbage; ++index) {
    make_garbage_self();
    lang_gc_runtime_poll_v1();
    check_root(root);
    if ((index & 31) == 0) {
      const uint64_t live_objects = lang_gc_registry_live_v1();
      const uint64_t live_bytes = g_live_bytes.load(std::memory_order_relaxed);
      if (live_objects > max_live_objects)
        max_live_objects = live_objects;
      if (live_bytes > max_live_bytes)
        max_live_bytes = live_bytes;
      CHECK(live_objects < 1024);
      CHECK(live_bytes < 2 * 1024 * 1024);
    }
  }

  const LangGcRuntimeStats stats = runtime_stats();
  CHECK(stats.collect_ok > 0);
  CHECK(stats.total_registered_scanned > 0);
  CHECK(stats.last_live_after >= 1);
  CHECK(stats.pressure_limit == expected_pressure_limit(stats.last_live_after));
  CHECK(max_live_objects < 1024);
  CHECK(max_live_bytes < 2 * 1024 * 1024);

  CHECK(lang_weak_upgrade_v1(weak) == nullptr);
  CHECK(!lang_weak_alive_v1(weak));
  lang_weak_release_v1(weak);

  check_root(root);
  node_release(root); // drop the external root
  LangGcStats cleanup{};
  CHECK(lang_gc_runtime_collect_v1(2000000000LL, &cleanup) == 0);
  CHECK(lang_gc_registry_live_v1() == 0);
  CHECK(g_live_objects.load(std::memory_order_relaxed) == 0);
  CHECK(g_live_bytes.load(std::memory_order_relaxed) == 0);
  CHECK(lang_gc_world_unregister_v1());
  expect_live(0);
  std::printf("GC_RUNTIME_AUTO_OK garbage=%d max_live_objects=%llu "
              "max_live_bytes=%llu scans=%llu\n",
              kGarbage, static_cast<unsigned long long>(max_live_objects),
              static_cast<unsigned long long>(max_live_bytes),
              static_cast<unsigned long long>(stats.total_registered_scanned));
}

void test_growing_root_budget_is_not_quadratic() {
  constexpr int kRoots = 4096;
  CHECK(lang_gc_world_register_v1());
  const LangGcRuntimeStats baseline = runtime_stats();
  std::vector<Node *> roots;
  roots.reserve(kRoots);
  for (int index = 0; index < kRoots; ++index) {
    Node *node = make_rooted_self();
    roots.push_back(node);
    lang_gc_runtime_poll_v1();
    if ((index & 511) == 0)
      check_root(node);
  }

  const LangGcRuntimeStats stats = runtime_stats();
  const uint64_t growth_collections = stats.collect_ok - baseline.collect_ok;
  const uint64_t growth_scans =
      stats.total_registered_scanned - baseline.total_registered_scanned;
  CHECK(growth_collections > 0);
  CHECK(growth_collections < 64);
  CHECK(stats.last_live_after >= static_cast<uint64_t>(kRoots / 2));
  CHECK(stats.pressure_limit == expected_pressure_limit(stats.last_live_after));
  // Fixed 128-object scanning would be roughly N^2 / 256 registrations here.
  CHECK(growth_scans < static_cast<uint64_t>(4 * kRoots));
  for (Node *root : roots)
    check_root(root);

  for (Node *root : roots)
    node_release(root); // drop external references; self-edges remain garbage
  LangGcStats cleanup{};
  CHECK(lang_gc_runtime_collect_v1(2000000000LL, &cleanup) == 0);
  CHECK(lang_gc_registry_live_v1() == 0);
  CHECK(g_live_objects.load(std::memory_order_relaxed) == 0);
  CHECK(lang_gc_world_unregister_v1());
  expect_live(0);
  std::printf("GC_RUNTIME_GROWTH_OK roots=%d collections=%llu scans=%llu "
              "limit=%llu live_after=%llu\n",
              kRoots, static_cast<unsigned long long>(growth_collections),
              static_cast<unsigned long long>(growth_scans),
              static_cast<unsigned long long>(stats.pressure_limit),
              static_cast<unsigned long long>(stats.last_live_after));
}

void test_critical_safe_timeout_then_collect() {
  CHECK(lang_gc_world_register_v1());

  // Build pending pressure without polling.
  for (int index = 0; index < 128; ++index)
    make_garbage_self(external_new(3, 0));

  std::atomic<bool> c1_ready{false}, c2_ready{false}, safe_ready{false};
  std::atomic<bool> release_critical{false}, release_safe{false};
  std::atomic<bool> done{false};
  std::atomic<int> critical_left{0};

  auto critical_worker = [&](std::atomic<bool> *ready) {
    CHECK(lang_gc_world_register_v1());
    lang_gc_world_enter_critical_v1();
    g_half_active.fetch_add(1, std::memory_order_acquire);
    ready->store(true, std::memory_order_release);
    while (!release_critical.load(std::memory_order_acquire))
      sleep_ms(1);
    lang_gc_world_leave_critical_v1();
    g_half_active.fetch_sub(1, std::memory_order_release);
    critical_left.fetch_add(1, std::memory_order_release);
    while (!done.load(std::memory_order_acquire)) {
      lang_gc_world_poll_v1();
      sleep_ms(1);
    }
    CHECK(lang_gc_world_unregister_v1());
  };

  std::thread c1(critical_worker, &c1_ready);
  std::thread c2(critical_worker, &c2_ready);
  std::thread safe_worker([&] {
    CHECK(lang_gc_world_register_v1());
    lang_gc_world_enter_safe_v1();
    safe_ready.store(true, std::memory_order_release);
    while (!release_safe.load(std::memory_order_acquire))
      sleep_ms(1);
    lang_gc_world_leave_safe_v1();
    CHECK(lang_gc_world_unregister_v1());
  });

  CHECK(wait_for([&] { return c1_ready.load() && c2_ready.load(); }, 5000));
  CHECK(wait_for([&] { return safe_ready.load(); }, 5000));

  const LangGcRuntimeStats before = runtime_stats();
  lang_gc_runtime_poll_v1(); // critical regions force the 10 ms auto stop to time out
  const LangGcRuntimeStats after_timeout = runtime_stats();
  CHECK(after_timeout.collect_timeout > before.collect_timeout);
  CHECK(after_timeout.collect_ok == before.collect_ok);
  CHECK(g_ext_released[3].load() == 0);
  CHECK(g_half_active.load(std::memory_order_acquire) == 2);

  release_critical.store(true, std::memory_order_release);
  CHECK(wait_for([&] {
          return critical_left.load(std::memory_order_acquire) == 2;
        },
        5000));
  CHECK(g_half_active.load(std::memory_order_acquire) == 0);

  lang_gc_runtime_poll_v1(); // SAFE worker must not prevent success
  const LangGcRuntimeStats after_success = runtime_stats();
  CHECK(after_success.collect_ok > before.collect_ok);
  CHECK(after_success.collect_timeout > before.collect_timeout);
  CHECK(g_ext_released[3].load() == 128);
  CHECK(g_callback_bad_world.load() == 0);
  CHECK(lang_gc_registry_live_v1() == 0);

  done.store(true, std::memory_order_release);
  release_safe.store(true, std::memory_order_release);
  c1.join();
  c2.join();
  safe_worker.join();
  CHECK(lang_gc_world_unregister_v1());
  CHECK(g_live_objects.load() == 0);
  expect_live(0);
  std::printf("GC_RUNTIME_TIMEOUT_OK timeout=%llu ok=%llu ext=%llu\n",
              static_cast<unsigned long long>(after_success.collect_timeout),
              static_cast<unsigned long long>(after_success.collect_ok),
              static_cast<unsigned long long>(g_ext_released[3].load()));
}

void test_queue_reentry_nested_collect() {
  CHECK(lang_gc_world_register_v1());
  Node *outer = make_garbage_self(external_new(10, 1), external_new(11, 0));
  (void)outer;
  g_nested_status.store(-9999, std::memory_order_release);
  g_nested_done.store(false, std::memory_order_release);
  const uint64_t enqueued_before = runtime_stats().queue_enqueued;

  LangGcStats outer_stats{};
  CHECK(lang_gc_runtime_collect_v1(2000000000LL, &outer_stats) == 0);
  CHECK(g_nested_done.load(std::memory_order_acquire));
  CHECK(g_nested_status.load(std::memory_order_acquire) == 0);
  CHECK(g_ext_released[10].load() == 1);
  CHECK(g_ext_released[11].load() == 1);
  CHECK(g_ext_released[2].load() == 1);
  CHECK(g_callback_bad_world.load() == 0);
  const LangGcRuntimeStats after = runtime_stats();
  CHECK(after.queue_enqueued >= enqueued_before + 3);
  CHECK(after.queue_drained >= 3);
  CHECK(after.queue_enqueued == after.queue_drained);
  CHECK(after.queue_depth == 0);
  CHECK(after.collect_ok >= 2);
  CHECK(lang_gc_registry_live_v1() == 0);
  CHECK(g_live_objects.load() == 0);
  CHECK(lang_gc_world_unregister_v1());
  expect_live(0);
  std::printf("GC_RUNTIME_QUEUE_OK enqueued=%llu drained=%llu\n",
              static_cast<unsigned long long>(after.queue_enqueued),
              static_cast<unsigned long long>(after.queue_drained));
}

void test_register_during_collect_preserves_pressure() {
  CHECK(lang_gc_world_register_v1());
  Node *outer = node_new(nullptr, nullptr);
  outer->edge = outer;
  node_retain(outer);
  register_node(outer);
  outer->register_on_clear = 1;
  node_release(outer); // drop external; outer is garbage

  LangGcStats stats{};
  CHECK(lang_gc_runtime_collect_v1(2000000000LL, &stats) == 0);
  // clear() registered one late garbage cycle after the kernel snapshot.
  CHECK(lang_gc_registry_live_v1() == 1);
  const LangGcRuntimeStats after = runtime_stats();
  CHECK(after.pressure_accum == 1); // late registration was not lost
  CHECK(after.last_live_after == 1);
  CHECK(after.pressure_limit == expected_pressure_limit(1));

  CHECK(lang_gc_runtime_collect_v1(2000000000LL, &stats) == 0);
  CHECK(lang_gc_registry_live_v1() == 0);
  CHECK(runtime_stats().pressure_accum == 0);
  CHECK(g_live_objects.load() == 0);
  CHECK(lang_gc_world_unregister_v1());
  expect_live(0);
  std::printf("GC_RUNTIME_LATE_REGISTER_OK\n");
}

void test_non_gc_defer_releases_directly() {
  External *external = external_new(50, 0);
  // No controller collection is open; the fast path returns false without
  // gettid or registration so the caller releases its own reference.
  CHECK(!lang_gc_runtime_defer_release_v1(external, release_external));
  release_external(external);
  CHECK(g_ext_released[50].load() == 1);
  CHECK(g_callback_bad_world.load() == 0);
  expect_live(0);
  std::printf("GC_RUNTIME_DIRECT_RELEASE_OK\n");
}

void test_non_collector_cannot_enqueue_during_kernel() {
  CHECK(lang_gc_world_register_v1());
  g_clear_entered.store(false, std::memory_order_release);
  g_clear_release.store(false, std::memory_order_release);
  g_safe_defer_false.store(false, std::memory_order_release);

  Node *node = node_new(nullptr, nullptr);
  node->edge = node;
  node_retain(node);
  register_node(node);
  node->block_clear = 1;
  node_release(node); // drop external; still garbage

  std::atomic<bool> safe_ready{false};
  std::thread safe([&] {
    CHECK(lang_gc_world_register_v1());
    lang_gc_world_enter_safe_v1();
    safe_ready.store(true, std::memory_order_release);
    CHECK(wait_for([&] { return g_clear_entered.load(); }, 5000));
    auto *payload = static_cast<External *>(
        lang_alloc_v1(sizeof(External), alignof(External)));
    CHECK(payload != nullptr);
    payload->magic = kExternalMagic;
    // The collector tid is different; no allocation or enqueue is allowed.
    g_safe_defer_false.store(
        !lang_gc_runtime_defer_release_v1(payload, release_probe),
        std::memory_order_release);
    release_probe(payload); // caller owns it after false
    g_clear_release.store(true, std::memory_order_release);
    lang_gc_world_leave_safe_v1();
    CHECK(lang_gc_world_unregister_v1());
  });

  CHECK(wait_for([&] { return safe_ready.load(); }, 5000));
  LangGcStats stats{};
  CHECK(lang_gc_runtime_collect_v1(2000000000LL, &stats) == 0);
  CHECK(g_safe_defer_false.load(std::memory_order_acquire));
  CHECK(lang_gc_registry_live_v1() == 0);
  safe.join();
  CHECK(g_live_objects.load() == 0);
  CHECK(lang_gc_world_unregister_v1());
  expect_live(0);
  std::printf("GC_RUNTIME_NONCOLLECTOR_ENQUEUE_OK\n");
}

void test_invalid_timeout_and_controller_busy_no_graph_change() {
  CHECK(lang_gc_world_register_v1());
  const uint64_t critical_busy_before = runtime_stats().collect_busy;
  lang_gc_world_enter_critical_v1();
  LangGcStats critical_stats{};
  CHECK(lang_gc_runtime_collect_v1(0, &critical_stats) == -16);
  CHECK(!lang_gc_world_poll_required_v1());
  lang_gc_world_leave_critical_v1();
  CHECK(runtime_stats().collect_busy > critical_busy_before);

  Node *root = make_rooted_self(external_new(12, 0));
  check_root(root);
  const uint64_t live_before = lang_gc_registry_live_v1();
  const LangGcRuntimeStats stats_before = runtime_stats();
  // DUPLICATE/INVALID wrapper results do not add pressure.
  CHECK(lang_gc_runtime_register_v1(root, &kNodeOps) == LANG_GC_DUPLICATE);
  CHECK(lang_gc_runtime_register_v1(nullptr, &kNodeOps) == LANG_GC_INVALID);
  CHECK(runtime_stats().pressure_accum == stats_before.pressure_accum);
  LangGcStats invalid_stats{};
  CHECK(lang_gc_runtime_collect_v1(-2, &invalid_stats) == -22);
  CHECK(lang_gc_registry_live_v1() == live_before);
  check_root(root);
  const LangGcRuntimeStats invalid_after = runtime_stats();
  CHECK(invalid_after.collect_invalid > stats_before.collect_invalid);
  CHECK(invalid_after.pressure_accum == stats_before.pressure_accum);

  std::atomic<bool> critical_ready{false};
  std::atomic<bool> release_critical{false};
  std::thread critical([&] {
    CHECK(lang_gc_world_register_v1());
    lang_gc_world_enter_critical_v1();
    g_half_active.fetch_add(1, std::memory_order_release);
    critical_ready.store(true, std::memory_order_release);
    while (!release_critical.load(std::memory_order_acquire))
      sleep_ms(1);
    lang_gc_world_leave_critical_v1();
    g_half_active.fetch_sub(1, std::memory_order_release);
    CHECK(lang_gc_world_unregister_v1());
  });

  std::atomic<bool> competitor_ready{false};
  std::atomic<int64_t> competitor_status{0};
  std::thread competitor([&] {
    CHECK(lang_gc_world_register_v1());
    competitor_ready.store(true, std::memory_order_release);
    CHECK(wait_for([] { return runtime_stats().busy != 0; }, 5000));
    LangGcStats stats{};
    competitor_status.store(lang_gc_runtime_collect_v1(2000000000LL, &stats),
                            std::memory_order_release);
    CHECK(lang_gc_world_unregister_v1());
  });

  CHECK(wait_for([&] { return critical_ready.load(); }, 5000));
  CHECK(wait_for([&] { return competitor_ready.load(); }, 5000));
  LangGcStats timeout_stats{};
  CHECK(lang_gc_runtime_collect_v1(50000000LL, &timeout_stats) == -110);
  CHECK(wait_for([&] {
          return competitor_status.load(std::memory_order_acquire) != 0;
        },
        5000));
  CHECK(competitor_status.load(std::memory_order_acquire) == -16);
  const LangGcRuntimeStats after = runtime_stats();
  CHECK(after.busy == 0);
  CHECK(after.collect_timeout > invalid_after.collect_timeout);
  CHECK(after.collect_busy > invalid_after.collect_busy);
  CHECK(after.pressure_accum == stats_before.pressure_accum);
  CHECK(lang_gc_registry_live_v1() == live_before);
  check_root(root);
  CHECK(g_half_active.load(std::memory_order_acquire) == 1);

  release_critical.store(true, std::memory_order_release);
  critical.join();
  competitor.join();

  node_release(root);
  LangGcStats cleanup{};
  CHECK(lang_gc_runtime_collect_v1(2000000000LL, &cleanup) == 0);
  CHECK(lang_gc_registry_live_v1() == 0);
  CHECK(g_live_objects.load() == 0);
  CHECK(lang_gc_world_unregister_v1());
  expect_live(0);
  std::printf("GC_RUNTIME_FAIL_OK invalid=%llu busy=%llu timeout=%llu\n",
              static_cast<unsigned long long>(after.collect_invalid),
              static_cast<unsigned long long>(after.collect_busy),
              static_cast<unsigned long long>(after.collect_timeout));
}

struct ForkReport {
  uint64_t magic;
  uint64_t ext_kind;
  uint64_t ext_count;
  uint64_t controller_busy;
  uint64_t controller_phase;
  uint64_t world_request;
  uint64_t registry_live;
  int32_t ok;
  int32_t reserved;
};

constexpr uint64_t kForkReportMagic = 0x464f524b5f474331ULL;

bool write_all_fd(int fd, const void *buffer, size_t size) {
  const auto *bytes = static_cast<const char *>(buffer);
  while (size) {
    const ssize_t written = ::write(fd, bytes, size);
    if (written < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    bytes += written;
    size -= static_cast<size_t>(written);
  }
  return true;
}

bool read_all_fd(int fd, void *buffer, size_t size) {
  auto *bytes = static_cast<char *>(buffer);
  while (size) {
    const ssize_t received = ::read(fd, bytes, size);
    if (received < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    if (received == 0)
      return false;
    bytes += received;
    size -= static_cast<size_t>(received);
  }
  return true;
}

void close_fd(int &fd) {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}

void fork_prepare_all_runtime_locks() {
  lang_alloc_fork_prepare_v1();
  lang_weak_fork_prepare_v1();
  lang_gc_fork_prepare_v1();
  lang_gc_world_fork_prepare_v1();
  lang_gc_runtime_fork_prepare_v1();
}

void fork_parent_all_runtime_locks() {
  lang_gc_runtime_fork_parent_v1();
  lang_gc_world_fork_parent_v1();
  lang_gc_fork_parent_v1();
  lang_weak_fork_parent_v1();
  lang_alloc_fork_parent_v1();
}

void fork_child_all_runtime_locks() {
  lang_alloc_fork_child_v1();
  lang_weak_fork_child_v1();
  lang_gc_fork_child_v1();
  lang_gc_world_fork_child_v1();
  lang_gc_runtime_fork_child_v1();
}

void fork_from_drain_callback() noexcept {
  fork_prepare_all_runtime_locks();
  const pid_t pid = ::fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    fork_child_all_runtime_locks();
    // The inherited drain loop must observe the generation change and abort
    // after this callback; remaining queue payloads are not fake-dropped.
    g_child_after_fork.store(true, std::memory_order_release);
    return;
  }
  fork_parent_all_runtime_locks();
  g_fork_pid.store(pid, std::memory_order_release);
}

int child_controller_smoke(uint64_t ext_kind, ForkReport *report,
                           bool leave_critical_first) {
  const LangGcRuntimeStats stats = runtime_stats();
  report->magic = kForkReportMagic;
  report->controller_busy = stats.busy;
  report->controller_phase = stats.phase;
  report->world_request = lang_gc_world_poll_required_v1();
  report->ext_kind = ext_kind;
  report->ok = stats.busy == 0 && stats.phase == LANG_GC_RUNTIME_PHASE_IDLE &&
               stats.queue_depth == 0 && report->world_request == 0;

  // The child retains only the forking thread's world registration. It must not
  // fake drops of inherited queue/registry entries.
  if (lang_gc_world_register_v1())
    report->ok = 0;

  if (leave_critical_first) {
    if (lang_gc_world_can_collect_v1())
      report->ok = 0; // still critical in the child
    lang_gc_world_leave_critical_v1();
    if (!lang_gc_world_can_collect_v1())
      report->ok = 0;
  }

  // A single inherited rooted graph may remain from the parent. A fresh garbage
  // cycle is collected using the child's reset controller.
  make_garbage_self(external_new(ext_kind, 0));
  LangGcStats collect{};
  if (lang_gc_runtime_collect_v1(2000000000LL, &collect) != 0)
    report->ok = 0;
  report->ext_count = g_ext_released[ext_kind & 63].load();
  if (report->ext_count != 1)
    report->ok = 0;
  if (g_callback_bad_world.load() != 0)
    report->ok = 0;
  report->registry_live = lang_gc_registry_live_v1();
  if (!lang_gc_world_unregister_v1())
    report->ok = 0;
  return report->ok;
}

void test_fork_idle_controller_reset() {
  CHECK(lang_gc_world_register_v1());
  Node *root = make_rooted_self();
  check_root(root);

  int pipe_fds[2] = {-1, -1};
  CHECK(::pipe(pipe_fds) == 0);
  fork_prepare_all_runtime_locks();
  const pid_t pid = ::fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    close_fd(pipe_fds[0]);
    fork_child_all_runtime_locks();
    ForkReport report{};
    (void)child_controller_smoke(20, &report, false);
    (void)write_all_fd(pipe_fds[1], &report, sizeof(report));
    ::close(pipe_fds[1]);
    ::_exit(report.ok ? 0 : 1);
  }

  fork_parent_all_runtime_locks();
  close_fd(pipe_fds[1]);
  ForkReport report{};
  CHECK(read_all_fd(pipe_fds[0], &report, sizeof(report)));
  close_fd(pipe_fds[0]);
  int status = 0;
  CHECK(::waitpid(pid, &status, 0) == pid);
  CHECK(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
  CHECK(report.magic == kForkReportMagic);
  CHECK(report.ok == 1);
  CHECK(report.ext_count == 1);
  CHECK(report.registry_live == 1); // inherited parent root stays live
  check_root(root);

  node_release(root);
  LangGcStats cleanup{};
  CHECK(lang_gc_runtime_collect_v1(2000000000LL, &cleanup) == 0);
  CHECK(lang_gc_registry_live_v1() == 0);
  CHECK(g_live_objects.load() == 0);
  CHECK(lang_gc_world_unregister_v1());
  expect_live(0);
  std::printf("GC_RUNTIME_FORK_IDLE_OK child_phase=%llu\n",
              static_cast<unsigned long long>(report.controller_phase));
}

void test_fork_pending_stop_controller_reset() {
  CHECK(lang_gc_world_register_v1());
  int pipe_fds[2] = {-1, -1};
  CHECK(::pipe(pipe_fds) == 0);

  std::atomic<bool> forker_ready{false};
  std::atomic<bool> fork_done{false};
  std::atomic<bool> forker_left{false};
  std::thread forker([&] {
    CHECK(lang_gc_world_register_v1());
    lang_gc_world_enter_critical_v1();
    forker_ready.store(true, std::memory_order_release);
    CHECK(wait_for([] { return runtime_stats().busy != 0; }, 5000));
    while (!lang_gc_world_poll_required_v1())
      std::this_thread::yield();

    fork_prepare_all_runtime_locks();
    const pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid == 0) {
      close_fd(pipe_fds[0]);
      fork_child_all_runtime_locks();
      ForkReport report{};
      (void)child_controller_smoke(21, &report, true);
      (void)write_all_fd(pipe_fds[1], &report, sizeof(report));
      ::close(pipe_fds[1]);
      ::_exit(report.ok ? 0 : 1);
    }
    fork_parent_all_runtime_locks();
    close_fd(pipe_fds[1]);
    ForkReport report{};
    CHECK(read_all_fd(pipe_fds[0], &report, sizeof(report)));
    close_fd(pipe_fds[0]);
    int status = 0;
    CHECK(::waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    CHECK(report.magic == kForkReportMagic);
    CHECK(report.ok == 1);
    CHECK(report.ext_count == 1);
    fork_done.store(true, std::memory_order_release);

    // The outer leave is the pending stop's safepoint; it parks until resume.
    lang_gc_world_leave_critical_v1();
    forker_left.store(true, std::memory_order_release);
    CHECK(lang_gc_world_unregister_v1());
  });

  CHECK(wait_for([&] { return forker_ready.load(); }, 5000));
  LangGcStats collect{};
  CHECK(lang_gc_runtime_collect_v1(2000000000LL, &collect) == 0);
  CHECK(wait_for([&] { return fork_done.load(); }, 5000));
  CHECK(wait_for([&] { return forker_left.load(); }, 5000));
  forker.join();
  const LangGcRuntimeStats stats = runtime_stats();
  CHECK(stats.busy == 0);
  CHECK(stats.phase == LANG_GC_RUNTIME_PHASE_IDLE);
  CHECK(lang_gc_registry_live_v1() == 0);
  CHECK(lang_gc_world_unregister_v1());
  expect_live(0);
  std::printf("GC_RUNTIME_FORK_STOP_OK collections=%llu\n",
              static_cast<unsigned long long>(stats.collect_ok));
}

struct DrainForkReport {
  uint64_t magic;
  int64_t collect_status;
  uint64_t e1;
  uint64_t e2;
  uint64_t busy;
  uint64_t phase;
  int32_t ok;
  int32_t reserved;
};

void test_fork_from_drain_aborts_child_releases() {
  constexpr uint64_t kDrainForkMagic = 0x445241494e464b31ULL;
  CHECK(lang_gc_world_register_v1());
  CHECK(::pipe(g_drain_fork_pipe) == 0);
  g_child_after_fork.store(false, std::memory_order_release);
  g_fork_pid.store(0, std::memory_order_release);
  g_ext_released[30].store(0, std::memory_order_relaxed);
  g_ext_released[31].store(0, std::memory_order_relaxed);

  make_garbage_self(external_new(30, 2), external_new(31, 0));
  LangGcStats stats{};
  const int64_t status = lang_gc_runtime_collect_v1(2000000000LL, &stats);

  if (g_child_after_fork.load(std::memory_order_acquire)) {
    // Child: the E1 callback returned and drain_batch aborted after detecting
    // the fork generation. E2 must not have run and must not be fake-dropped.
    DrainForkReport report{};
    report.magic = kDrainForkMagic;
    report.collect_status = status;
    report.e1 = g_ext_released[30].load();
    report.e2 = g_ext_released[31].load();
    const LangGcRuntimeStats child_stats = runtime_stats();
    report.busy = child_stats.busy;
    report.phase = child_stats.phase;
    report.ok = status == 0 && report.e1 == 1 && report.e2 == 0 &&
                report.busy == 0;
    (void)write_all_fd(g_drain_fork_pipe[1], &report, sizeof(report));
    ::close(g_drain_fork_pipe[1]);
    ::_exit(report.ok ? 0 : 1);
  }

  CHECK(status == 0);
  DrainForkReport report{};
  CHECK(read_all_fd(g_drain_fork_pipe[0], &report, sizeof(report)));
  ::close(g_drain_fork_pipe[0]);
  const pid_t pid = static_cast<pid_t>(g_fork_pid.load());
  CHECK(pid > 0);
  int child_status = 0;
  CHECK(::waitpid(pid, &child_status, 0) == pid);
  CHECK(WIFEXITED(child_status));
  CHECK(WEXITSTATUS(child_status) == 0);
  CHECK(report.magic == kDrainForkMagic);
  CHECK(report.ok == 1);
  CHECK(report.collect_status == 0);
  CHECK(report.e1 == 1);
  CHECK(report.e2 == 0);
  CHECK(report.busy == 0);
  CHECK(g_ext_released[30].load() == 1);
  CHECK(g_ext_released[31].load() == 1);
  CHECK(lang_gc_registry_live_v1() == 0);
  CHECK(g_live_objects.load() == 0);
  CHECK(lang_gc_world_unregister_v1());
  expect_live(0);
  std::printf("GC_RUNTIME_FORK_DRAIN_OK child_e2=%llu\n",
              static_cast<unsigned long long>(report.e2));
}

void test_kernel_busy_mapping() {
  CHECK(lang_gc_world_register_v1());
  Node *root = make_rooted_self();
  check_root(root);
  __atomic_store_n(&root->block_trace, 1u, __ATOMIC_RELEASE);
  g_trace_entered.store(false, std::memory_order_release);
  g_trace_release.store(false, std::memory_order_release);

  std::atomic<bool> safe_ready{false};
  std::thread direct([&] {
    CHECK(lang_gc_world_register_v1());
    lang_gc_world_enter_safe_v1();
    safe_ready.store(true, std::memory_order_release);
    LangGcStats stats{};
    CHECK(lang_gc_collect_stopped_v1(&stats) == LANG_GC_OK);
    lang_gc_world_leave_safe_v1();
    CHECK(lang_gc_world_unregister_v1());
  });

  CHECK(wait_for([&] { return safe_ready.load(); }, 5000));
  CHECK(wait_for([&] { return g_trace_entered.load(); }, 5000));
  const LangGcRuntimeStats before = runtime_stats();
  LangGcStats controller_stats{};
  CHECK(lang_gc_runtime_collect_v1(2000000000LL, &controller_stats) == -16);
  const LangGcRuntimeStats after = runtime_stats();
  CHECK(after.kernel_busy > before.kernel_busy);
  CHECK(after.busy == 0);
  CHECK(after.pressure_accum == before.pressure_accum);
  CHECK(lang_gc_world_poll_required_v1() == 0);
  CHECK(lang_gc_registry_live_v1() == 1);
  check_root(root);

  __atomic_store_n(&root->block_trace, 0u, __ATOMIC_RELEASE);
  g_trace_release.store(true, std::memory_order_release);
  direct.join();

  node_release(root);
  LangGcStats cleanup{};
  CHECK(lang_gc_runtime_collect_v1(2000000000LL, &cleanup) == 0);
  CHECK(lang_gc_registry_live_v1() == 0);
  CHECK(g_live_objects.load() == 0);
  CHECK(lang_gc_world_unregister_v1());
  expect_live(0);
  std::printf("GC_RUNTIME_KERNEL_BUSY_OK kernel_busy=%llu\n",
              static_cast<unsigned long long>(after.kernel_busy));
}

} // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::setvbuf(stderr, nullptr, _IONBF, 0);

  test_auto_reclaim_8192_garbage_cycles();
  test_growing_root_budget_is_not_quadratic();
  test_critical_safe_timeout_then_collect();
  test_queue_reentry_nested_collect();
  test_register_during_collect_preserves_pressure();
  test_non_gc_defer_releases_directly();
  test_non_collector_cannot_enqueue_during_kernel();
  test_invalid_timeout_and_controller_busy_no_graph_change();
  test_kernel_busy_mapping();
  test_fork_idle_controller_reset();
  test_fork_pending_stop_controller_reset();
  test_fork_from_drain_aborts_child_releases();

  CHECK(g_callback_bad_world.load() == 0);
  CHECK(g_live_objects.load() == 0);
  expect_live(0);
  std::printf("GC_RUNTIME_HOST_OK allocations=%llu\n",
              static_cast<unsigned long long>(gc_test_alloc_total()));
  return 0;
}
