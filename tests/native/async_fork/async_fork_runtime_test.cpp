// Runtime stress checks for the async fork hooks. These use the private
// lang-async/1 ABI and the real Native runtime archive, not a mock executor.
//
// Modes are separate processes because "executor unbound" and "executor bound"
// are process-lifetime states:
//   same-thread  bind the executor to main, then fork repeatedly and use the
//                child's reset ticket lock/TID.
//   race         race a first executor bind against a fork; the reservation gate
//                must make exactly one side win and never copy a TID that was
//                bound after the check.
//   gc           pause the last Task release before GC unregister, then fork.
//                The child must trace the still-registered Future with refs=1.
#include "../../../runtime/native/async.hpp"
#include "../../../runtime/native/gc_program.hpp"
#include "../../../runtime/native/gc.hpp"
#include "../../../runtime/native/process.hpp"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <cstddef>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
std::atomic<void *> g_paused_unregister{nullptr};
std::atomic<bool> g_unregister_entered{false};
std::atomic<bool> g_unregister_resume{false};
}

extern "C" void __real_lang_gc_program_unregister_v1(void *) noexcept;
extern "C" void __wrap_lang_gc_program_unregister_v1(void *object) noexcept {
  if (object == g_paused_unregister.load(std::memory_order_acquire)) {
    g_unregister_entered.store(true, std::memory_order_release);
    while (!g_unregister_resume.load(std::memory_order_acquire))
      std::this_thread::yield();
  }
  __real_lang_gc_program_unregister_v1(object);
}

namespace {
void need(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

void wait_ok(int64_t pid, int expected = 0) {
  need(pid > 0, "fork/spawn failed");
  int32_t status = -1;
  need(lang_process_waitpid_v1(static_cast<int32_t>(pid), 0, &status) == pid,
       "waitpid failed");
  need(WIFEXITED(status) && WEXITSTATUS(status) == expected,
       "child status differs");
}

LangText text(const char *value) {
  return {reinterpret_cast<const uint8_t *>(value), std::strlen(value),
          std::strlen(value), nullptr};
}

bool deny_fork_syscalls() {
  sock_filter filter[] = {
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
               static_cast<unsigned>(offsetof(seccomp_data, nr))),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_clone, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_fork, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
#ifdef SYS_clone3
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_clone3, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
#endif
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)};
  sock_fprog program{
      static_cast<unsigned short>(sizeof(filter) / sizeof(filter[0])), filter};
  return prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 &&
         prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0;
}

void same_thread() {
  // First executor call binds main. There are no tasks: this is the NoGraph
  // scalar/Unit shape that still owns the ticket lock and epoll words.
  lang_async_run_v1();
  for (unsigned i = 0; i < 12; ++i) {
    const auto pid = lang_process_fork_v1();
    if (pid == 0) {
      alarm(10);
      // The child inherited the executor's thread. A final release and a fresh
      // run must not wait on the parent's ticket number.
      void *task = lang_async_task_new_v1();
      lang_async_task_release_v1(task);
      lang_async_run_v1();
      lang_async_shutdown_v1();
      _exit(0);
    }
    wait_ok(pid);
  }
  lang_async_shutdown_v1();
  std::puts("passed: same-thread NoGraph forks reset ticket/TID state");
}

void failed_fork() {
  lang_async_run_v1();
  need(deny_fork_syscalls(), "seccomp filter failed");
  need(lang_process_fork_v1() == -EPERM, "denied fork did not report EPERM");
  // A failed fork must release the executor ticket and the outer reservation:
  // both a second run and a final Task release would hang on a leaked ticket or
  // gate.
  lang_async_run_v1();
  void *task = lang_async_task_new_v1();
  lang_async_task_release_v1(task);
  lang_async_shutdown_v1();
  need(lang_process_fork_v1() == -EPERM,
       "denied fork left the async reservation or ticket lock held");
  std::puts("passed: failed fork restores async lock and reservation");
}

