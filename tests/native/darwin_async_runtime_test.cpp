#include "../../runtime/native/async.hpp"
#include "../../runtime/native/gc_program.hpp"
#include "../../runtime/native/network.hpp"
#include "../../runtime/native/process.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

constexpr uint32_t pending = 0;
constexpr uint32_t done = 1;
constexpr uint32_t cancelled = 3;
constexpr int32_t readable = 1;
constexpr int32_t writable = 4;

struct Frame {
  void *target;
  uint32_t promise = pending;
  unsigned resumes = 0;
  unsigned destroyed = 0;
};

void resume(void *raw) noexcept {
  auto &frame = *static_cast<Frame *>(raw);
  ++frame.resumes;
  frame.promise = lang_async_current_cancelled_v1()
      ? cancelled
      : lang_async_await_v1(frame.target);
}

void destroy(void *raw) noexcept { ++static_cast<Frame *>(raw)->destroyed; }

void *start(Frame &frame) {
  void *task = lang_async_task_new_v1();
  lang_async_publish_v2(task, &frame, resume, destroy, &frame.promise, nullptr, nullptr);
  lang_async_spawn_v1(task);
  return task;
}

void suspend(void *task) {
  void *timer = lang_async_sleep_v1(0);
  lang_async_run_until_v1(timer);
  assert(lang_async_status_v1(timer) == done);
  assert(lang_async_status_v1(task) == pending);
  lang_async_task_release_v1(timer);
}

void release_delayed(void *future) {
  const int32_t registered = lang_gc_program_thread_enter_v1();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  lang_async_task_release_v1(future);
  lang_gc_program_thread_leave_v1(registered);
}

void test_fd_and_timer(int receiver, int sender) {
  Frame input{lang_async_wait_fd_v1(receiver, readable)};
  void *task = start(input);
  suspend(task);
  assert(input.resumes == 1);
  std::thread writer([&] {
    const int32_t registered = lang_gc_program_thread_enter_v1();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    void *timer = lang_async_sleep_v1(10);
    while (lang_async_status_v1(timer) == pending) std::this_thread::yield();
    assert(lang_async_status_v1(timer) == done);
    assert(write(sender, "a", 1) == 1);
    lang_async_task_release_v1(timer);
    lang_gc_program_thread_leave_v1(registered);
  });
  lang_async_run_until_v1(task);
  writer.join();
  assert(input.promise == done && input.resumes == 2);
  lang_async_task_release_v1(task);
  lang_async_task_release_v1(input.target);
  assert(input.destroyed == 1);

  // Both filters on one fd can appear in a single event batch.
  Frame both{lang_async_wait_fd_v1(receiver, readable | writable)};
  task = start(both);
  lang_async_run_until_v1(task);
  assert(both.promise == done && both.resumes == 2);
  lang_async_task_release_v1(task);
  lang_async_task_release_v1(both.target);
  char byte;
  assert(read(receiver, &byte, 1) == 1 && byte == 'a');

  void *long_timer = lang_async_sleep_v1(INT64_MAX);
  void *short_timer = lang_async_sleep_v1(10);
  lang_async_run_until_v1(short_timer);
  assert(lang_async_status_v1(short_timer) == done);
  assert(lang_async_status_v1(long_timer) == pending);
  lang_async_task_release_v1(short_timer);
  void *unsubmitted = lang_async_task_new_v1();
  std::thread timer_releaser(release_delayed, long_timer);
  lang_async_run_until_v1(unsubmitted);
  timer_releaser.join();
  assert(lang_async_status_v1(unsubmitted) == pending);
  lang_async_task_release_v1(unsubmitted);
}

void test_cancel_and_release(int receiver) {
  Frame input{lang_async_wait_fd_v1(receiver, readable)};
  void *task = start(input);
  suspend(task);
  lang_async_cancel_v1(task);
  lang_async_run_until_v1(task);
  assert(input.promise == cancelled && input.resumes == 2);
  lang_async_task_release_v1(task);
  assert(input.destroyed == 1);
  void *unsubmitted = lang_async_task_new_v1();
  std::thread watch_releaser(release_delayed, input.target);
  lang_async_run_until_v1(unsubmitted);
  watch_releaser.join();
  assert(lang_async_status_v1(unsubmitted) == pending);
  lang_async_task_release_v1(unsubmitted);
}

