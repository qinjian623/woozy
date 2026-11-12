// C++ runtime tests for lang-gc-program/1.
//
// These are runtime/ABI tests, not Source backend tests. They construct real
// LangVec objects and language threads with the actual runtime, but they do not
// compile or claim a .wz source program. The Source pressure target remains RED
// until the Native emitter emits registration, polls, critical regions and the
// main wrapper calls.

#include "alloc.hpp"
#include "gc.hpp"
#include "gc_program.hpp"
#include "gc_runtime.hpp"
#include "gc_world.hpp"
#include "os.hpp"
#include "process.hpp"
#include "sync.hpp"
#include "system.hpp"
#include "thread.hpp"
#include "vec.hpp"
#include "weak.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <unistd.h>

extern "C" uint64_t gc_test_alloc_live() noexcept;

namespace {

using namespace lang::native::gc_program;

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

constexpr uint64_t kSentinel = 0x4e4f44455f505247ULL;
constexpr uint64_t kU64Max = ~uint64_t{0};

std::atomic<uint64_t> g_live_nodes{0};
std::atomic<uint64_t> g_vec_drops{0};
std::atomic<uint64_t> g_vec_drop_registry_live{kU64Max};
std::atomic<uint64_t> g_probe_drops{0};
std::atomic<uint64_t> g_probe_drop_registry_live{kU64Max};
std::atomic<bool> g_waiter_ready{false};
std::atomic<bool> g_stop_ok{false};
std::atomic<bool> g_fork_started{false};

void sleep_ms(int ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

bool wait_flag(const std::atomic<bool> &flag, int ms) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (flag.load(std::memory_order_acquire))
      return true;
    sleep_ms(1);
  }
  return flag.load(std::memory_order_acquire);
}

uint64_t registry_live() { return lang_gc_registry_live_v1(); }

struct Node {
  uint64_t refs;
  Node *edge;
  uint64_t sentinel;
  uint64_t payload[123];
};
static_assert(offsetof(Node, refs) == 0);
static_assert(sizeof(Node) >= 1000);

void node_retain(Node *node) noexcept {
  CHECK(__atomic_load_n(&node->refs, __ATOMIC_RELAXED) != 0);
  CHECK(__atomic_load_n(&node->refs, __ATOMIC_RELAXED) != kU64Max);
  __atomic_add_fetch(&node->refs, 1u, __ATOMIC_RELAXED);
}

void node_retain_op(void *object) noexcept {
  node_retain(static_cast<Node *>(object));
}

void node_release(Node *node) noexcept {
  if (__atomic_sub_fetch(&node->refs, 1u, __ATOMIC_ACQ_REL) != 0)
    return;
  // A normal final strong release unregisters before payload cleanup.
  lang_gc_program_unregister_v1(node);
  g_live_nodes.fetch_sub(1, std::memory_order_relaxed);
  lang_free_v1(node);
}

void node_release_op(void *object) noexcept {
  node_release(static_cast<Node *>(object));
}

void node_trace(void *object, LangGcVisitor visitor, void *context) noexcept {
  auto *node = static_cast<Node *>(object);
  if (node->edge)
    visitor(node->edge, context);
}

void node_clear(void *object) noexcept {
  auto *node = static_cast<Node *>(object);
  Node *edge = node->edge;
  node->edge = nullptr; // publish the new state before external release
  if (edge)
    node_release(edge);
}

void node_invalidate(void *object) noexcept {
  (void)object; // these nodes have no Weak target in this test
}

const LangGcOps kNodeOps = {node_trace, node_clear, node_retain_op,
                            node_release_op, node_invalidate};

Node *node_new() {
  auto *node = static_cast<Node *>(lang_alloc_v1(sizeof(Node), alignof(Node)));
  CHECK(node != nullptr);
  std::memset(node, 0, sizeof(*node));
  node->refs = 1;
  node->sentinel = kSentinel;
  node->payload[0] = kSentinel;
  node->payload[122] = kSentinel;
  g_live_nodes.fetch_add(1, std::memory_order_relaxed);
  return node;
}

