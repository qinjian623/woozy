#include "../native/process.hpp"
#include "../native/alloc.hpp"
#include "../native/async.hpp"
#include "../native/gc_program.hpp"
#include "../native/thread.hpp"
#include "../native/weak.hpp"
#include "posix.hpp"

#include <crt_externs.h>
#include <fcntl.h>
#include <initializer_list>
#include <sys/wait.h>
#include <unistd.h>

extern "C" [[noreturn]] void lang_native_panic_v1(uint32_t, uint64_t, uint64_t, uint64_t) noexcept;
extern "C" void lang_signal_fork_child_v1() noexcept;

#include "../native/process_values.inc"

namespace {
namespace darwin = lang::native::darwin;
namespace gc = lang::native::gc_program;

uint32_t environment_lock;

void lock() noexcept {
  while (__atomic_exchange_n(&environment_lock, 1u, __ATOMIC_ACQUIRE)) {
    while (__atomic_load_n(&environment_lock, __ATOMIC_RELAXED)) {
      __asm__ volatile("yield");
    }
  }
}

void unlock() noexcept {
  __atomic_store_n(&environment_lock, 0u, __ATOMIC_RELEASE);
}

class ForkScope {
public:
  ForkScope() noexcept {
    if (gc::hooks_v1.fork_enter) gc::hooks_v1.fork_enter(&token_);
  }
  ~ForkScope() {
    if (gc::hooks_v1.fork_leave) gc::hooks_v1.fork_leave(&token_);
  }
  ForkScope(const ForkScope &) = delete;
  ForkScope &operator=(const ForkScope &) = delete;

private:
  LangGcProgramForkToken token_{};
};

class AsyncForkReservation {
 public:
  AsyncForkReservation() noexcept : status(lang_async_fork_reserve_v1(0)) {}
  ~AsyncForkReservation() noexcept {
    if (status == 0) lang_async_fork_release_v1();
  }
  AsyncForkReservation(const AsyncForkReservation &) = delete;
  AsyncForkReservation &operator=(const AsyncForkReservation &) = delete;

  const int64_t status;
};

int64_t execute(Bytes path, const LangVec *args, const LangVec *env, bool raw) noexcept {
  Exec command;
  const auto status = command.prepare(path, args, env, raw);
  if (status < 0) return status;
  darwin::LaunchGuard launch;
  return darwin::result(execve(command.path, command.arguments, command.environment));
}

struct LaunchFds {
  int copies[3] = {-1, -1, -1};
  int channel[2] = {-1, -1};

  ~LaunchFds() {
    for (const int fd : copies) if (fd >= 0) close(fd);
    for (const int fd : channel) if (fd >= 0) close(fd);
  }
};

void reap(pid_t pid) noexcept {
  while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
}

[[noreturn]] void run_child(const Exec &command, const LaunchFds &fds) noexcept {
  close(fds.channel[0]);
  int failure = 0;
  for (int dest = 0; dest < 3; ++dest) {
    if (fds.copies[dest] >= 0 && dup2(fds.copies[dest], dest) < 0) {
      failure = errno;
      break;
    }
  }
  for (const int fd : fds.copies) if (fd >= 0) close(fd);
  if (failure == 0) {
    execve(command.path, command.arguments, command.environment);
    failure = errno;
  }
  const auto *bytes = reinterpret_cast<const uint8_t *>(&failure);
  size_t written = 0;
  while (written < sizeof(failure)) {
    const auto count = write(fds.channel[1], bytes + written, sizeof(failure) - written);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    written += static_cast<size_t>(count);
  }
  _exit(127);
}

int64_t spawn(Bytes path, const LangVec *args, const LangVec *env, bool raw,
              int32_t input, int32_t output, int32_t error) noexcept {
  Exec command;
  const auto prepared = command.prepare(path, args, env, raw);
  if (prepared < 0) return prepared;
  ForkScope scope;
  darwin::LaunchGuard launch;
  const int sources[] = {input, output, error};
  for (const int fd : sources) {
    if (fd < -1) return -9;
    if (fd >= 0 && fcntl(fd, F_GETFD) < 0) return darwin::error(errno);
  }

  LaunchFds fds;
  // Snapshot every source before child file actions can overwrite a source.
  for (int dest = 0; dest < 3; ++dest) {
    if (sources[dest] < 0) continue;
    fds.copies[dest] = fcntl(sources[dest], F_DUPFD_CLOEXEC, 3);
    if (fds.copies[dest] < 0) return darwin::error(errno);
  }
  if (pipe(fds.channel) < 0) return darwin::error(errno);
  for (int &fd : fds.channel) {
    if (fd < 3) {
      const int duplicate = fcntl(fd, F_DUPFD_CLOEXEC, 3);
      if (duplicate < 0) return darwin::error(errno);
      close(fd);
      fd = duplicate;
    } else if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
      return darwin::error(errno);
    }
  }