void first_bind_race() {
  std::atomic<bool> go{false};
  std::thread binder([&] {
    while (!go.load(std::memory_order_acquire))
      std::this_thread::yield();
    lang_async_run_v1();
  });
  go.store(true, std::memory_order_release);
  const auto pid = lang_process_fork_v1();
  if (pid == 0) {
    alarm(10);
    _exit(0);
  }
  if (pid > 0) wait_ok(pid);
  else need(pid == -EBUSY, "fork/bind race returned an unexpected error");
  binder.join();
  // The binder now owns the executor. The same-thread fork contract rejects
  // both ordinary language forks and direct task handoff, but exec-only spawn
  // must keep working and must not touch inherited tasks.
  need(lang_process_fork_v1() == -EBUSY,
       "fork accepted an executor owned by another thread");
  auto path = text("/bin/sh");
  LangText arguments[] = {text("sh"), text("-c"), text("exit 3")};
  LangVec *args = lang_vec_new_v1(0, sizeof(LangText), nullptr, nullptr);
  for (const auto &value : arguments) lang_vec_push_v1(args, &value);
  LangVec *env = lang_vec_new_v1(0, sizeof(LangText), nullptr, nullptr);
  const auto spawned = lang_process_spawn_v1(&path, args, env, -1, -1, -1);
  wait_ok(spawned, 3);
  auto missing = text("/definitely/missing/woozy");
  need(lang_process_spawn_v1(&missing, args, env, -1, -1, -1) == -ENOENT,
       "exec-only spawn lost exec errno");
  lang_vec_release_v1(args);
  lang_vec_release_v1(env);
  std::puts("passed: first bind/fork reservation race and exec-only spawn");
}

volatile int g_child_mode = 0;
volatile int g_child_clear = 0;
volatile int g_child_bad_refs = 0;
volatile uint64_t g_child_seen_refs = 0;
std::atomic<int> g_parent_clear{0};

// lang-gc/1 puts the strong count in the first u64 of every registered object.
// A fork child may legitimately inherit a still-registered Future with a
// positive reference (its only owner was the vanished releasing thread), but a
// registered zero-ref node is an illegal snapshot. This trace runs before the
// collector's own refs check, so it can report that distinction.
void gc_trace(void *object, LangGcVisitor, void *) noexcept {
  if (!g_child_mode) return;
  const auto refs = __atomic_load_n(static_cast<uint64_t *>(object),
                                    __ATOMIC_ACQUIRE);
  g_child_seen_refs = refs;
  if (refs == 0) g_child_bad_refs = 1;
}
void gc_clear(void *) noexcept {
  if (g_child_mode) g_child_clear = 1;
  else g_parent_clear.fetch_add(1, std::memory_order_relaxed);
}

void gc_window() {
  need(lang_gc_program_init_v1() == 0, "gc init failed");
  const LangAsyncFrameGcOps ops{&gc_trace, &gc_clear};
  void *task = lang_async_task_new_v1();
  need(task != nullptr, "task allocation failed");
  lang_async_publish_v2(task, nullptr, nullptr, nullptr, nullptr, task, &ops);
  need(lang_gc_registry_live_v1() == 1, "registered task missing");

  std::atomic<bool> release_done{false};
  // Pause before the real unregister acquires the registry lock. The fork
  // therefore always copies a registered Future, rather than racing sleeps.
  g_paused_unregister.store(task, std::memory_order_release);
  std::thread releaser([&] {
    // The async contract requires a GC-visible final release on a thread that
    // is already registered with lang-gc-world/1.
    const int32_t registered = lang_gc_program_thread_enter_v1();
    lang_async_task_release_v1(task);
    release_done.store(true, std::memory_order_release);
    lang_gc_program_thread_leave_v1(registered);
  });
  while (!g_unregister_entered.load(std::memory_order_acquire))
    std::this_thread::yield();
  need(__atomic_load_n(static_cast<uint64_t *>(task), __ATOMIC_ACQUIRE) == 1,
       "last release published zero before GC unregister");
  const auto pid = lang_process_fork_v1();
  if (pid == 0) {
    alarm(10);
    g_paused_unregister.store(nullptr, std::memory_order_release);
    g_child_mode = 1;
    const uint64_t registered = lang_gc_registry_live_v1();
    if (registered != 1) _exit(3);
    const auto finished = lang_gc_program_finish_v1();
    _exit(finished == 0 && g_child_clear == 0 && !g_child_bad_refs &&
                  g_child_seen_refs == 1
              ? 0
              : 2);
  }
  g_unregister_resume.store(true, std::memory_order_release);
  releaser.join();
  wait_ok(pid);
  need(release_done.load(std::memory_order_acquire), "release did not finish");
  need(lang_gc_registry_live_v1() == 0, "final release left a GC node");
  need(g_child_clear == 0, "child observed a zero-ref clear");
  need(g_child_bad_refs == 0, "child observed a zero-ref registered node");
  need(g_parent_clear.load() == 1, "parent final destroy did not run once");
  need(lang_gc_program_finish_v1() == 0, "parent finish failed");
  std::puts("passed: final release keeps a positive registered ref across fork");
}
} // namespace

int main(int argc, char **argv) {
  try {
    need(argc == 2, "expected one mode argument");
    const std::string mode = argv[1];
    if (mode == "same-thread") same_thread();
    else if (mode == "failed-fork") failed_fork();
    else if (mode == "race") first_bind_race();
    else if (mode == "gc") gc_window();
    else throw std::runtime_error("unknown mode");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "async fork runtime: %s\n", error.what());
    return 1;
  }
}
