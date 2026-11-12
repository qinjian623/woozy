// Hosted process/fork regression harness.  It links a normal CRT program
// against the hosted archive and checks that fork goes through libc (atfork,
// malloc/TLS/clock recovery), not the native raw syscall path.
#include "../../../runtime/native/process.hpp"
#include "../../../runtime/native/thread.hpp"
#include "../../../runtime/native/alloc.hpp"
#include "../../../runtime/hosted/process.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <pthread.h>
#include <sched.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace {
enum class ForkRole { normal, spawning, direct_window, failed, held_direct };
thread_local ForkRole fork_role = ForkRole::normal;
std::atomic<bool> spawn_paused{false}, release_spawn{false};
std::atomic<bool> direct_entered{false}, direct_returned{false};
std::atomic<bool> failure_paused{false}, release_failure{false};
std::atomic<bool> direct_locks_held{false}, release_direct{false};

bool wait_flag(const std::atomic<bool> &flag, unsigned milliseconds) {
  const auto end = std::chrono::steady_clock::now() +
                   std::chrono::milliseconds(milliseconds);
  while (!flag.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < end)
    sched_yield();
  return flag.load(std::memory_order_acquire);
}
} // namespace

extern "C" pid_t __real_fork();
extern "C" pid_t __wrap_fork() {
  if (fork_role == ForkRole::spawning) {
    spawn_paused.store(true, std::memory_order_release);
    while (!release_spawn.load(std::memory_order_acquire)) sched_yield();
  }
  if (fork_role == ForkRole::direct_window)
    direct_entered.store(true, std::memory_order_release);
  const pid_t pid = __real_fork();
  if (pid > 0 && fork_role == ForkRole::direct_window)
    direct_returned.store(true, std::memory_order_release);
  if (pid < 0 && fork_role == ForkRole::failed) {
    // libc's parent handlers have run, but the language failure fallback has
    // not. Let another thread enter prepare before this wrapper returns.
    failure_paused.store(true, std::memory_order_release);
    while (!release_failure.load(std::memory_order_acquire)) sched_yield();
  }
  return pid;
}

extern "C" void __real_lang_hosted_process_atfork_prepare_v1() noexcept;
extern "C" void __wrap_lang_hosted_process_atfork_prepare_v1() noexcept {
  __real_lang_hosted_process_atfork_prepare_v1();
  if (fork_role == ForkRole::held_direct) {
    direct_locks_held.store(true, std::memory_order_release);
    while (!release_direct.load(std::memory_order_acquire)) sched_yield();
  }
}

