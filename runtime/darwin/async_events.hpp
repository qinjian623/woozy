#ifndef LANG_RUNTIME_DARWIN_ASYNC_EVENTS_HPP
#define LANG_RUNTIME_DARWIN_ASYNC_EVENTS_HPP

#include "posix.hpp"

#include <fcntl.h>
#include <sys/event.h>
#include <unistd.h>

namespace lang::native::darwin {

using AsyncEvent = struct kevent;
// Private async ABI flags emitted for readable() and writable().
constexpr uint32_t kAsyncRead = 1;
constexpr uint32_t kAsyncWrite = 4;

inline int async_change(int queue, uintptr_t ident, int16_t filter,
                        uint16_t flags, uint32_t fflags, uint64_t token) noexcept {
  AsyncEvent event;
  EV_SET(&event, ident, filter, flags, fflags, 0,
         reinterpret_cast<void *>(static_cast<uintptr_t>(token)));
  int status;
  do {
    status = kevent(queue, &event, 1, nullptr, 0, nullptr);
  } while (status < 0 && errno == EINTR);
  return status < 0 ? static_cast<int>(error(errno)) : 0;
}

// The caller holds LaunchGuard before taking the executor lock.
inline int async_queue() noexcept {
  const int queue = kqueue();
  if (queue < 0) return static_cast<int>(error(errno));
  const int result = fcntl(queue, F_SETFD, FD_CLOEXEC) < 0
      ? static_cast<int>(error(errno))
      : async_change(queue, 0, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0);
  if (result < 0) {
    close(queue);
    return result;
  }
  return queue;
}

inline int async_register(int queue, int fd, uint32_t interests,
                          uint64_t token) noexcept {
  if (interests & ~(kAsyncRead | kAsyncWrite)) return -22;
  if (interests & kAsyncRead) {
    const int status = async_change(queue, fd, EVFILT_READ, EV_ADD, 0, token);
    if (status < 0) return status;
  }
  if (interests & kAsyncWrite)
    return async_change(queue, fd, EVFILT_WRITE, EV_ADD, 0, token);
  return 0;
}

inline void async_unregister(int queue, int fd, uint32_t interests) noexcept {
  if (interests & kAsyncRead)
    (void)async_change(queue, fd, EVFILT_READ, EV_DELETE, 0, 0);
  if (interests & kAsyncWrite)
    (void)async_change(queue, fd, EVFILT_WRITE, EV_DELETE, 0, 0);
}

inline int async_wake(int queue) noexcept {
  return async_change(queue, 0, EVFILT_USER, 0, NOTE_TRIGGER, 0);
}

inline bool async_live_read_watch(int queue, int fd, uint64_t token) noexcept {
  // Closing a descriptor removes its knote. EV_ENABLE without EV_ADD
  // checks that the old registration still exists even after fd reuse.
  return async_change(queue, fd, EVFILT_READ, EV_ENABLE, 0, token) == 0;
}

inline int async_notify_watch(int queue, uint64_t token) noexcept {
  return async_change(queue, token, EVFILT_USER, EV_ADD | EV_ONESHOT,
                      NOTE_TRIGGER, token);
}

inline int async_wait(int queue, AsyncEvent *events, int max_events,
                      int milliseconds) noexcept {
  const timespec timeout{milliseconds / 1000, (milliseconds % 1000) * 1000000L};
  const int count = kevent(queue, nullptr, 0, events, max_events,
                           milliseconds < 0 ? nullptr : &timeout);
  return count < 0 ? static_cast<int>(error(errno)) : count;
}

inline uint64_t async_token(const AsyncEvent &event) noexcept {
  return reinterpret_cast<uintptr_t>(event.udata);
}

} // namespace lang::native::darwin

#endif
