#include "../../runtime/native/system.hpp"
#include "../../runtime/native/vec.hpp"
#include <cstdio>

int main() {
  auto *bytes = lang_vec_new_v1(2, 1, nullptr, nullptr);
  const unsigned char first = 71, second = 92;
  lang_vec_init_v1(bytes, &first);
  lang_vec_init_v1(bytes, &second);
  if (lang_sys_getrandom_vec_v1(bytes, 3, 0) != -22 ||
      lang_sys_getdents_vec_v1(-1, bytes, 3) != -22 ||
      lang_sys_getcwd_vec_v1(bytes, 3) != -22 ||
      lang_sys_readlink_vec_v1("x", 1, bytes, 3) != -22) return 1;
  if (static_cast<unsigned char *>(bytes->data)[0] != first ||
      static_cast<unsigned char *>(bytes->data)[1] != second) return 2;
  if (lang_sys_getdents_vec_v1(-1, bytes, 2) != -9) return 3;
  if (lang_sys_getrandom_vec_v1(bytes, 0, ~uint32_t{0}) != 0) return 4;
  auto *empty = lang_vec_new_v1(0, 1, nullptr, nullptr);
  if (lang_sys_getrandom_vec_v1(empty, 0, 0) != 0 ||
      lang_sys_getcwd_vec_v1(empty, 0) != -22) return 5;
  auto *remaining = lang_vec_new_v1(1, sizeof(LangTimespec), nullptr, nullptr);
  const LangTimespec saved{123, 456}, bad{0, -1}, zero{0, 0};
  lang_vec_init_v1(remaining, &saved);
  if (lang_sys_nanosleep_vec_v1(&bad, remaining) != -22 ||
      lang_sys_nanosleep_vec_v1(&zero, remaining) != 0 ||
      lang_sys_nanosleep_vec_v1(&zero, bytes) != -22) return 6;
  const auto *value = static_cast<const LangTimespec *>(remaining->data);
  if (value->seconds != saved.seconds || value->nanoseconds != saved.nanoseconds) return 7;
  lang_vec_release_v1(remaining);
  lang_vec_release_v1(empty);
  lang_vec_release_v1(bytes);
  std::puts("system buffers: bounds and out-parameter rules passed");
}
