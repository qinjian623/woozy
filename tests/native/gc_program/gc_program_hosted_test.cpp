// Hosted C++ runtime tests for direct libc fork and std/process.fork through
// lang-gc-program/1. These are runtime tests, not Source backend evidence.

#include "alloc.hpp"
#include "gc.hpp"
#include "gc_program.hpp"
#include "gc_runtime.hpp"
#include "gc_world.hpp"
#include "process.hpp"
#include "weak.hpp"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

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

constexpr uint64_t kSentinel = 0x484f53545f505247ULL;
constexpr uint64_t kU64Max = ~uint64_t{0};
std::atomic<uint64_t> g_live_nodes{0};

struct Node {
  uint64_t refs;
  Node *edge;
  uint64_t sentinel;
  uint64_t payload[123];
};
static_assert(sizeof(Node) >= 1000);

void node_retain(Node *node) noexcept {
  CHECK(__atomic_load_n(&node->refs, __ATOMIC_RELAXED) != 0);
  CHECK(__atomic_load_n(&node->refs, __ATOMIC_RELAXED) != kU64Max);
  __atomic_add_fetch(&node->refs, 1u, __ATOMIC_RELAXED);
}

void retain_op(void *object) noexcept { node_retain(static_cast<Node *>(object)); }

void node_release(Node *node) noexcept {
  if (__atomic_sub_fetch(&node->refs, 1u, __ATOMIC_ACQ_REL) != 0)
    return;
  lang_gc_program_unregister_v1(node);
  g_live_nodes.fetch_sub(1, std::memory_order_relaxed);
  lang_free_v1(node);
}

void release_op(void *object) noexcept {
  node_release(static_cast<Node *>(object));
}

void trace(void *object, LangGcVisitor visitor, void *context) noexcept {
  auto *node = static_cast<Node *>(object);
  if (node->edge)
    visitor(node->edge, context);
}

void clear(void *object) noexcept {
  auto *node = static_cast<Node *>(object);
  Node *edge = node->edge;
  node->edge = nullptr;
  if (edge)
    node_release(edge);
}

void invalidate(void *object) noexcept { (void)object; }

const LangGcOps kOps = {trace, clear, retain_op, release_op, invalidate};

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

void make_garbage_self() {
  Node *node = node_new();
  node->edge = node;
  node_retain(node);
  CHECK(lang_gc_program_register_v1(node, &kOps));
  node_release(node);
}

bool install_filter(const sock_filter *filter, unsigned short count) {
  sock_fprog program{count, const_cast<sock_filter *>(filter)};
  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
    return false;
  return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0;
}

bool deny_fork_syscalls() {
  const sock_filter filter[] = {
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 56, 1, 0), // clone
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 57, 0, 1), // fork
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
  };
  return install_filter(filter, 5);
}

void child_expect_collect() {
  CHECK(lang_gc_world_can_collect_v1());
  CHECK(lang_gc_registry_live_v1() == 1);
  LangGcStats stats{};
  CHECK(lang_gc_runtime_collect_v1(2000000000LL, &stats) == 0);
  CHECK(stats.collected == 1);
  CHECK(lang_gc_registry_live_v1() == 0);
  std::puts("GC_PROGRAM_HOSTED_CHILD_OK");
  std::fflush(stdout);
  _exit(0);
}

int mode_language_fork() {
  CHECK(lang_gc_program_init_v1() == LANG_GC_PROGRAM_OK);
  make_garbage_self();
  const int64_t pid = lang_process_fork_v1();
  if (pid == 0)
    child_expect_collect();
  CHECK(pid > 0);
  int status = -1;
  CHECK(waitpid(static_cast<pid_t>(pid), &status, 0) == pid);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  LangGcStats stats{};
  CHECK(lang_gc_runtime_collect_v1(2000000000LL, &stats) == 0);
  CHECK(stats.collected == 1);
  CHECK(lang_gc_program_finish_v1() == LANG_GC_PROGRAM_OK);
  std::puts("GC_PROGRAM_HOSTED_LANGUAGE_FORK_OK");
  return 0;
}

int mode_libc_fork() {
  CHECK(lang_gc_program_init_v1() == LANG_GC_PROGRAM_OK);
  make_garbage_self();
  const pid_t pid = ::fork();
  if (pid == 0)
    child_expect_collect();
  CHECK(pid > 0);
  int status = -1;
  CHECK(waitpid(pid, &status, 0) == pid);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  LangGcStats stats{};
  CHECK(lang_gc_runtime_collect_v1(2000000000LL, &stats) == 0);
  CHECK(stats.collected == 1);
  CHECK(lang_gc_program_finish_v1() == LANG_GC_PROGRAM_OK);
  std::puts("GC_PROGRAM_HOSTED_LIBC_FORK_OK");
  return 0;
}

int mode_libc_fork_safe() {
  CHECK(lang_gc_program_init_v1() == LANG_GC_PROGRAM_OK);
  CHECK(lang_gc_program_enter_safe_v1() == 1);
  const pid_t pid = ::fork();
  if (pid == 0) {
    CHECK(!lang_gc_world_can_collect_v1()); // atfork restored outer SAFE
    lang_gc_program_leave_safe_v1(1);
    CHECK(lang_gc_world_can_collect_v1());
    _exit(0);
  }
  CHECK(pid > 0);
  CHECK(!lang_gc_world_can_collect_v1());
  lang_gc_program_leave_safe_v1(1);
  CHECK(lang_gc_world_can_collect_v1());
  int status = -1;
  CHECK(waitpid(pid, &status, 0) == pid);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  CHECK(lang_gc_program_finish_v1() == LANG_GC_PROGRAM_OK);
  std::puts("GC_PROGRAM_HOSTED_LIBC_FORK_SAFE_OK");
  return 0;
}

int mode_failure_parent_restore() {
  CHECK(lang_gc_program_init_v1() == LANG_GC_PROGRAM_OK);
  make_garbage_self();
  CHECK(deny_fork_syscalls());
  const int64_t pid = lang_process_fork_v1();
  CHECK(pid < 0); // EPERM; libc and language parent hooks must have run
  CHECK(lang_gc_world_can_collect_v1());
  LangGcStats stats{};
  CHECK(lang_gc_runtime_collect_v1(2000000000LL, &stats) == 0);
  CHECK(stats.collected == 1);
  CHECK(lang_gc_program_finish_v1() == LANG_GC_PROGRAM_OK);
  std::puts("GC_PROGRAM_HOSTED_FORK_FAILURE_OK");
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  if (std::strcmp(argv[1], "language") == 0)
    return mode_language_fork();
  if (std::strcmp(argv[1], "libc") == 0)
    return mode_libc_fork();
  if (std::strcmp(argv[1], "libc-safe") == 0)
    return mode_libc_fork_safe();
  if (std::strcmp(argv[1], "failure") == 0)
    return mode_failure_parent_restore();
  return 2;
}
