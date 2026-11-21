#ifndef LANG_RUNTIME_NATIVE_DARWIN_HPP
#define LANG_RUNTIME_NATIVE_DARWIN_HPP

#include <cerrno>
#include <cstdint>
#include <mach/mach_time.h>
#include <os/os_sync_wait_on_address.h>
#include <pthread.h>

namespace lang::native::darwin {

inline int64_t thread_id() noexcept {
  uint64_t id = 0;
  return pthread_threadid_np(nullptr, &id) == 0 && id <= INT64_MAX
             ? static_cast<int64_t>(id)
             : 0;
}

inline void cpu_pause() noexcept { __asm__ volatile("yield" ::: "memory"); }

inline int address_wait(uint32_t *address, uint32_t expected) noexcept {
  return os_sync_wait_on_address(address, expected, sizeof(*address),
                                 OS_SYNC_WAIT_ON_ADDRESS_NONE) < 0
             ? -errno
             : 0;
}

inline int address_wait_until(uint32_t *address, uint32_t expected,
                              uint64_t deadline) noexcept {
  if (__atomic_load_n(address, __ATOMIC_ACQUIRE) != expected)
    return 0;
  if (mach_absolute_time() >= deadline)
    return -ETIMEDOUT;
  return os_sync_wait_on_address_with_deadline(
             address, expected, sizeof(*address), OS_SYNC_WAIT_ON_ADDRESS_NONE,
             OS_CLOCK_MACH_ABSOLUTE_TIME, deadline) < 0
             ? -errno
             : 0;
}

inline int address_wake(uint32_t *address, bool all) noexcept {
  const int result = all
      ? os_sync_wake_by_address_all(address, sizeof(*address),
                                    OS_SYNC_WAKE_BY_ADDRESS_NONE)
      : os_sync_wake_by_address_any(address, sizeof(*address),
                                    OS_SYNC_WAKE_BY_ADDRESS_NONE);
  // A change to the address can beat the waiter's attempt to park.
  return result == 0 || errno == ENOENT ? 0 : -errno;
}

inline int monotonic_deadline(uint64_t nanoseconds, uint64_t &deadline) noexcept {
  mach_timebase_info_data_t timebase{};
  if (mach_timebase_info(&timebase) != KERN_SUCCESS ||
      !timebase.numer || !timebase.denom)
    return -EINVAL;
  // Divide first to keep the conversion in 64 bits. Round up to a full tick.
  const uint64_t whole = nanoseconds / timebase.numer;
  const uint64_t fraction = (nanoseconds % timebase.numer) * timebase.denom;
  const uint64_t ticks = fraction / timebase.numer +
      (fraction % timebase.numer != 0);
  const uint64_t now = mach_absolute_time();
  const uint64_t available = UINT64_MAX - now;
  deadline = ticks > available ||
      whole > (available - ticks) / timebase.denom
      ? UINT64_MAX
      : now + whole * timebase.denom + ticks;
  return 0;
}

} // namespace lang::native::darwin

#endif