// A real self-cycle whose only strong edge is node->edge == node. The external
// reference is dropped, so only the collector can reclaim it.
Node *make_garbage_self() {
  Node *node = node_new();
  node->edge = node;
  node_retain(node);
  CHECK(lang_gc_program_register_v1(node, &kNodeOps));
  node_release(node);
  return node;
}

// Vec operations: trace only the initialized prefix and publish an empty
// prefix before dropping old elements. This mirrors the contract the Native
// emitter must follow; it is not a compiler implementation.
void vec_slot_retain(void *slot) noexcept {
  auto *value = static_cast<LangVec **>(slot);
  if (*value)
    lang_vec_retain_v1(*value);
}

void vec_slot_drop(void *slot) noexcept {
  auto *value = static_cast<LangVec **>(slot);
  if (!*value)
    return;
  LangVec *target = *value;
  *value = nullptr;
  g_vec_drops.fetch_add(1, std::memory_order_relaxed);
  g_vec_drop_registry_live.store(registry_live(), std::memory_order_release);
  lang_vec_release_v1(target);
}

void probe_drop(void *slot) noexcept {
  (void)slot;
  g_probe_drops.fetch_add(1, std::memory_order_relaxed);
  g_probe_drop_registry_live.store(registry_live(), std::memory_order_release);
}

void vec_trace(void *object, LangGcVisitor visitor, void *context) noexcept {
  auto *vec = static_cast<LangVec *>(object);
  for (uint64_t index = 0; index < vec->initialized; ++index) {
    auto *value = reinterpret_cast<LangVec **>(
        static_cast<unsigned char *>(vec->data) + index * vec->stride);
    if (*value)
      visitor(*value, context);
  }
}

void vec_clear(void *object) noexcept {
  auto *vec = static_cast<LangVec *>(object);
  const uint64_t initialized = vec->initialized;
  // Publish the new empty prefix before running any element drop. A drop may
  // re-enter Woozy; it must not observe a stale element.
  vec->initialized = 0;
  vec->length = 0;
  for (uint64_t index = 0; index < initialized; ++index) {
    if (vec->drop)
      vec->drop(static_cast<unsigned char *>(vec->data) + index * vec->stride);
  }
}

void vec_invalidate(void *object) noexcept {
  auto *vec = static_cast<LangVec *>(object);
  lang_weak_invalidate_slot_v1(&vec->weak_control);
}

// Vec's retain/release ops are distinct from Node's. The collector calls these
// on the LangVec itself.
void vec_retain_op(void *object) noexcept {
  lang_vec_retain_v1(static_cast<LangVec *>(object));
}
void vec_release_op(void *object) noexcept {
  lang_vec_release_v1(static_cast<LangVec *>(object));
}
const LangGcOps kVecSelfOps = {vec_trace, vec_clear, vec_retain_op,
                               vec_release_op, vec_invalidate};

bool install_filter(const sock_filter *filter, unsigned short count) {
  sock_fprog program{count, const_cast<sock_filter *>(filter)};
  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
    return false;
  return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0;
}

bool deny_gettid() {
  const sock_filter filter[] = {
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 186, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
  };
  return install_filter(filter, 4);
}

bool deny_fork_syscalls() {
  const sock_filter filter[] = {
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 56, 1, 0), // clone -> deny
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 57, 0, 1), // fork -> deny
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
  };
  return install_filter(filter, 5);
}

