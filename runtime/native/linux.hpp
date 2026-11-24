#ifndef LANG_RUNTIME_NATIVE_LINUX_HPP
#define LANG_RUNTIME_NATIVE_LINUX_HPP

#include <cstdint>

// Private Linux x86_64 syscall substrate: lang-native-linux/1.
//
// This header is shared by runtime translation units that need direct Linux
// system calls without libc or libstdc++. It is not a public runtime ABI and
// source programs cannot reach it: the typed `os` module exposes only
// LangSys-style entry points declared in system.hpp.
//
// syscall6 performs exactly one Linux x86_64 `syscall` instruction with the
// standard argument registers (rdi, rsi, rdx, r10, r8, r9) and returns the raw
// kernel result. On failure that result is a negative errno in [-4095, -1];
// callers must test `result < 0` rather than only comparing with -1. The
// wrapper never sets a global errno, retries, or normalises errors.
//
// The `memory` clobber prevents the compiler from moving memory accesses across
// the call, which matters because kernel calls can read or write any caller
// buffer. rcx and r11 are clobbered by the `syscall` instruction itself.
//
// copy_path is the single path-validation helper used by every path-taking
// native primitive. It borrows `source` for the duration of the call, copies
// exactly `length` bytes into `destination`, and writes a terminating NUL at
// destination[length]. It never reads source[length] or beyond, never accepts
// an embedded NUL, and never truncates:
//   * length >= 4096                    -> -ENAMETOOLONG
//   * source == nullptr && length != 0 -> -EFAULT
//   * embedded NUL in the range        -> -EINVAL
//   * otherwise                        -> 0
// A zero length with a null source is the empty path and is accepted.
//
// This substrate is implemented only for Linux x86_64; there is no fallback.

#ifndef LANG_NATIVE_NO_STACK_PROTECTOR
#if defined(__GNUC__) || defined(__clang__)
#define LANG_NATIVE_NO_STACK_PROTECTOR __attribute__((no_stack_protector))
#else
#define LANG_NATIVE_NO_STACK_PROTECTOR
#endif
#endif

namespace lang {
namespace native {
namespace linux_x86_64 {

constexpr uint64_t kPathMax = 4096;

constexpr int64_t kErrorFault = -14;        // EFAULT
constexpr int64_t kErrorInterrupted = -4;   // EINTR
constexpr int64_t kErrorInval = -22;        // EINVAL
constexpr int64_t kErrorNameTooLong = -36;  // ENAMETOOLONG

static_assert(sizeof(long) == 8,
              "Linux x86_64 syscall arguments and results must be 64-bit");
static_assert(sizeof(void *) == 8,
              "Linux x86_64 pointers must be 64-bit");

// One raw Linux x86_64 system call. The arguments are passed as signed long so
// negative values (AT_FDCWD, negative offsets) survive the register ABI.
inline int64_t syscall6(long number, long arg1, long arg2, long arg3,
                        long arg4, long arg5, long arg6) noexcept {
  register long r10 __asm__("r10") = arg4;
  register long r8 __asm__("r8") = arg5;
  register long r9 __asm__("r9") = arg6;
  long result;
  __asm__ volatile("syscall"
                   : "=a"(result)
                   : "a"(number), "D"(arg1), "S"(arg2), "d"(arg3),
                     "r"(r10), "r"(r8), "r"(r9)
                   : "rcx", "r11", "memory", "cc");
  return result;
}

// Copies and validates one borrowed path into caller-provided stack storage.
// destination must provide at least kPathMax bytes.
LANG_NATIVE_NO_STACK_PROTECTOR
inline int64_t copy_path(const char *source, uint64_t length,
                         char destination[kPathMax]) noexcept {
  if (length >= kPathMax) return kErrorNameTooLong;
  if (length != 0 && source == nullptr) return kErrorFault;

  for (uint64_t index = 0; index < length; ++index) {
    const char byte = source[index];
    if (byte == '\0') return kErrorInval;
    destination[index] = byte;
  }
  destination[length] = '\0';
  return 0;
}

// Copies and validates one borrowed directory-entry name. The name must be
// exactly one component: non-empty, no embedded NUL, no '/', and neither "."
// nor "..". Rejecting multi-component paths keeps descriptor-relative
// operations from following a nested symlink and lets a recursive walker
// revalidate every entry before it acts.
//   * length == 0                      -> -EINVAL
//   * length >= 4096                   -> -ENAMETOOLONG
//   * source == nullptr                -> -EFAULT
//   * ".", "..", NUL or '/'           -> -EINVAL
//   * otherwise                        -> 0
LANG_NATIVE_NO_STACK_PROTECTOR
inline int64_t copy_component(const char *source, uint64_t length,
                              char destination[kPathMax]) noexcept {
  if (length == 0) return kErrorInval;
  if (length >= kPathMax) return kErrorNameTooLong;
  if (source == nullptr) return kErrorFault;
  if (length == 1 && source[0] == '.') return kErrorInval;
  if (length == 2 && source[0] == '.' && source[1] == '.')
    return kErrorInval;

  for (uint64_t index = 0; index < length; ++index) {
    const char byte = source[index];
    if (byte == '\0' || byte == '/') return kErrorInval;
    destination[index] = byte;
  }
  destination[length] = '\0';
  return 0;
}

} // namespace linux_x86_64
} // namespace native
} // namespace lang

#endif // LANG_RUNTIME_NATIVE_LINUX_HPP
