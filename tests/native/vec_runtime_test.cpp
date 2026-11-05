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
extern "C" void *__real_lang_alloc_v1(uint64_t, uint64_t);
static unsigned allocation = 0, fail_at = 0;
extern "C" void *__wrap_lang_alloc_v1(uint64_t size, uint64_t align) {
  return fail_at && ++allocation == fail_at ? nullptr : __real_lang_alloc_v1(size, align);
}

struct S12 { int a, b, c; };
struct S24 { uint64_t a, b, c; };

static void test_freestanding_memory() {
  // Test memset
  char buf[32];
  memset(buf, 0xaa, sizeof(buf));
  for (size_t i = 0; i < sizeof(buf); ++i) {
    if (static_cast<unsigned char>(buf[i]) != 0xaa) std::abort();
  }
  memset(buf, 0, 0); // zero length should not crash or modify
  if (static_cast<unsigned char>(buf[0]) != 0xaa) std::abort();

  // Test memcpy
  char src[16] = "0123456789abcde";
  char dst[16];
  memcpy(dst, src, sizeof(src));
  if (memcmp(dst, src, sizeof(src)) != 0) std::abort();
  memcpy(dst, src, 0); // zero length

  // Test memmove non-overlapping
  memmove(dst, src, sizeof(src));
  if (memcmp(dst, src, sizeof(src)) != 0) std::abort();

  // Test memmove backward overlap (dst > src)
  char overlap[32] = "abcdefghijklmnop";
  memmove(overlap + 4, overlap, 12);
  if (memcmp(overlap + 4, "abcdefghijkl", 12) != 0) std::abort();

  // Test memmove forward overlap (dst < src)
  char overlap2[32] = "0123456789abcdef";
  memmove(overlap2, overlap2 + 4, 12);
  if (memcmp(overlap2, "456789abcdef", 12) != 0) std::abort();
  if (memcmp("a", "b", 1) >= 0 || memcmp("b", "a", 1) <= 0 ||
      memcmp("a", "b", 0) != 0) std::abort();
  memmove(overlap2, overlap2, sizeof(overlap2));
  if (memcmp(overlap2, "456789abcdef", 12) != 0) std::abort();
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

  // Test stride 12 (3 fields of i32) and stride 24 (3 fields of u64)
  auto *v12 = lang_vec_new_v1(2, sizeof(S12), copy, drop);
  S12 s12{101, 202, 303};
  lang_vec_init_v1(v12, &s12);
  auto *p12 = static_cast<S12 *>(lang_vec_at_v1(v12, 0));
  if (p12->a != 101 || p12->b != 202 || p12->c != 303) return 10;
  lang_vec_release_v1(v12);

  auto *v24 = lang_vec_new_v1(2, sizeof(S24), copy, drop);
  S24 s24{1001, 2002, 3003};
  lang_vec_init_v1(v24, &s24);
  auto *p24 = static_cast<S24 *>(lang_vec_at_v1(v24, 0));
  if (p24->a != 1001 || p24->b != 2002 || p24->c != 3003) return 11;
  lang_vec_release_v1(v24);

  test_freestanding_memory();

  std::puts("reverse-index-and-prefix-ok");
  return 0;
}