int mode_lifecycle() {
  CHECK(!enabled());
  CHECK(registry_live() == 0);

  // A primitive Vec must not be registered by the disabled default.
  LangVec *plain = lang_vec_new_v1(4, sizeof(uint64_t), nullptr, nullptr);
  CHECK(plain != nullptr);
  CHECK(!lang_gc_program_register_v1(plain, &kVecSelfOps));
  CHECK(registry_live() == 0);
  lang_vec_release_v1(plain);

  CHECK(lang_gc_program_init_v1() == LANG_GC_PROGRAM_OK);
  CHECK(enabled());
  CHECK(lang_gc_program_init_v1() == LANG_GC_PROGRAM_ERROR_STATE);
  CHECK(lang_gc_world_can_collect_v1());

  Node *garbage = make_garbage_self();
  CHECK(registry_live() == 1);
  CHECK(lang_gc_program_finish_v1() == LANG_GC_PROGRAM_OK);
  // Hooks/bookkeeping stay installed for surviving language threads. Process
  // exit, not main finish, is the shutdown boundary.
  CHECK(enabled());
  CHECK(registry_live() == 0);
  CHECK(g_live_nodes.load(std::memory_order_relaxed) == 0);
  CHECK(!lang_gc_world_can_collect_v1()); // main registration was removed
  CHECK(lang_gc_program_finish_v1() == LANG_GC_PROGRAM_ERROR_STATE);
  LangVec *after_finish = lang_vec_new_v1(1, 1, nullptr, nullptr);
  CHECK(lang_gc_program_register_v1(after_finish, &kVecSelfOps));
  lang_vec_release_v1(after_finish); // final release still unregisters
  CHECK(registry_live() == 0);
  {
    // Main hooks are no-ops after finish because main is no longer registered.
    SafeGuard safe;
    CHECK(lang_gc_program_enter_critical_v1() == 0);
    lang_gc_program_leave_critical_v1(0);
  }
  (void)garbage;

  std::puts("GC_PROGRAM_LIFECYCLE_OK");
  return 0;
}

int mode_vec_cycle() {
  CHECK(lang_gc_program_init_v1() == LANG_GC_PROGRAM_OK);
  const uint64_t allocation_baseline = gc_test_alloc_live();

  // Normal final strong release of a registered Vec: unregister first, then
  // drop elements. The probe element is deliberately not an owning edge.
  LangVec *probe = lang_vec_new_v1(1, sizeof(uint64_t), nullptr, probe_drop);
  CHECK(probe != nullptr);
  uint64_t zero = 0;
  lang_vec_init_v1(probe, &zero);
  CHECK(lang_gc_program_register_v1(probe, &kVecSelfOps));
  CHECK(registry_live() == 1);
  lang_vec_release_v1(probe);
  CHECK(g_probe_drops.load(std::memory_order_relaxed) == 1);
  CHECK(g_probe_drop_registry_live.load(std::memory_order_acquire) == 0);
  CHECK(registry_live() == 0);
  CHECK(gc_test_alloc_live() == allocation_baseline);

  LangVec *vec = lang_vec_new_v1(2, sizeof(LangVec *), vec_slot_retain,
                                 vec_slot_drop);
  CHECK(vec != nullptr);
  LangVec *self = vec;
  lang_vec_init_v1(vec, &self);
  vec_slot_retain(static_cast<unsigned char *>(vec->data) + vec->stride * 0);
  // Slot 1 is beyond the initialized prefix. A trace that walks length
  // instead of initialized would see this poison pointer.
  *reinterpret_cast<void **>(static_cast<unsigned char *>(vec->data) +
                             vec->stride) = reinterpret_cast<void *>(0x1);
  CHECK(vec->length == 2 && vec->initialized == 1);
  CHECK(lang_gc_program_register_v1(vec, &kVecSelfOps));
  lang_vec_release_v1(vec); // drop the external reference; the self-edge remains
  CHECK(registry_live() == 1);

  LangGcStats stats{};
  CHECK(lang_gc_runtime_collect_v1(2000000000LL, &stats) == 0);
  CHECK(stats.registered == 1);
  CHECK(stats.traced_edges == 1); // only the initialized prefix slot
  CHECK(stats.candidates == 1);
  CHECK(stats.collected == 1);
  CHECK(registry_live() == 0);
  CHECK(g_vec_drops.load(std::memory_order_relaxed) == 1);
  // The collector still pins the candidate during clear, so this drop observes
  // the registered object. The later final pin release unregisters it.
  CHECK(g_vec_drop_registry_live.load(std::memory_order_acquire) == 1);
  CHECK(gc_test_alloc_live() == allocation_baseline);

  CHECK(lang_gc_program_finish_v1() == LANG_GC_PROGRAM_OK);
  std::puts("GC_PROGRAM_VEC_OK");
  return 0;
}

