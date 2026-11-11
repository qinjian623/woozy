#include "system.hpp"
#include "vec.hpp"

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
