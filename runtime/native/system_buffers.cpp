#include "system.hpp"
#include "vec.hpp"

#if defined(__APPLE__)
#include "../darwin/posix.hpp"
#include "gc_program.hpp"
#include <cstdlib>
#include <cstring>
#include <limits.h>
#include <mach-o/dyld.h>
#endif

namespace {
bool bytes(LangVec *value, uint64_t count) noexcept {
  return value && value->stride == 1 && count <= value->length &&
         count <= value->initialized && !value->copy && !value->drop;
}
}
extern "C" int64_t lang_sys_getcwd_vec_v1(LangVec *value, uint64_t count) noexcept {
  if (!bytes(value, count)) return -22;
  return lang_sys_getcwd_v1(static_cast<char *>(value->data), count);
}
extern "C" int64_t lang_sys_getdents_vec_v1(int32_t fd, LangVec *value,
                                            uint64_t count) noexcept {
  if (!bytes(value, count)) return -22;
  return lang_sys_getdents_v1(fd, value->data, count);
}
extern "C" int64_t lang_sys_readlink_vec_v1(const char *path, uint64_t length,
                                            LangVec *value, uint64_t count) noexcept {
  if (!bytes(value, count)) return -22;
  return lang_sys_readlink_v1(path, length, value->data, count);
}
extern "C" int64_t lang_sys_executable_path_vec_v1(LangVec *value,
                                                   uint64_t count) noexcept {
  if (!bytes(value, count) || count == 0 || !value->data) return -22;
#if defined(__APPLE__)
  const lang::native::gc_program::SafeGuard safe;
  char path[PATH_MAX];
  char *allocated = nullptr;
  char *source = path;
  uint32_t length = sizeof(path);
  if (_NSGetExecutablePath(path, &length) != 0) {
    allocated = static_cast<char *>(std::malloc(length));
    if (!allocated) return -12;
    if (_NSGetExecutablePath(allocated, &length) != 0) {
      std::free(allocated);
      return -36;
    }
    source = allocated;
  }
  char *resolved = realpath(source, nullptr);
  const int error = errno;
  std::free(allocated);
  if (!resolved) return lang::native::darwin::error(error);
  const size_t size = std::strlen(resolved);
  if (size < count) std::memcpy(value->data, resolved, size);
  std::free(resolved);
  return size < count ? static_cast<int64_t>(size) : -34;
#else
  constexpr char path[] = "/proc/self/exe";
  const int64_t size = lang_sys_readlink_v1(path, sizeof(path) - 1,
                                            value->data, count);
  return size >= 0 && static_cast<uint64_t>(size) == count ? -34 : size;
#endif
}
extern "C" int64_t lang_sys_getrandom_vec_v1(LangVec *value, uint64_t count,
                                             uint32_t flags) noexcept {
  if (!bytes(value, count)) return -22;
  return lang_sys_getrandom_v1(value->data, count, flags);
}
extern "C" int64_t lang_sys_nanosleep_vec_v1(const LangTimespec *request,
                                             LangVec *remaining) noexcept {
  if (!remaining || remaining->stride != sizeof(LangTimespec) ||
      remaining->length < 1 || remaining->initialized < 1 ||
      remaining->copy || remaining->drop) return -22;
  return lang_sys_nanosleep_v1(request, static_cast<LangTimespec *>(remaining->data));
}
