#include "../../runtime/native/vec.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
static_assert(sizeof(LangVec) == 80, "LangVec capacity ABI must stay 80 bytes");
static constexpr uint64_t kVecHeaderBytes =
    (static_cast<uint64_t>(sizeof(LangVec)) + 15) & ~uint64_t{15};
static_assert(kVecHeaderBytes == 80, "Vec header bytes changed");
static std::vector<int> drops;
static unsigned observed_create = 0;
static unsigned observed_element_drop = 0;
static unsigned observed_final_free = 0;
static unsigned observed_indices[16] = {};
static unsigned observed_index_count = 0;
static bool track_observed_indices = false;
extern "C" void lang_vec_observe_v1(uint32_t event, const void *, uint64_t detail) {
  if (event == 1) ++observed_create;
  if (event == 4) {
    ++observed_element_drop;
    if (track_observed_indices && observed_index_count < 16)
      observed_indices[observed_index_count++] = static_cast<unsigned>(detail);
  }
  if (event == 5) ++observed_final_free;
}
static void copy(void *) {}
static void drop(void *p) { drops.push_back(*static_cast<int *>(p)); }
extern "C" void *__real_lang_alloc_v1(uint64_t, uint64_t);
extern "C" void __real_lang_free_v1(void *);
static unsigned allocation = 0, fail_at = 0;
static uint64_t last_allocation_size = 0, last_allocation_align = 0;
static unsigned frees = 0, free_calls = 0;
static void *last_freed = nullptr;
extern "C" void *__wrap_lang_alloc_v1(uint64_t size, uint64_t align) {
  ++allocation;
  last_allocation_size = size;
  last_allocation_align = align;
  return fail_at && allocation == fail_at ? nullptr : __real_lang_alloc_v1(size, align);
}
extern "C" void __wrap_lang_free_v1(void *pointer) {
  ++free_calls;
  if (pointer) {
    ++frees;
    last_freed = pointer;
  }
  __real_lang_free_v1(pointer);
}

struct S12 { int a, b, c; };
struct S24 { uint64_t a, b, c; };

static int test_nullable_callbacks() {
  drops.clear();
  auto *v = lang_vec_new_v1(3, sizeof(int), nullptr, nullptr);
  if (!v) return 20;
  for (int i : {1, 2, 3}) lang_vec_init_v1(v, &i);
  if (*static_cast<int *>(lang_vec_at_v1(v, 2)) != 3) return 21;
  int nine = 9;
  lang_vec_set_v1(v, 1, &nine);
  auto *slice = lang_vec_slice_v1(v, 1, 3);
  if (*static_cast<int *>(lang_vec_at_v1(slice, 0)) != 9 ||
      *static_cast<int *>(lang_vec_at_v1(slice, 1)) != 3) return 22;
  lang_vec_release_v1(slice);
  if (!drops.empty()) return 23;
  lang_vec_release_v1(v);
  if (!drops.empty()) return 24;

  auto *empty = lang_vec_new_v1(0, sizeof(int), nullptr, nullptr);
  auto *empty_slice = lang_vec_slice_v1(empty, 0, 0);
  if (lang_vec_length_v1(empty_slice) != 0) return 25;
  lang_vec_release_v1(empty_slice);
  lang_vec_release_v1(empty);

  auto *partial = lang_vec_new_v1(4, sizeof(int), nullptr, nullptr);
  for (int i : {4, 5}) lang_vec_init_v1(partial, &i);
  lang_vec_release_v1(partial);
  if (!drops.empty()) return 26;
  return 0;
}

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

