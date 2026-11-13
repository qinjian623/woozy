// Hosted direct-libc-fork checks for the optional async hooks. Unlike the
// freestanding runtime this links glibc and the hosted atfork constructor.
//   same-direct    executor on main; libc fork; child uses reset async state.
//   foreign-source Source fork wrapper returns -EBUSY (no child).
//   foreign-direct direct libc fork records the refusal marker and Panics.
//   foreign-spawn  exec-only spawn still works with a foreign executor.
//   prepare-final-release-{direct,source,parent}: a user prepare/parent handler
//   joins a worker that drops the last Task reference. A direct libc positive
//   control is kept; Source parent also performs a nested direct fork after the
//   runtime parent hook released the binding reservation. The runner wraps each
//   mode in a process-group timeout.
#include "../../../runtime/native/async.hpp"
#include "../../../runtime/native/process.hpp"
#include "../../../runtime/native/alloc.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <pthread.h>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <cstddef>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
void need(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
void sleep_ms(unsigned milliseconds) {
  std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
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
LangText text(const char *value) {
  return {reinterpret_cast<const uint8_t *>(value), std::strlen(value),
          std::strlen(value), nullptr};
}
void wait_ok(pid_t pid, int expected = 0) {
  int status = 0;
  need(pid > 0 && waitpid(pid, &status, 0) == pid, "waitpid failed");
  need(WIFEXITED(status) && WEXITSTATUS(status) == expected,
       "child status differs");
}

struct Foreign {
  std::atomic<bool> ready{false}, stop{false};
  std::thread thread;
  void start() {
    thread = std::thread([this] {
      lang_async_run_v1();
      ready.store(true, std::memory_order_release);
      while (!stop.load(std::memory_order_acquire)) sleep_ms(1);
    });
    while (!ready.load(std::memory_order_acquire)) sleep_ms(1);
  }
  void finish() {
    stop.store(true, std::memory_order_release);
    thread.join();
  }
};

void same_direct() {
  lang_async_run_v1();
  const pid_t child = ::fork();
  if (child == 0) {
    alarm(10);
    lang_async_run_v1();
    lang_async_shutdown_v1();
    _exit(0);
  }
  wait_ok(child);
  lang_async_shutdown_v1();
  std::puts("passed: hosted same-thread direct libc fork");
}

void failed_source() {
  lang_async_run_v1();
  need(deny_fork_syscalls(), "seccomp filter failed");
  need(lang_process_fork_v1() == -EPERM, "hosted denied fork did not report EPERM");
  lang_async_run_v1();
  void *task = lang_async_task_new_v1();
  lang_async_task_release_v1(task);
  lang_async_shutdown_v1();
  need(lang_process_fork_v1() == -EPERM,
       "hosted failed fork left the async reservation or ticket lock held");
  std::puts("passed: hosted failed fork restores async state");
}

void foreign_source() {
  Foreign foreign;
  foreign.start();
  const auto status = lang_process_fork_v1();
  need(status == -EBUSY, "hosted Source fork did not reject a foreign executor");
  foreign.finish();
  std::puts("passed: hosted Source fork returns -EBUSY");
}

void foreign_direct() {
  Foreign foreign;
  foreign.start();
  const pid_t child = ::fork();
  if (child == 0) _exit(0);
  // The atfork prepare must terminate this process with the recorded marker;
  // this line is unreachable on a correct build.
  if (child > 0) {
    int status = 0;
    waitpid(child, &status, 0);
  }
  std::fprintf(stderr, "direct fork unexpectedly returned\n");
  _exit(70);
}

std::atomic<bool> g_release_go{false};
pthread_t g_release_worker;
void *g_release_task = nullptr;
bool g_release_parent_mode = false;

void release_marker(const char *text) { (void)write(2, text, std::strlen(text)); }
void *release_worker(void *) {
  while (!g_release_go.load(std::memory_order_acquire))
    std::this_thread::yield();
  release_marker("worker-enter-release\n");
  lang_async_task_release_v1(g_release_task);
  release_marker("worker-release-returned\n");
  return nullptr;
}
void release_wait_worker() {
  release_marker("user-handler-join\n");
  g_release_go.store(true, std::memory_order_release);
  need(pthread_join(g_release_worker, nullptr) == 0, "worker join failed");
  release_marker("user-handler-returned\n");
}
void release_prepare() {
  if (!g_release_parent_mode) release_wait_worker();
}
int g_release_parent_nested = 0;
void release_parent() {
  if (!g_release_parent_mode) return;
  if (g_release_parent_nested != 0) return;
  release_wait_worker();
  // The runtime parent hook released the Source binding reservation and cleared
  // the outer-reservation TLS before this user handler. A nested direct libc
  // fork must therefore take its own reservation instead of asserting on a
  // free gate with a stale outer flag.
  ++g_release_parent_nested;
  const pid_t child = ::fork();
  if (child == 0) _exit(0);
  if (child > 0) wait_ok(child);
  --g_release_parent_nested;
}

void final_release_handler(bool parent_mode, bool direct) {
  g_release_parent_mode = parent_mode;
  lang_async_run_v1();
  g_release_task = lang_async_task_new_v1();
  need(g_release_task != nullptr, "release task allocation failed");
  need(pthread_create(&g_release_worker, nullptr, release_worker, nullptr) == 0,
       "release worker creation failed");
  need(pthread_atfork(&release_prepare, &release_parent, nullptr) == 0,
       "release pthread_atfork failed");
  alarm(10);
  const auto pid = direct ? static_cast<int64_t>(::fork())
                         : lang_process_fork_v1();
  if (pid == 0) _exit(0);
  need(pid > 0, "release probe fork failed");
  wait_ok(static_cast<pid_t>(pid));
  lang_async_shutdown_v1();
  release_marker("fork-returned\n");
  std::puts(parent_mode ? "passed: user parent joined the final release"
                        : "passed: user prepare joined the final release");
}

int user_child_depth = 0;
void user_prepare() {}
void user_parent() {}
void user_child() {
  if (user_child_depth != 0) return;
  ++user_child_depth;
  alarm(10);
  const int64_t nested = lang_process_fork_v1();
  if (nested == 0) _exit(0);
  if (nested > 0) wait_ok(static_cast<pid_t>(nested));
  const pid_t direct = ::fork();
  if (direct == 0) _exit(0);
  if (direct > 0) wait_ok(direct);
  --user_child_depth;
}

void nested_user_child() {
  lang_async_run_v1();
  need(pthread_atfork(&user_prepare, &user_parent, &user_child) == 0,
       "pthread_atfork failed");
  const auto child = lang_process_fork_v1();
  if (child == 0) _exit(0);
  wait_ok(child);
  lang_async_run_v1();
  lang_async_shutdown_v1();
  std::puts("passed: hosted nested Source/direct forks in a user child handler");
}

void prepare_final_release_direct() { final_release_handler(false, true); }
void prepare_final_release_source() { final_release_handler(false, false); }
void prepare_final_release_parent() { final_release_handler(true, false); }

void foreign_spawn() {
  Foreign foreign;
  foreign.start();
  auto path = text("/bin/sh");
  LangText arguments[] = {text("sh"), text("-c"), text("exit 3")};
  LangVec *args = lang_vec_new_v1(0, sizeof(LangText), nullptr, nullptr);
  for (const auto &value : arguments) lang_vec_push_v1(args, &value);
  LangVec *env = lang_vec_new_v1(0, sizeof(LangText), nullptr, nullptr);
  const auto pid = lang_process_spawn_v1(&path, args, env, -1, -1, -1);
  need(pid > 0, "hosted exec-only spawn failed");
  wait_ok(static_cast<pid_t>(pid), 3);
  auto missing = text("/definitely/missing/woozy");
  need(lang_process_spawn_v1(&missing, args, env, -1, -1, -1) == -ENOENT,
       "hosted exec-only spawn lost exec errno");
  lang_vec_release_v1(args);
  lang_vec_release_v1(env);
  foreign.finish();
  std::puts("passed: hosted exec-only spawn with a foreign executor");
}
} // namespace

int main(int argc, char **argv) {
  try {
    need(argc == 2, "expected one mode");
    const std::string mode = argv[1];
    if (mode == "same-direct") same_direct();
    else if (mode == "prepare-final-release-direct") prepare_final_release_direct();
    else if (mode == "prepare-final-release-source") prepare_final_release_source();
    else if (mode == "prepare-final-release-parent") prepare_final_release_parent();
    else if (mode == "failed-source") failed_source();
    else if (mode == "nested-user-child") nested_user_child();
    else if (mode == "foreign-source") foreign_source();
    else if (mode == "foreign-direct") foreign_direct();
    else if (mode == "foreign-spawn") foreign_spawn();
    else throw std::runtime_error("unknown mode");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "hosted async fork: %s\n", error.what());
    return 1;
  }
}