  // Darwin posix_spawn can report an exec error while a transient child is
  // still visible to waitpid. The handshake lets us reap the exact child.
  const auto pid = fork();
  if (pid < 0) return darwin::error(errno);
  if (pid == 0) run_child(command, fds);
  close(fds.channel[1]);
  fds.channel[1] = -1;
  int failure = 0;
  auto *bytes = reinterpret_cast<uint8_t *>(&failure);
  size_t used = 0;
  while (used < sizeof(failure)) {
    const auto count = read(fds.channel[0], bytes + used, sizeof(failure) - used);
    if (count < 0) {
      if (errno == EINTR) continue;
      const auto status = darwin::error(errno);
      kill(pid, SIGKILL);
      reap(pid);
      return status;
    }
    if (count == 0) break;
    used += static_cast<size_t>(count);
  }
  if (used == 0) return pid;
  reap(pid);
  return used == sizeof(failure) ? darwin::error(failure) : -5;
}

int woozy_signal(int signal) noexcept {
  const auto &signals = darwin::linux_signal_to_darwin;
  for (unsigned n = 1; n < sizeof(signals) / sizeof(signals[0]); ++n) {
    if (signals[n] == signal) return static_cast<int>(n);
  }
  // Preserve the Darwin number outside the supported Linux signal range.
  return 64 + signal;
}

int32_t encode_wait_status(int status) noexcept {
  if (WIFCONTINUED(status)) return 0xffff;
  if (WIFEXITED(status)) return WEXITSTATUS(status) << 8;
  if (WIFSTOPPED(status)) return (woozy_signal(WSTOPSIG(status)) << 8) | 0x7f;
  if (WIFSIGNALED(status)) return woozy_signal(WTERMSIG(status)) | (WCOREDUMP(status) ? 0x80 : 0);
  return status;
}
} // namespace

extern "C" __attribute__((constructor, used)) void lang_darwin_startup_v1() noexcept {
  lang_process_init_v1(static_cast<uint64_t>(*_NSGetArgc()), *_NSGetArgv(), *_NSGetEnviron());
}

extern "C" int64_t lang_process_fork_v1() noexcept {
  ForkScope scope;
  AsyncForkReservation reservation;
  if (reservation.status < 0) return reservation.status;
  darwin::LaunchGuard launch;
  lock();
  lang_alloc_fork_prepare_v1();
  lang_weak_fork_prepare_v1();
  const auto &hooks = gc::hooks_v1;
  const bool collect = hooks.fork_ready && hooks.fork_ready() != 0;
  if (collect) {
    if (hooks.gc_fork_prepare) hooks.gc_fork_prepare();
    if (hooks.world_fork_prepare) hooks.world_fork_prepare();
    if (hooks.runtime_fork_prepare) hooks.runtime_fork_prepare();
  }
  lang_async_fork_prepare_v1(0);

  const auto pid = darwin::result(fork());
  if (pid == 0) {
    lang_alloc_fork_child_v1();
    lang_weak_fork_child_v1();
    if (collect) {
      if (hooks.gc_fork_child) hooks.gc_fork_child();
      if (hooks.world_fork_child) hooks.world_fork_child();
      if (hooks.runtime_fork_child) hooks.runtime_fork_child();
    }
    lang_async_fork_child_v1(0);
    lang_signal_fork_child_v1();
  } else {
    lang_async_fork_parent_v1(0);
    if (collect) {
      if (hooks.runtime_fork_parent) hooks.runtime_fork_parent();
      if (hooks.world_fork_parent) hooks.world_fork_parent();
      if (hooks.gc_fork_parent) hooks.gc_fork_parent();
    }
    lang_weak_fork_parent_v1();
    lang_alloc_fork_parent_v1();
  }
  unlock();
  launch.release();
  if (pid == 0) lang_thread_fork_child_v1();
  return pid;
}

extern "C" int64_t lang_process_waitpid_v1(int32_t pid, int32_t options,
                                            int32_t *status) noexcept {
  if (!status) return -14;
  if (options & ~11) return -22;
  int flags = 0;
  if (options & 1) flags |= WNOHANG;
  if (options & 2) flags |= WUNTRACED;
  if (options & 8) flags |= WCONTINUED;
  int value = 0;
  int64_t child;
  {
    gc::SafeGuard safe;
    child = darwin::result(waitpid(pid, &value, flags));
  }
  if (child >= 0) *status = child != 0 ? encode_wait_status(value) : 0;
  return child;
}

extern "C" int64_t lang_process_execve_v1(const LangText *path, const LangVec *args,
                                           const LangVec *env) noexcept {
  Bytes file;
  const auto status = text_view(path, file);
  return status < 0 ? status : execute(file, args, env, false);
}

extern "C" int64_t lang_process_execve_bytes_v1(const LangVec *path, const LangVec *args,
                                                 const LangVec *env) noexcept {
  Bytes file;
  const auto status = byte_view(path, file);
  return status < 0 ? status : execute(file, args, env, true);
}

extern "C" int64_t lang_process_spawn_v1(const LangText *path, const LangVec *args,
                                          const LangVec *env, int32_t input,
                                          int32_t output, int32_t error) noexcept {
  Bytes file;
  const auto status = text_view(path, file);
  return status < 0 ? status : spawn(file, args, env, false, input, output, error);
}

extern "C" int64_t lang_process_spawn_bytes_v1(const LangVec *path, const LangVec *args,
                                                const LangVec *env, int32_t input,
                                                int32_t output, int32_t error) noexcept {
  Bytes file;
  const auto status = byte_view(path, file);
  return status < 0 ? status : spawn(file, args, env, true, input, output, error);
}