int mode_pressure() {
  CHECK(lang_gc_program_init_v1() == LANG_GC_PROGRAM_OK);
  Node *nodes[128];
  for (auto *&node : nodes) {
    node = node_new();
    CHECK(lang_gc_program_register_v1(node, &kNodeOps));
  }
  LangGcRuntimeStats stats{};
  lang_gc_runtime_stats_v1(&stats);
  CHECK(stats.pressure_accum == 128);
  CHECK(lang_gc_registry_live_v1() == 128);
  for (auto *node : nodes)
    node_release(node);
  CHECK(lang_gc_registry_live_v1() == 0);
  CHECK(lang_gc_program_finish_v1() == LANG_GC_PROGRAM_OK);
  std::puts("GC_PROGRAM_PRESSURE_OK");
  return 0;
}

int mode_finish_survivor() {
  CHECK(lang_gc_program_init_v1() == LANG_GC_PROGRAM_OK);
  std::atomic<bool> ready{false};
  std::atomic<bool> go{false};
  std::thread worker([&] {
    // A detached/std thread is not a language runner, so it registers
    // explicitly. Its nodes use the production final-release path through
    // lang_gc_program_unregister_v1.
    CHECK(lang_gc_world_register_v1());
    Node *nodes[128];
    for (auto *&node : nodes) {
      node = node_new();
      CHECK(lang_gc_runtime_register_v1(node, &kNodeOps) == LANG_GC_OK);
    }
    ready.store(true, std::memory_order_release);
    while (!go.load(std::memory_order_acquire))
      std::this_thread::yield();
    for (auto *node : nodes)
      node_release(node);
    CHECK(lang_gc_registry_live_v1() == 0);
    // Pressure is still pending. The worker must be able to poll/collect an
    // empty registry without reading freed nodes or stale entries.
    lang_gc_runtime_poll_v1();
    CHECK(lang_gc_registry_live_v1() == 0);
    CHECK(lang_gc_world_unregister_v1());
  });
  while (!ready.load(std::memory_order_acquire))
    std::this_thread::yield();
  const int64_t status = lang_gc_program_finish_v1();
  CHECK(status == LANG_GC_PROGRAM_TIMED_OUT);
  CHECK(enabled()); // surviving worker still needs unregister bookkeeping
  go.store(true, std::memory_order_release);
  worker.join();
  CHECK(lang_gc_registry_live_v1() == 0);
  CHECK(enabled());
  CHECK(lang_gc_program_finish_v1() == LANG_GC_PROGRAM_ERROR_STATE);
  std::puts("GC_PROGRAM_FINISH_SURVIVOR_OK");
  return 0;
}

struct ThreadContext {
  int fd;
  Node *node;
  uint64_t observed;
};

void context_copy(void *destination) noexcept {
  auto *ctx = static_cast<ThreadContext *>(destination);
  node_retain(ctx->node);
}

void context_drop(void *destination) noexcept {
  auto *ctx = static_cast<ThreadContext *>(destination);
  node_release(ctx->node);
  ctx->node = nullptr;
}

int64_t blocking_reader(void *argument) noexcept {
  auto *ctx = static_cast<ThreadContext *>(argument);
  ctx->observed = ctx->node->sentinel;
  g_waiter_ready.store(true, std::memory_order_release);
  unsigned char byte = 0;
  const int64_t result = lang_os_read_v1(ctx->fd, &byte, 1);
  if (ctx->node->sentinel != kSentinel)
    return -1;
  return result == 1 ? static_cast<int64_t>(byte) : result;
}