static int test_single_allocation_storage() {
  drops.clear();
  const unsigned base_allocations = allocation;
  const unsigned base_frees = frees;
  const unsigned base_free_calls = free_calls;

  // An empty Vec owns exactly one descriptor allocation and no dummy payload.
  auto *empty = lang_vec_new_v1(0, sizeof(int), copy, drop);
  if (!empty) return 30;
  if (allocation != base_allocations + 1) return 31;
  if (last_allocation_size != kVecHeaderBytes) return 32;
  if (last_allocation_align != 16) return 33;
  if (empty->data != nullptr) return 34;
  if (reinterpret_cast<uintptr_t>(empty) % 16 != 0) return 35;

  // Slicing an empty Vec creates another independent empty descriptor.
  auto *empty_slice = lang_vec_slice_v1(empty, 0, 0);
  if (!empty_slice) return 36;
  if (allocation != base_allocations + 2) return 37;
  if (last_allocation_size != kVecHeaderBytes ||
      last_allocation_align != 16) return 38;
  if (empty_slice == empty || empty_slice->data != nullptr) return 39;
  lang_vec_release_v1(empty_slice);
  if (frees != base_frees + 1 || last_freed != empty_slice) return 40;
  lang_vec_release_v1(empty);
  if (frees != base_frees + 2 || last_freed != empty) return 41;
  if (!drops.empty()) return 42;

  // A non-empty Vec keeps its payload inside the same block, after the
  // 16-byte-aligned descriptor, with every full element value preserved.
  auto *full = lang_vec_new_v1(3, sizeof(int), copy, drop);
  if (!full) return 43;
  if (allocation != base_allocations + 3) return 44;
  if (last_allocation_size != kVecHeaderBytes + 3 * sizeof(int)) return 45;
  if (last_allocation_align != 16) return 46;
  const auto full_address = reinterpret_cast<uintptr_t>(full);
  if (reinterpret_cast<uintptr_t>(full->data) != full_address + kVecHeaderBytes)
    return 47;
  if (reinterpret_cast<uintptr_t>(full->data) % 16 != 0) return 48;
  if (reinterpret_cast<uintptr_t>(full->data) <
      full_address + sizeof(LangVec)) return 49;
  const int values[] = {11, 22, 33};
  for (int value : values) lang_vec_init_v1(full, &value);
  int replaced = 44;
  lang_vec_set_v1(full, 1, &replaced);
  if (*static_cast<int *>(lang_vec_at_v1(full, 0)) != 11 ||
      *static_cast<int *>(lang_vec_at_v1(full, 1)) != 44 ||
      *static_cast<int *>(lang_vec_at_v1(full, 2)) != 33) return 50;
  drops.clear();

  // A slice is one more self-contained Vec allocation.
  auto *slice = lang_vec_slice_v1(full, 1, 3);
  if (!slice) return 51;
  if (allocation != base_allocations + 4) return 52;
  if (last_allocation_size != kVecHeaderBytes + 2 * sizeof(int)) return 53;
  if (last_allocation_align != 16) return 54;
  if (reinterpret_cast<uintptr_t>(slice->data) !=
      reinterpret_cast<uintptr_t>(slice) + kVecHeaderBytes) return 55;
  if (*static_cast<int *>(lang_vec_at_v1(slice, 0)) != 44 ||
      *static_cast<int *>(lang_vec_at_v1(slice, 1)) != 33) return 56;
  lang_vec_release_v1(slice);
  if (frees != base_frees + 3 || last_freed != slice) return 57;
  if (drops != std::vector<int>{33, 44}) return 58;
  drops.clear();
  lang_vec_release_v1(full);
  if (frees != base_frees + 4 || last_freed != full) return 59;
  if (drops != std::vector<int>{33, 44, 11}) return 60;
  drops.clear();

  // Partially initialized destructors still drop only the initialized prefix.
  auto *partial = lang_vec_new_v1(4, sizeof(int), copy, drop);
  if (!partial) return 61;
  if (allocation != base_allocations + 5) return 62;
  const int prefix[] = {7, 8};
  for (int value : prefix) lang_vec_init_v1(partial, &value);
  lang_vec_release_v1(partial);
  if (frees != base_frees + 5 || last_freed != partial) return 63;
  if (drops != std::vector<int>{8, 7}) return 64;
  drops.clear();

  // A payload above the small-class ceiling still shares one aligned block.
  auto *large = lang_vec_new_v1(600, sizeof(uint64_t), nullptr, nullptr);
  if (!large) return 65;
  if (allocation != base_allocations + 6) return 66;
  if (last_allocation_size != kVecHeaderBytes + 600 * sizeof(uint64_t))
    return 67;
  if (last_allocation_align != 16) return 68;
  if (reinterpret_cast<uintptr_t>(large->data) !=
      reinterpret_cast<uintptr_t>(large) + kVecHeaderBytes) return 69;
  if (reinterpret_cast<uintptr_t>(large->data) % 16 != 0) return 70;
  uint64_t first = 0x1122334455667788ULL;
  lang_vec_init_v1(large, &first);
  if (*static_cast<uint64_t *>(lang_vec_at_v1(large, 0)) != first) return 71;
  lang_vec_release_v1(large);
  if (frees != base_frees + 6 || last_freed != large) return 72;

  if (allocation - base_allocations != 6) return 73;
  if (frees - base_frees != 6) return 74;
  if (free_calls - base_free_calls != 6) return 75;
  return 0;
}

