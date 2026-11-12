// Default-core static ELF check: links the allocator/panic/Vec/Weak runtime but
// not gc_program.cpp. The program never calls lang_gc_program_init_v1, so the
// Vec publish windows must take the null-hook disabled path without adding a
// new dependency or requiring gettid. Exit code 0 is the runner marker.

#include "vec.hpp"

#include <cstdint>

namespace {

uint64_t g_copies = 0;
uint64_t g_drops = 0;

void copy(void *) noexcept { ++g_copies; }
void drop(void *) noexcept { ++g_drops; }

} // namespace

extern "C" int main() {
  LangVec *vec = lang_vec_new_v1(2, sizeof(uint64_t), copy, drop);
  uint64_t first = 1;
  uint64_t second = 2;
  lang_vec_init_v1(vec, &first);
  lang_vec_init_v1(vec, &second);

  uint64_t replacement = 9;
  lang_vec_set_v1(vec, 0, &replacement); // drop 1
  uint64_t pushed = 3;
  lang_vec_push_v1(vec, &pushed); // growth, transfer
  uint64_t fill = 7;
  lang_vec_resize_v1(vec, 5, &fill); // two logical copies
  lang_vec_resize_v1(vec, 3, nullptr); // drop 7, 7
  LangVec *slice = lang_vec_slice_v1(vec, 1, 3); // two logical copies
  lang_vec_release_v1(slice); // drop 2, 2
  lang_vec_clear_v1(vec); // drop 3, 2, 9
  if (vec->length != 0 || vec->initialized != 0) return 1;
  lang_vec_release_v1(vec);

  if (g_copies != 4) return 2;
  if (g_drops != 8) return 3;
  return 0;
}