int mode_thread_safe_root() {
  CHECK(lang_gc_program_init_v1() == LANG_GC_PROGRAM_OK);
  int fds[2] = {-1, -1};
  CHECK(pipe(fds) == 0);

  Node *node = node_new();
  CHECK(lang_gc_program_register_v1(node, &kNodeOps));
  ThreadContext ctx{fds[0], node, 0};
  LangHandle *thread = lang_thread_spawn_v1(&blocking_reader, &ctx, sizeof(ctx),
                                            &context_copy, &context_drop);
  CHECK(thread != nullptr);
  CHECK(lang_thread_error_v1(thread) == 0);
  node_release(node); // the copied context now owns the only strong reference
  CHECK(g_live_nodes.load(std::memory_order_relaxed) == 1);
  CHECK(wait_flag(g_waiter_ready, 2000));

  // The reader is SAFE in read(2). The registered node is kept only by the
  // ThreadState context, which is deliberately not a registered graph object;
  // its strong count makes it an external root.
  LangGcStats stats{};
  CHECK(lang_gc_runtime_collect_v1(2000000000LL, &stats) == 0);
  CHECK(stats.registered == 1);
  CHECK(stats.collected == 0);
  CHECK(node->sentinel == kSentinel);
  CHECK(registry_live() == 1);

  unsigned char byte = 7;
  CHECK(write(fds[1], &byte, 1) == 1);
  CHECK(lang_thread_wait_v1(thread) == 0);
  CHECK(lang_thread_result_v1(thread) == 7);
  // context_drop ran before thread completion and released the node.
  CHECK(g_live_nodes.load(std::memory_order_relaxed) == 0);
  CHECK(registry_live() == 0);

  lang_handle_release_v1(thread);
  close(fds[0]);
  close(fds[1]);
  CHECK(lang_gc_program_finish_v1() == LANG_GC_PROGRAM_OK);
  std::puts("GC_PROGRAM_THREAD_OK");
  return 0;
}

struct MutexContext {
  LangHandle *mutex;
};

int64_t mutex_waiter(void *argument) noexcept {
  auto *ctx = static_cast<MutexContext *>(argument);
  g_waiter_ready.store(true, std::memory_order_release);
  lang_mutex_lock_v1(ctx->mutex);
  lang_mutex_unlock_v1(ctx->mutex);
  return 0;
}

int64_t stop_and_hold(void *argument) noexcept {
  (void)argument;
  g_stop_ok.store(lang_gc_world_stop_v1(2000000000LL) == 0,
                  std::memory_order_release);
  while (!g_fork_started.load(std::memory_order_acquire)) {
    // Sleep in SAFE while owning the stop; the forking thread must park in
    // its program fork scope and wait for this resume.
    LangTimespec wait{0, 1000000};
    LangTimespec unused{};
    (void)lang_sys_nanosleep_v1(&wait, &unused);
  }
  lang_gc_world_resume_v1();
  return 0;
}

int mode_fork_stop_active() {
  CHECK(lang_gc_program_init_v1() == LANG_GC_PROGRAM_OK);
  std::atomic<int64_t> status{999};
  std::thread collector([&] {
    CHECK(lang_gc_world_register_v1());
    const int64_t result = lang_gc_world_stop_v1(2000000000LL);
    status.store(result, std::memory_order_release);
    if (result == 0) lang_gc_world_resume_v1();
    CHECK(lang_gc_world_unregister_v1());
  });
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(2);
  while (!lang_gc_world_poll_required_v1()) {
    CHECK(std::chrono::steady_clock::now() < deadline);
    std::this_thread::yield();
  }
  CHECK(status.load(std::memory_order_acquire) == 999);
  LangGcProgramForkToken token{};
  lang_gc_program_fork_enter_v1(&token);
  // A pending stop must finish before fork acquires any internal runtime lock.
  const int64_t before_locks = status.load(std::memory_order_acquire);
  lang_gc_program_fork_leave_v1(&token);
  collector.join();
  CHECK(before_locks == 0);
  CHECK(lang_gc_program_finish_v1() == LANG_GC_PROGRAM_OK);
  std::puts("GC_PROGRAM_FORK_STOP_ACTIVE_OK");
  return 0;
}