int main(int argc, char **argv) {
  std::string mode = argc > 1 ? argv[1] : "normal";
  if (mode == "null-observe") {
    observed_create = observed_element_drop = observed_final_free = 0;
    observed_index_count = 0;
    track_observed_indices = true;
    auto *v = lang_vec_new_v1(4, sizeof(int), nullptr, nullptr);
    for (int i : {1, 2, 3, 4}) lang_vec_init_v1(v, &i);
    lang_vec_release_v1(v);
    if (observed_create != 1 || observed_element_drop != 4 ||
        observed_final_free != 1 || observed_index_count != 4)
      return 10;
    for (unsigned i = 0; i < 4; ++i)
      if (observed_indices[i] != 3 - i) return 11;
    observed_element_drop = observed_final_free = 0;
    observed_index_count = 0;
    auto *partial = lang_vec_new_v1(4, sizeof(int), nullptr, nullptr);
    for (int i : {8, 9}) lang_vec_init_v1(partial, &i);
    lang_vec_release_v1(partial);
    if (observed_element_drop != 2 || observed_final_free != 1 ||
        observed_index_count != 2 || observed_indices[0] != 1 ||
        observed_indices[1] != 0)
      return 12;
    return 0;
  }
  if (mode == "null-iteration-resize") {
    auto *v = lang_vec_new_v1(2, sizeof(int), nullptr, nullptr);
    lang_vec_begin_v1(v);
    lang_vec_check_resize_v1(v);
    return 9;
  }
  if (mode == "object-oom" || mode == "storage-oom") {
    allocation = 0;
    fail_at = mode == "object-oom" ? 1 : 2;
    if (mode == "storage-oom") {
      // Descriptor and payload are one allocation now. After one Vec succeeds,
      // fail the next combined allocation so this mode still exercises OOM
      // after a prior successful allocation.
      auto *parent = lang_vec_new_v1(1, sizeof(int), copy, drop);
      lang_vec_slice_v1(parent, 0, 1);
      return 9;
    }
    lang_vec_new_v1(2, sizeof(int), copy, drop); return 9;
  }
  if (mode == "size-overflow") { lang_vec_new_v1(UINT64_MAX, 8, copy, drop); return 9; }
  if (mode == "size-overflow-total") {
    // The multiplication fits, but adding the 16-aligned descriptor overflows.
    lang_vec_new_v1(UINT64_MAX / 8, 8, copy, drop); return 9;
  }
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
  int nullable = test_nullable_callbacks();
  if (nullable) return nullable;
  int single = test_single_allocation_storage();
  if (single) return single;

  std::puts("reverse-index-and-prefix-ok");
  return 0;
}