namespace {
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

// User atfork callbacks are registered after the runtime constructors.  POSIX
// therefore runs this prepare first and this child callback after the hosted
// child handlers have reset the runtime locks.
std::atomic<int> g_prepare{0};
std::atomic<int> g_parent{0};
std::atomic<int> g_child{0};
int g_fork_signal = -1;
LangVec *g_environment_key = nullptr;

void user_prepare() { g_prepare.fetch_add(1, std::memory_order_release); }
void user_parent() { g_parent.fetch_add(1, std::memory_order_release); }
void user_child() {
  g_child.fetch_add(1, std::memory_order_release);
  char result = 'C';
  // Exercise the language environment and allocator from a user child
  // callback.  This would deadlock if the runtime child handler had not reset
  // its own locks before user child callbacks run.
  LangVec *value = nullptr;
  if (lang_process_getenv_bytes_v1(&value, g_environment_key) != 1 || !value)
    result = 'F';
  if (value) lang_vec_release_v1(value);
  void *allocation = lang_alloc_v1(64, 16);
  if (!allocation)
    result = 'F';
  else
    lang_free_v1(allocation);
  if (g_fork_signal >= 0) {
    const ssize_t written = write(g_fork_signal, &result, 1);
    (void)written;
  }
}

LangVec *bytes(const char *text) {
  const uint64_t length = std::strlen(text);
  LangVec *value = lang_vec_new_v1(length, 1, nullptr, nullptr);
  need(value != nullptr, "lang_vec_new_v1 failed");
  std::memcpy(value->data, text, length);
  value->initialized = length;
  return value;
}

void retain_row(void *pointer) {
  lang_vec_retain_v1(*static_cast<LangVec **>(pointer));
}
void drop_row(void *pointer) {
  lang_vec_release_v1(*static_cast<LangVec **>(pointer));
}

LangVec *list(const char *const *items, size_t count) {
  LangVec *value =
      lang_vec_new_v1(0, sizeof(LangVec *), retain_row, drop_row);
  need(value != nullptr, "list allocation failed");
  for (size_t i = 0; i < count; ++i) {
    LangVec *row = bytes(items[i]);
    lang_vec_push_v1(value, &row);
  }
  return value;
}

int64_t thread_answer(void *) noexcept { return 42; }

struct Background {
  std::atomic<uint32_t> started{0};
  std::atomic<uint32_t> stop{0};
  std::atomic<uint32_t> errors{0};
};

int64_t background_entry(void *pointer) noexcept {
  auto *background = *static_cast<Background **>(pointer);
  background->started.store(1, std::memory_order_release);
  while (!background->stop.load(std::memory_order_acquire)) {
    LangVec *value = nullptr;
    const int64_t status =
        lang_process_getenv_bytes_v1(&value, g_environment_key);
    if (status == 1 && value)
      lang_vec_release_v1(value);
    else
      background->errors.fetch_add(1, std::memory_order_relaxed);
    void *allocation = lang_alloc_v1(128, 16);
    if (!allocation)
      background->errors.fetch_add(1, std::memory_order_relaxed);
    else
      lang_free_v1(allocation);
    void *host = std::malloc(256);
    if (!host)
      background->errors.fetch_add(1, std::memory_order_relaxed);
    else
      std::free(host);
  }
  return 7;
}

int user_child_lock_check() {
  if (g_prepare.load(std::memory_order_acquire) != 1 ||
      g_child.load(std::memory_order_acquire) != 1)
    return 40;
  errno = 777;
  void *allocation = std::malloc(4096);
  if (!allocation) return 41;
  std::memset(allocation, 0x5a, 4096);
  std::free(allocation);
  if (errno != 777) return 42;

  clockid_t clock = 0;
  if (pthread_getcpuclockid(pthread_self(), &clock) != 0) return 43;
  timespec now{};
  errno = 0;
  if (clock_gettime(clock, &now) != 0) return 44;

  LangVec *value = nullptr;
  if (lang_process_getenv_bytes_v1(&value, g_environment_key) != 1 || !value)
    return 45;
  const bool environment_ok =
      value->length == 17 &&
      std::memcmp(value->data, "hosted-fork-value", 17) == 0;
  lang_vec_release_v1(value);
  if (!environment_ok) return 46;

  void *language_allocation = lang_alloc_v1(2048, 16);
  if (!language_allocation) return 47;
  lang_free_v1(language_allocation);

  return 0;
}

int child_checks(LangHandle *inherited) {
  alarm(10);
  if (const int lock_status = user_child_lock_check()) return lock_status;
  // Verify the child's launch lock was reset: a nested language fork must not
  // deadlock.  Disable the user-child pipe write in this child copy so the
  // parent still sees exactly the outer callback byte.
  g_fork_signal = -1;
  const int64_t grandchild = lang_process_fork_v1();
  if (grandchild < 0) return 51;
  if (grandchild == 0) _exit(0);
  int grandchild_status = 0;
  if (lang_process_waitpid_v1(static_cast<int32_t>(grandchild), 0,
                              &grandchild_status) != grandchild)
    return 52;
  if (!WIFEXITED(grandchild_status) || WEXITSTATUS(grandchild_status) != 0)
    return 53;
  // A parent-created runtime handle must be recognised as inherited and return
  // ESRCH before touching a mutex that a vanished thread may have held.
  if (inherited && lang_thread_wait_v1(inherited) != -3) return 48;
  int argument = 0;
  LangHandle *thread =
      lang_thread_spawn_v1(thread_answer, &argument, sizeof(argument), nullptr,
                           nullptr);
  if (lang_thread_error_v1(thread) != 0) return 49;
  const int64_t status = lang_thread_wait_v1(thread);
  const int64_t answer = status ? status : lang_thread_result_v1(thread);
  lang_handle_release_v1(thread);
  if (status != 0 || answer != 42) return 50;
  return 0;
}

int64_t read_fork_signal(int descriptor) {
  char bytes_seen[8]{};
  const ssize_t count = read(descriptor, bytes_seen, sizeof(bytes_seen));
  close(descriptor);
  return count;
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
  sock_fprog program{static_cast<unsigned short>(sizeof(filter) / sizeof(filter[0])),
                     filter};
  return prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 &&
         prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0;
}

void check_parent_state() {
  LangVec *value = nullptr;
  need(lang_process_getenv_bytes_v1(&value, g_environment_key) == 1 && value,
       "parent language environment lock is unusable");
  need(value->length == 17 &&
           std::memcmp(value->data, "hosted-fork-value", 17) == 0,
       "parent language environment changed");
  lang_vec_release_v1(value);
  void *allocation = lang_alloc_v1(4096, 16);
  need(allocation != nullptr, "parent allocator lock is unusable");
  lang_free_v1(allocation);
  errno = 321;
  need(errno == 321, "parent errno TLS is unusable");
}

void check_direct_fork_spawn_window() {
  int release_pipe[2];
  need(pipe2(release_pipe, O_CLOEXEC) == 0, "window release pipe failed");
  LangVec *path = bytes("/bin/true");
  const char *argv[] = {"/bin/true"};
  LangVec *args = list(argv, 1);
  LangVec *env = lang_vec_new_v1(0, sizeof(LangVec *), retain_row, drop_row);
  int64_t spawned = -1;
  std::atomic<bool> spawn_done{false}, before_child_release{false};
  std::thread launcher([&] {
    fork_role = ForkRole::spawning;
    spawned = lang_process_spawn_bytes_v1(path, args, env, -1, -1, -1);
    spawn_done.store(true, std::memory_order_release);
  });
  until([&] { return spawn_paused.load(std::memory_order_acquire); },
        "spawn did not reach the temporary-pipe window");
  std::thread controller([&] {
    wait_flag(direct_entered, 2000);
    // Without the launch lock the direct child is created here and inherits
    // spawn's writer. With the fix prepare waits until spawn closes it.
    wait_flag(direct_returned, 100);
    release_spawn.store(true, std::memory_order_release);
    before_child_release.store(wait_flag(spawn_done, 2000),
                               std::memory_order_release);
    const char release = 'R';
    const ssize_t written = write(release_pipe[1], &release, 1);
    (void)written;
  });
  fork_role = ForkRole::direct_window;
  const pid_t child = ::fork();
  if (!child) {
    alarm(5);
    close(release_pipe[1]);
    char release;
    _exit(read(release_pipe[0], &release, 1) == 1 ? 0 : 91);
  }
  fork_role = ForkRole::normal;
  controller.join();
  launcher.join();
  close(release_pipe[0]);
  close(release_pipe[1]);
  int status = 0;
  const bool child_ok = child > 0 && waitpid(child, &status, 0) == child &&
                        WIFEXITED(status) && WEXITSTATUS(status) == 0;
  const bool spawn_ok = spawned > 0 && waitpid(spawned, &status, 0) == spawned &&
                        WIFEXITED(status) && WEXITSTATUS(status) == 0;
  lang_vec_release_v1(path);
  lang_vec_release_v1(args);
  lang_vec_release_v1(env);
  need(child_ok && spawn_ok, "window child or spawned program failed");
  need(before_child_release.load(std::memory_order_acquire),
       "direct libc fork inherited spawn's exec-error pipe writer");
  check_parent_state();
  std::puts("passed: direct libc fork cannot inherit a concurrent spawn error writer");
}

void check_failed_fork_ownership() {
  int64_t failed_status = 0;
  std::atomic<bool> failed_done{false}, observer_started{false}, observer_done{false};
  std::thread failing([&] {
    if (!deny_fork_syscalls()) {
      failed_status = -999;
      failure_paused.store(true, std::memory_order_release);
    } else {
      fork_role = ForkRole::failed;
      failed_status = lang_process_fork_v1();
    }
    failed_done.store(true, std::memory_order_release);
  });
  until([&] { return failure_paused.load(std::memory_order_acquire); },
        "failed fork did not reach the parent-handler/fallback window");
  pid_t direct_child = -1;
  std::thread direct([&] {
    fork_role = ForkRole::held_direct;
    direct_child = ::fork();
    if (!direct_child) _exit(0);
  });
  // A correct direct prepare is still waiting for failing's LaunchGuard.
  // Always release failing, rather than requiring direct prepare to finish.
  wait_flag(direct_locks_held, 100);
  release_failure.store(true, std::memory_order_release);
  until([&] { return failed_done.load(std::memory_order_acquire); },
        "failed fork did not release its launch guard");
  until([&] { return direct_locks_held.load(std::memory_order_acquire); },
        "direct fork did not acquire its prepare locks");
  std::atomic<int> observer_error{0};
  std::thread observer([&] {
    observer_started.store(true, std::memory_order_release);
    LangVec *value = nullptr;
    if (lang_process_getenv_bytes_v1(&value, g_environment_key) != 1 || !value)
      observer_error.store(1);
    if (value) lang_vec_release_v1(value);
    void *allocation = lang_alloc_v1(128, 16);
    if (!allocation) observer_error.store(2);
    else lang_free_v1(allocation);
    observer_done.store(true, std::memory_order_release);
  });
  until([&] { return observer_started.load(std::memory_order_acquire); },
        "lock observer did not start");
  const bool released_others_locks = wait_flag(observer_done, 100);
  release_direct.store(true, std::memory_order_release);
  failing.join();
  direct.join();
  observer.join();
  int status = 0;
  need(direct_child > 0 && waitpid(direct_child, &status, 0) == direct_child &&
           WIFEXITED(status) && WEXITSTATUS(status) == 0,
       "direct child after failed fork did not exit");
  need(failed_status == -EPERM, "seccomp fork did not report EPERM");
  need(!released_others_locks,
       "failed-fork fallback released another thread's prepare locks");
  need(observer_error.load() == 0, "environment/allocator unusable after overlap");
  check_parent_state();
  std::puts("passed: failed-fork fallback cannot release another thread's locks");
}

void check_repeated_concurrent_forks() {
  std::atomic<int> errors{0};
  std::thread failing([&] {
    if (!deny_fork_syscalls()) { errors.fetch_add(1); return; }
    for (int i = 0; i < 32; ++i) {
      if (lang_process_fork_v1() != -EPERM) errors.fetch_add(1);
      LangVec *value = nullptr;
      if (lang_process_getenv_bytes_v1(&value, g_environment_key) != 1 || !value)
        errors.fetch_add(1);
      if (value) lang_vec_release_v1(value);
    }
  });
  std::thread direct([&] {
    for (int i = 0; i < 32; ++i) {
      const pid_t child = ::fork();
      if (!child) {
        clockid_t clock;
        timespec now{};
        const bool ok = pthread_getcpuclockid(pthread_self(), &clock) == 0 &&
                        clock_gettime(clock, &now) == 0;
        _exit(ok ? 0 : 92);
      }
      int status = 0;
      if (child < 0 || waitpid(child, &status, 0) != child ||
          !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        errors.fetch_add(1);
      void *allocation = lang_alloc_v1(256, 16);
      if (!allocation) errors.fetch_add(1);
      else lang_free_v1(allocation);
    }
  });
  failing.join();
  direct.join();
  need(errors.load() == 0, "repeated concurrent direct/failed forks failed");
  check_parent_state();
  std::puts("passed: 32 direct forks concurrent with 32 seccomp-denied language forks");
}
} // namespace

int main() {
  try {
    need(std::getenv("HOSTED_FORK_ENV") != nullptr,
         "HOSTED_FORK_ENV was not provided by the harness");
    g_environment_key = bytes("HOSTED_FORK_ENV");
    need(pthread_atfork(&user_prepare, &user_parent, &user_child) == 0,
         "user pthread_atfork registration failed");

    Background background;
    Background *background_pointer = &background;
    LangHandle *background_thread = lang_thread_spawn_v1(
        background_entry, &background_pointer, sizeof(background_pointer),
        nullptr, nullptr);
    need(lang_thread_error_v1(background_thread) == 0,
         "background thread creation failed");
    until([&] { return background.started.load(std::memory_order_acquire) == 1; },
          "background thread did not start");
    for (int i = 0; i < 100; ++i) sched_yield();

    int pipe_descriptors[2];
    need(pipe2(pipe_descriptors, O_CLOEXEC) == 0, "pipe2 failed");
    g_fork_signal = pipe_descriptors[1];
    const int64_t child = lang_process_fork_v1();
    if (child == 0) _exit(child_checks(background_thread));
    need(child > 0, "language fork failed");
    close(pipe_descriptors[1]);
    g_fork_signal = -1;
    int child_status = 0;
    need(lang_process_waitpid_v1(static_cast<int32_t>(child), 0,
                                 &child_status) == child,
         "waitpid for language fork child failed");
    need(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0,
         "fork child failed library/TLS/clock/lock checks (status " +
             std::to_string(WIFEXITED(child_status) ? WEXITSTATUS(child_status) : -1) +
             ")");
    need(read_fork_signal(pipe_descriptors[0]) == 1,
         "user atfork child callback did not run through libc fork");
    need(g_prepare.load(std::memory_order_acquire) == 1 &&
             g_parent.load(std::memory_order_acquire) == 1,
         "user atfork prepare/parent callbacks did not run");
    check_parent_state();

    // The parent must still work after the child and the background thread must
    // remain unaffected by the child-only fork.
    background.stop.store(1, std::memory_order_release);
    need(lang_thread_wait_v1(background_thread) == 0,
         "background thread wait failed");
    need(lang_thread_result_v1(background_thread) == 7,
         "background thread result changed");
    lang_handle_release_v1(background_thread);
    need(background.errors.load(std::memory_order_acquire) == 0,
         "background thread encountered runtime errors around fork");

    // Exec path: a libc fork child calls the language execve helper.
    LangVec *path = bytes("/bin/true");
    const char *arguments[] = {"/bin/true"};
    LangVec *exec_arguments = list(arguments, 1);
    LangVec *exec_environment =
        lang_vec_new_v1(0, sizeof(LangVec *), retain_row, drop_row);
    need(exec_environment != nullptr, "empty environment allocation failed");
    need(pipe2(pipe_descriptors, O_CLOEXEC) == 0, "exec pipe2 failed");
    g_fork_signal = pipe_descriptors[1];
    const pid_t exec_child = ::fork();
    need(exec_child >= 0, "libc fork for exec failed");
    if (!exec_child) {
      const int64_t status =
          lang_process_execve_bytes_v1(path, exec_arguments, exec_environment);
      _exit(status < 0 ? 70 : 71);
    }
    close(pipe_descriptors[1]);
    g_fork_signal = -1;
    int exec_status = 0;
    need(lang_process_waitpid_v1(static_cast<int32_t>(exec_child), 0,
                                 &exec_status) == exec_child,
         "waitpid for exec child failed");
    need(WIFEXITED(exec_status) && WEXITSTATUS(exec_status) == 0,
         "language execve child failed");
    need(read_fork_signal(pipe_descriptors[0]) == 1,
         "user atfork child callback did not run for exec fork");

    // Spawn path: lang_process_spawn uses the same hosted fork selection and
    // the user child callback must fire before exec.
    need(pipe2(pipe_descriptors, O_CLOEXEC) == 0, "spawn pipe2 failed");
    g_fork_signal = pipe_descriptors[1];
    const int64_t spawned = lang_process_spawn_bytes_v1(
        path, exec_arguments, exec_environment, -1, -1, -1);
    need(spawned > 0, "language spawn failed");
    close(pipe_descriptors[1]);
    g_fork_signal = -1;
    int spawn_status = 0;
    need(lang_process_waitpid_v1(static_cast<int32_t>(spawned), 0,
                                 &spawn_status) == spawned,
         "waitpid for spawn child failed");
    need(WIFEXITED(spawn_status) && WEXITSTATUS(spawn_status) == 0,
         "spawned /bin/true failed");
    need(read_fork_signal(pipe_descriptors[0]) == 1,
         "user atfork child callback did not run for spawn fork");
    lang_vec_release_v1(path);
    lang_vec_release_v1(exec_arguments);
    lang_vec_release_v1(exec_environment);

    // Fork failure: deny the kernel clone/fork calls, then ensure two attempts
    // return errors and leave environment, allocator and launch locks usable.
    const pid_t failure_child = ::fork();
    need(failure_child >= 0, "libc fork for failure test failed");
    if (!failure_child) {
      alarm(5);
      if (!deny_fork_syscalls()) _exit(80);
      const int64_t first = lang_process_fork_v1();
      if (first >= 0) _exit(81);
      const int64_t second = lang_process_fork_v1();
      if (second >= 0) _exit(82);
      LangVec *value = nullptr;
      if (lang_process_getenv_bytes_v1(&value, g_environment_key) != 1 || !value)
        _exit(83);
      lang_vec_release_v1(value);
      void *allocation = lang_alloc_v1(1024, 16);
      if (!allocation) _exit(84);
      lang_free_v1(allocation);
      _exit(0);
    }
    int failure_status = 0;
    need(lang_process_waitpid_v1(static_cast<int32_t>(failure_child), 0,
                                 &failure_status) == failure_child,
         "waitpid for fork-failure child failed");
    need(WIFEXITED(failure_status) && WEXITSTATUS(failure_status) == 0,
         "fork failure did not release runtime locks");

    check_direct_fork_spawn_window();
    check_failed_fork_ownership();
    check_repeated_concurrent_forks();
    lang_vec_release_v1(g_environment_key);
    std::printf("passed: libc fork/atfork ordering, child malloc/TLS/pthread "
                "clock, language env/alloc/thread, exec, spawn and fork-failure "
                "lock recovery\n");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "hosted_process_test: %s\n", error.what());
    return 1;
  }
}