int mode_fork_stop_safe() {
  CHECK(lang_gc_program_init_v1() == LANG_GC_PROGRAM_OK);
  // Main is SAFE before the stopper starts, so the stopper can own a real
  // stop while main has an outer SAFE depth to suspend.
  CHECK(lang_gc_program_enter_safe_v1() == 1);
  LangHandle *stopper = lang_thread_spawn_v1(&stop_and_hold, nullptr, 0,
                                              nullptr, nullptr);
  CHECK(stopper != nullptr);
  CHECK(wait_flag(g_stop_ok, 2000));
  g_fork_started.store(true, std::memory_order_release);
  const int64_t pid = lang_process_fork_v1();
  if (pid == 0) {
    CHECK(!lang_gc_world_can_collect_v1()); // outer SAFE restored, stop gone
    lang_gc_program_leave_safe_v1(1);
    CHECK(lang_gc_world_can_collect_v1());
    lang_os_exit_v1(0);
  }
  CHECK(pid > 0);
  CHECK(!lang_gc_world_can_collect_v1());
  lang_gc_program_leave_safe_v1(1);
  CHECK(lang_gc_world_can_collect_v1());
  CHECK(lang_thread_wait_v1(stopper) == 0);
  lang_handle_release_v1(stopper);
  int32_t status = -1;
  CHECK(lang_process_waitpid_v1(static_cast<int32_t>(pid), 0, &status) == pid);
  CHECK(status == 0);
  CHECK(lang_gc_program_finish_v1() == LANG_GC_PROGRAM_OK);
  std::puts("GC_PROGRAM_FORK_STOP_SAFE_OK");
  return 0;
}

int mode_blocking_mutex() {
  CHECK(lang_gc_program_init_v1() == LANG_GC_PROGRAM_OK);
  LangHandle *mutex = lang_mutex_new_v1();
  CHECK(mutex != nullptr);
  lang_mutex_lock_v1(mutex); // main owns the mutex

  MutexContext ctx{mutex};
  LangHandle *thread = lang_thread_spawn_v1(&mutex_waiter, &ctx, sizeof(ctx),
                                            nullptr, nullptr);
  CHECK(thread != nullptr);
  CHECK(wait_flag(g_waiter_ready, 2000));
  sleep_ms(20); // let the waiter enter the contended futex SAFE region

  LangGcStats stats{};
  CHECK(lang_gc_runtime_collect_v1(2000000000LL, &stats) == 0);
  CHECK(stats.registered == 0);

  lang_mutex_unlock_v1(mutex);
  CHECK(lang_thread_wait_v1(thread) == 0);
  lang_handle_release_v1(thread);
  lang_handle_release_v1(mutex);
  CHECK(lang_gc_program_finish_v1() == LANG_GC_PROGRAM_OK);
  std::puts("GC_PROGRAM_MUTEX_OK");
  return 0;
}

int mode_suspend_restore() {
  CHECK(lang_gc_program_init_v1() == LANG_GC_PROGRAM_OK);
  CHECK(lang_gc_program_enter_safe_v1() == 1);
  CHECK(lang_gc_program_enter_safe_v1() == 1);
  CHECK(!lang_gc_world_can_collect_v1());
  CHECK(lang_gc_world_suspend_safe_v1() == 2);
  CHECK(lang_gc_world_can_collect_v1());
  lang_gc_world_restore_safe_v1(2);
  CHECK(!lang_gc_world_can_collect_v1());
  lang_gc_program_leave_safe_v1(1);
  lang_gc_program_leave_safe_v1(1);
  CHECK(lang_gc_world_can_collect_v1());
  CHECK(lang_gc_program_finish_v1() == LANG_GC_PROGRAM_OK);
  std::puts("GC_PROGRAM_SUSPEND_OK");
  return 0;
}

