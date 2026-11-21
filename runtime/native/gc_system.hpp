#ifndef LANG_RUNTIME_NATIVE_GC_SYSTEM_HPP
#define LANG_RUNTIME_NATIVE_GC_SYSTEM_HPP

#if defined(__APPLE__) && defined(__aarch64__)
#include "darwin.hpp"
#include <unistd.h>
#elif defined(__linux__) && defined(__x86_64__)
#include "linux.hpp"
#else
#error "Woozy GC requires Linux x86_64 or macOS arm64"
#endif

namespace lang::native::gc_system {

#if defined(__APPLE__)
using ThreadId = int64_t;
using darwin::cpu_pause;
inline int64_t raw_current_tid() noexcept { return darwin::thread_id(); }
inline int64_t write(int fd, const char *bytes, uint64_t size) noexcept {
  const auto result = ::write(fd, bytes, size);
  return result < 0 ? -errno : result;
}
[[noreturn]] inline void exit_panic() noexcept { _exit(101); }
#else
using ThreadId = int32_t;
inline void cpu_pause() noexcept { __asm__ volatile("pause" ::: "memory"); }
inline int64_t raw_current_tid() noexcept {
  return linux_x86_64::syscall6(186 /* gettid */, 0, 0, 0, 0, 0, 0);
}
inline int64_t write(int fd, const char *bytes, uint64_t size) noexcept {
  return linux_x86_64::syscall6(1 /* write */, fd, reinterpret_cast<long>(bytes),
                                static_cast<long>(size), 0, 0, 0);
}
[[noreturn]] inline void exit_panic() noexcept {
  linux_x86_64::syscall6(231 /* exit_group */, 101, 0, 0, 0, 0, 0);
  __builtin_unreachable();
}
#endif

// Diagnostic fields in the v1 external structs have a fixed signed 32-bit ABI.
// Runtime decisions always use the full ID. A diagnostic -1 means too large.
inline int32_t diagnostic_tid(ThreadId tid) noexcept {
#if defined(__APPLE__)
  if (tid > INT32_MAX) return -1;
#endif
  return static_cast<int32_t>(tid);
}

} // namespace lang::native::gc_system

#endif
