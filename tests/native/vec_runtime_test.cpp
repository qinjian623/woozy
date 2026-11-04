#include "../../runtime/native/vec.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
static std::vector<int> drops;
static void copy(void *) {}
static void drop(void *p) { drops.push_back(*static_cast<int *>(p)); }
extern "C" void *__real_malloc(size_t);
static unsigned allocation = 0, fail_at = 0;
extern "C" void *__wrap_malloc(size_t n) {
  return fail_at && ++allocation == fail_at ? nullptr : __real_malloc(n);
}
int main(int argc, char **argv) {
  std::string mode = argc > 1 ? argv[1] : "normal";
  if (mode == "object-oom" || mode == "storage-oom") {
    allocation = 0; fail_at = mode == "object-oom" ? 1 : 2;
    lang_vec_new_v1(2, sizeof(int), copy, drop); return 9;
  }
  if (mode == "size-overflow") { lang_vec_new_v1(UINT64_MAX, 8, copy, drop); return 9; }
  auto *v = lang_vec_new_v1(3, sizeof(int), copy, drop);
  for (int i : {1,2,3}) lang_vec_init_v1(v, &i);
  if (mode == "count-overflow") { v->refs = UINT64_MAX; lang_vec_retain_v1(v); return 9; }
  if (mode == "iteration-resize") { lang_vec_begin_v1(v); lang_vec_check_resize_v1(v); return 9; }
  lang_vec_retain_v1(v); lang_vec_release_v1(v);
  if (!drops.empty()) return 1;
  auto *slice = lang_vec_slice_v1(v, 1, 3);
  int value = 9; lang_vec_set_v1(slice, 0, &value);
  if (*static_cast<int *>(lang_vec_at_v1(v, 1)) != 2) return 2;
  lang_vec_release_v1(slice);
  drops.clear(); lang_vec_release_v1(v);
  if (drops != std::vector<int>{3,2,1}) return 3;
  // A partially initialized object drops only its initialized prefix.
  auto *partial = lang_vec_new_v1(4, sizeof(int), copy, drop);
  for (int i : {4,5}) lang_vec_init_v1(partial, &i);
  drops.clear(); lang_vec_release_v1(partial);
  if (drops != std::vector<int>{5,4}) return 4;
  std::puts("reverse-index-and-prefix-ok");
}