int fork_child_collect(const char *marker) {
  CHECK(lang_gc_world_can_collect_v1());
  CHECK(registry_live() == 1);
  LangGcStats stats{};
  CHECK(lang_gc_runtime_collect_v1(2000000000LL, &stats) == 0);
  CHECK(stats.collected == 1);
  CHECK(registry_live() == 0);
  std::puts(marker);
  std::fflush(stdout);
  lang_os_exit_v1(0);
}

int mode_fork_native() {
  CHECK(lang_gc_program_init_v1() == LANG_GC_PROGRAM_OK);
  (void)make_garbage_self();
  CHECK(registry_live() == 1);
  const int64_t pid = lang_process_fork_v1();
  if (pid == 0)
    fork_child_collect("GC_PROGRAM_FORK_CHILD_OK");
  CHECK(pid > 0);
  int32_t status = -1;
  CHECK(lang_process_waitpid_v1(static_cast<int32_t>(pid), 0, &status) == pid);
  CHECK(status == 0);
  LangGcStats stats{};
  CHECK(lang_gc_runtime_collect_v1(2000000000LL, &stats) == 0);
  CHECK(stats.collected == 1);
  CHECK(registry_live() == 0);
  CHECK(lang_gc_program_finish_v1() == LANG_GC_PROGRAM_OK);
  std::puts("GC_PROGRAM_FORK_OK");
  return 0;
}

int mode_fork_safe() {
  CHECK(lang_gc_program_init_v1() == LANG_GC_PROGRAM_OK);
  CHECK(lang_gc_program_enter_safe_v1() == 1);
  const int64_t pid = lang_process_fork_v1();
  if (pid == 0) {
    CHECK(!lang_gc_world_can_collect_v1()); // outer SAFE restored exactly
    lang_gc_program_leave_safe_v1(1);
    CHECK(lang_gc_world_can_collect_v1());
    lang_os_exit_v1(0);
  }
  CHECK(pid > 0);
  CHECK(!lang_gc_world_can_collect_v1());
  lang_gc_program_leave_safe_v1(1);
  CHECK(lang_gc_world_can_collect_v1());
  int32_t status = -1;
  CHECK(lang_process_waitpid_v1(static_cast<int32_t>(pid), 0, &status) == pid);
  CHECK(status == 0);
  CHECK(lang_gc_program_finish_v1() == LANG_GC_PROGRAM_OK);
  std::puts("GC_PROGRAM_FORK_SAFE_OK");
  return 0;
}

int mode_fork_critical() {
  CHECK(lang_gc_program_init_v1() == LANG_GC_PROGRAM_OK);
  CHECK(lang_gc_program_enter_critical_v1() == 1);
  const int64_t pid = lang_process_fork_v1();
  if (pid == 0) {
    CHECK(!lang_gc_world_can_collect_v1()); // outer critical depth preserved
    lang_gc_program_leave_critical_v1(1);
    CHECK(lang_gc_world_can_collect_v1());
    lang_os_exit_v1(0);
  }
  CHECK(pid > 0);
  CHECK(!lang_gc_world_can_collect_v1());
  lang_gc_program_leave_critical_v1(1);
  CHECK(lang_gc_world_can_collect_v1());
  int32_t status = -1;
  CHECK(lang_process_waitpid_v1(static_cast<int32_t>(pid), 0, &status) == pid);
  CHECK(status == 0);
  CHECK(lang_gc_program_finish_v1() == LANG_GC_PROGRAM_OK);
  std::puts("GC_PROGRAM_FORK_CRITICAL_OK");
  return 0;
}