int listen_loopback() {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  sockaddr_in address{};
  address.sin_len = sizeof(address);
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0);
  assert(listen(fd, 1) == 0);
  return fd;
}

void test_listener_shutdown() {
  const int fd = listen_loopback();
  constexpr int linux_enotconn = -107;
  assert(lang_net_shutdown_v1(fd, SHUT_RDWR) == linux_enotconn);
  Frame listening{lang_async_wait_fd_v1(fd, readable)};
  void *task = start(listening);
  suspend(task); // No watch existed at the earlier shutdown.
  std::thread stopper([&] {
    const int32_t registered = lang_gc_program_thread_enter_v1();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(lang_net_shutdown_v1(fd, SHUT_RDWR) == linux_enotconn);
    lang_gc_program_thread_leave_v1(registered);
  });
  lang_async_run_until_v1(task);
  stopper.join();
  assert(listening.promise == done && listening.resumes == 2);
  lang_async_task_release_v1(task);
  lang_async_task_release_v1(listening.target);

  Frame stale{lang_async_wait_fd_v1(fd, readable)};
  task = start(stale);
  suspend(task);
  assert(lang_net_shutdown_v1(fd, SHUT_RDWR) == linux_enotconn);
  const int replacement = listen_loopback();
  assert(close(fd) == 0);
  assert(dup2(replacement, fd) == fd);
  void *timer = lang_async_sleep_v1(10);
  lang_async_run_until_v1(timer);
  lang_async_task_release_v1(timer);
  assert(stale.promise == pending && stale.resumes == 1);
  lang_async_cancel_v1(task);
  lang_async_run_until_v1(task);
  lang_async_task_release_v1(task);
  lang_async_task_release_v1(stale.target);

  Frame current{lang_async_wait_fd_v1(fd, readable)};
  task = start(current);
  suspend(task);
  assert(lang_net_shutdown_v1(fd, SHUT_RDWR) == linux_enotconn);
  lang_async_run_until_v1(task);
  assert(current.promise == done && current.resumes == 2);
  lang_async_task_release_v1(task);
  lang_async_task_release_v1(current.target);
  assert(close(fd) == 0);
  assert(close(replacement) == 0);
}

void test_fork(int receiver, int sender) {
  std::thread foreign([] {
    const int32_t registered = lang_gc_program_thread_enter_v1();
    const auto pid = lang_process_fork_v1();
    if (pid == 0) _exit(91);
    assert(pid == -16);
    lang_gc_program_thread_leave_v1(registered);
  });
  foreign.join();

  Frame frame{lang_async_wait_fd_v1(receiver, readable)};
  void *task = start(frame);
  suspend(task);
  const auto child = lang_process_fork_v1();
  assert(child >= 0);
  if (child == 0) {
    alarm(5);
    assert(write(sender, "b", 1) == 1);
    lang_async_run_until_v1(task);
    assert(frame.promise == done && frame.resumes == 2);
    lang_async_shutdown_v1();
    _exit(0);
  }
  int32_t status = -1;
  assert(lang_process_waitpid_v1(static_cast<int32_t>(child), 0, &status) == child);
  assert(status == 0);
  lang_async_run_until_v1(task);
  assert(frame.promise == done && frame.resumes == 2);
  lang_async_task_release_v1(task);
  lang_async_task_release_v1(frame.target);
}

} // namespace

int main() {
  alarm(15);
  assert(lang_gc_program_init_v1() == LANG_GC_PROGRAM_OK);
  int sockets[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  test_fd_and_timer(sockets[0], sockets[1]);
  test_cancel_and_release(sockets[0]);
  test_listener_shutdown();
  test_fork(sockets[0], sockets[1]);
  lang_async_shutdown_v1();
  assert(lang_gc_program_finish_v1() == LANG_GC_PROGRAM_OK);
  close(sockets[0]);
  close(sockets[1]);
  std::puts("darwin async readiness, timer, cancellation, release, fork, and GC passed");
}