int mode_fork_failure() {
  CHECK(lang_gc_program_init_v1() == LANG_GC_PROGRAM_OK);
  (void)make_garbage_self();
  CHECK(deny_fork_syscalls());
  const int64_t pid = lang_process_fork_v1();
  CHECK(pid < 0); // EPERM from seccomp; parent hooks must restore all locks
  CHECK(lang_gc_world_can_collect_v1());
  LangGcStats stats{};
  CHECK(lang_gc_runtime_collect_v1(2000000000LL, &stats) == 0);
  CHECK(stats.collected == 1);
  CHECK(registry_live() == 0);
  CHECK(lang_gc_program_finish_v1() == LANG_GC_PROGRAM_OK);
  std::puts("GC_PROGRAM_FORK_FAILURE_OK");
  return 0;
}

int mode_disabled_fast() {
  CHECK(deny_gettid());
  CHECK(!enabled());
  {
    SafeGuard guard;
  }
  {
    CriticalGuard guard;
  }
  LangVec *vec = lang_vec_new_v1(0, 1, nullptr, nullptr);
  CHECK(vec != nullptr);
  CHECK(!lang_gc_program_register_v1(vec, &kVecSelfOps));
  lang_gc_program_unregister_v1(vec);
  lang_vec_release_v1(vec); // no gettid, no registry lookup
  CHECK(lang_gc_program_thread_enter_v1() == 0);
  lang_gc_program_thread_leave_v1(0);
  LangGcProgramForkToken token{};
  lang_gc_program_fork_enter_v1(&token);
  lang_gc_program_fork_leave_v1(&token);
  std::puts("GC_PROGRAM_DISABLED_FAST_OK");
  return 0;
}

int mode_suspend_critical() {
  CHECK(lang_gc_program_init_v1() == LANG_GC_PROGRAM_OK);
  CHECK(lang_gc_program_enter_critical_v1() == 1);
  CHECK(lang_gc_world_suspend_safe_v1() == 0);
  lang_gc_program_leave_critical_v1(1);
  CHECK(lang_gc_world_can_collect_v1());
  CHECK(lang_gc_program_finish_v1() == LANG_GC_PROGRAM_OK);
  std::puts("GC_PROGRAM_SUSPEND_CRITICAL_OK");
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <mode>\n", argv[0]);
    return 2;
  }
  if (std::strcmp(argv[1], "lifecycle") == 0)
    return mode_lifecycle();
  if (std::strcmp(argv[1], "vec") == 0)
    return mode_vec_cycle();
  if (std::strcmp(argv[1], "pressure") == 0)
    return mode_pressure();
  if (std::strcmp(argv[1], "finish-survivor") == 0)
    return mode_finish_survivor();
  if (std::strcmp(argv[1], "thread") == 0)
    return mode_thread_safe_root();
  if (std::strcmp(argv[1], "mutex") == 0)
    return mode_blocking_mutex();
  if (std::strcmp(argv[1], "suspend") == 0)
    return mode_suspend_restore();
  if (std::strcmp(argv[1], "fork") == 0)
    return mode_fork_native();
  if (std::strcmp(argv[1], "fork-safe") == 0)
    return mode_fork_safe();
  if (std::strcmp(argv[1], "fork-critical") == 0)
    return mode_fork_critical();
  if (std::strcmp(argv[1], "fork-stop-active") == 0)
    return mode_fork_stop_active();
  if (std::strcmp(argv[1], "fork-stop-safe") == 0)
    return mode_fork_stop_safe();
  if (std::strcmp(argv[1], "fork-failure") == 0)
    return mode_fork_failure();
  if (std::strcmp(argv[1], "disabled-fast") == 0)
    return mode_disabled_fast();
  if (std::strcmp(argv[1], "suspend-critical") == 0)
    return mode_suspend_critical();
  return 2;
}
