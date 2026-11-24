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

static void test_trivial_range_move() {
  for (uint64_t stride : {1, 3, 4, 12, 32, 33}) {
    for (bool right : {false, true}) {
      constexpr uint64_t length = 49;
      auto *v = lang_vec_new_v1(length, stride, nullptr, nullptr);
      std::vector<unsigned char> original(length * stride);
      for (uint64_t byte = 0; byte < original.size(); ++byte)
        original[byte] = static_cast<unsigned char>(byte * 13);
      for (uint64_t i = 0; i < length; ++i)
        lang_vec_init_v1(v, original.data() + i * stride);
      const uint64_t destination = right ? 2 : 0;
      const uint64_t source = right ? 0 : 2;
      constexpr uint64_t count = length - 2;
      const auto before = allocation;
      if (!lang_vec_try_move_trivial_v1(v, destination, source, count) ||
          !lang_vec_try_move_trivial_v1(v, length, length, 0) ||
          allocation != before || v->length != length || v->initialized != length)
        std::abort();
      for (uint64_t i = 0; i < length; ++i) {
        const uint64_t expected = i >= destination && i - destination < count
            ? source + i - destination : i;
        if (memcmp(lang_vec_at_v1(v, i), original.data() + expected * stride,
                   stride) != 0)
          std::abort();
      }
      lang_vec_release_v1(v);
    }
  }

  drops.clear();
  auto *owned = lang_vec_new_v1(2, sizeof(int), copy, drop);
  for (int i : {1, 2}) lang_vec_init_v1(owned, &i);
  if (lang_vec_try_move_trivial_v1(owned, 1, 0, 1) ||
      *static_cast<int *>(lang_vec_at_v1(owned, 1)) != 2 ||
      !drops.empty())
    std::abort();
  lang_vec_release_v1(owned);
  if (drops != std::vector<int>{2, 1}) std::abort();
  drops.clear();
}

static void test_trivial_extend() {
  for (uint64_t stride : {1, 3, 4, 12, 32, 33}) {
    auto *values = lang_vec_new_v1(3, stride, nullptr, nullptr);
    auto *extra = lang_vec_new_v1(5, stride, nullptr, nullptr);
    std::vector<unsigned char> first(3 * stride), second(5 * stride);
    for (uint64_t byte = 0; byte < first.size(); ++byte)
      first[byte] = static_cast<unsigned char>(byte * 7 + 1);
    for (uint64_t byte = 0; byte < second.size(); ++byte)
      second[byte] = static_cast<unsigned char>(byte * 11 + 3);
    for (uint64_t i = 0; i < 3; ++i)
      lang_vec_init_v1(values, first.data() + i * stride);
    for (uint64_t i = 0; i < 5; ++i)
      lang_vec_init_v1(extra, second.data() + i * stride);
    const auto before = allocation;
    if (!lang_vec_extend_trivial_v1(values, extra) ||
        values->length != 8 || values->initialized != 8 ||
        values->capacity != 12 || allocation != before + 1)
      std::abort();
    for (uint64_t i = 0; i < 3; ++i)
      if (memcmp(lang_vec_at_v1(values, i), first.data() + i * stride,
                 stride) != 0)
        std::abort();
    for (uint64_t i = 0; i < 5; ++i)
      if (memcmp(lang_vec_at_v1(values, 3 + i), second.data() + i * stride,
                 stride) != 0)
        std::abort();
    if (extra->length != 5 || extra->initialized != 5 ||
        memcmp(lang_vec_at_v1(extra, 0), second.data(), stride) != 0)
      std::abort();
    lang_vec_release_v1(extra);
    lang_vec_release_v1(values);

    // Self extend uses the original count and the payload after growth.
    auto *self = lang_vec_new_v1(3, stride, nullptr, nullptr);
    for (uint64_t i = 0; i < 3; ++i)
      lang_vec_init_v1(self, first.data() + i * stride);
    if (!lang_vec_extend_trivial_v1(self, self) || self->length != 6 ||
        self->initialized != 6 || self->capacity != 6)
      std::abort();
    for (uint64_t i = 0; i < 6; ++i)
      if (memcmp(lang_vec_at_v1(self, i), first.data() + (i % 3) * stride,
                 stride) != 0)
        std::abort();
    lang_vec_release_v1(self);

    // Self extend inside existing capacity allocates nothing.
    auto *roomy = lang_vec_new_v1(2, stride, nullptr, nullptr);
    for (uint64_t i = 0; i < 2; ++i)
      lang_vec_init_v1(roomy, first.data() + i * stride);
    lang_vec_reserve_v1(roomy, 8);
    const uint64_t roomy_capacity = roomy->capacity;
    const auto roomy_before = allocation;
    if (!lang_vec_extend_trivial_v1(roomy, roomy) || roomy->length != 4 ||
        roomy->initialized != 4 || roomy->capacity != roomy_capacity ||
        allocation != roomy_before)
      std::abort();
    lang_vec_release_v1(roomy);
  }

  // Empty sources are successful no-ops, even with an active pin.
  auto *empty = lang_vec_new_v1(0, sizeof(int), nullptr, nullptr);
  (void)lang_vec_begin_v1(empty);
  if (!lang_vec_extend_trivial_v1(empty, empty) || empty->length != 0 ||
      empty->initialized != 0)
    std::abort();
  lang_vec_end_v1(empty);
  lang_vec_release_v1(empty);

  // Managed or partially initialized vectors hand back to the element loop.
  drops.clear();
  auto *owned = lang_vec_new_v1(2, sizeof(int), copy, drop);
  for (int i : {1, 2}) lang_vec_init_v1(owned, &i);
  auto *trivial = lang_vec_new_v1(1, sizeof(int), nullptr, nullptr);
  int three = 3;
  lang_vec_init_v1(trivial, &three);
  if (lang_vec_extend_trivial_v1(owned, owned) ||
      lang_vec_extend_trivial_v1(trivial, owned) || owned->length != 2 ||
      trivial->length != 1 || *static_cast<int *>(lang_vec_at_v1(trivial, 0)) != 3 ||
      !drops.empty())
    std::abort();
  lang_vec_release_v1(trivial);
  lang_vec_release_v1(owned);
  drops.clear();

  auto *partial = lang_vec_new_v1(4, sizeof(int), nullptr, nullptr);
  int four = 4;
  lang_vec_init_v1(partial, &four);
  auto *source = lang_vec_new_v1(1, sizeof(int), nullptr, nullptr);
  lang_vec_init_v1(source, &three);
  if (lang_vec_extend_trivial_v1(partial, source) ||
      lang_vec_extend_trivial_v1(source, partial) || partial->length != 4 ||
      partial->initialized != 1 || source->length != 1)
    std::abort();
  lang_vec_release_v1(source);
  lang_vec_release_v1(partial);

  auto *bytes2 = lang_vec_new_v1(1, 2, nullptr, nullptr);
  auto *bytes4 = lang_vec_new_v1(1, 4, nullptr, nullptr);
  unsigned char pair[4] = {1, 2, 3, 4};
  lang_vec_init_v1(bytes2, pair);
  lang_vec_init_v1(bytes4, pair);
  if (lang_vec_extend_trivial_v1(bytes2, bytes4) || bytes2->length != 1)
    std::abort();
  lang_vec_release_v1(bytes4);
  lang_vec_release_v1(bytes2);

  // Internal initialized-prefix edge: initialized > length is not the exact
  // prefix the single publication produces, so both roles fall back unchanged.
  auto *long_prefix = lang_vec_new_v1(4, sizeof(int), nullptr, nullptr);
  for (int i : {11, 22, 33, 44}) lang_vec_init_v1(long_prefix, &i);
  long_prefix->length = 1;
  auto *short_source = lang_vec_new_v1(1, sizeof(int), nullptr, nullptr);
  int five = 5;
  lang_vec_init_v1(short_source, &five);
  const auto prefix_allocations = allocation;
  if (lang_vec_extend_trivial_v1(long_prefix, short_source) ||
      lang_vec_extend_trivial_v1(short_source, long_prefix) ||
      long_prefix->length != 1 || long_prefix->initialized != 4 ||
      short_source->length != 1 || short_source->initialized != 1 ||
      allocation != prefix_allocations)
    std::abort();
  auto *prefix_values = static_cast<int *>(long_prefix->data);
  for (int i = 0; i < 4; ++i)
    if (prefix_values[i] != (i + 1) * 11) std::abort();
  long_prefix->length = 4; // restore before release
  lang_vec_release_v1(short_source);
  lang_vec_release_v1(long_prefix);

  // The same raw prefix on a managed Vec stays on the element loop with no
  // copy or drop callback.
  drops.clear();
  auto *owned_prefix = lang_vec_new_v1(4, sizeof(int), copy, drop);
  for (int i : {11, 22, 33, 44}) lang_vec_init_v1(owned_prefix, &i);
  owned_prefix->length = 2;
  auto *owned_source = lang_vec_new_v1(1, sizeof(int), copy, drop);
  lang_vec_init_v1(owned_source, &five);
  if (lang_vec_extend_trivial_v1(owned_prefix, owned_source) ||
      lang_vec_extend_trivial_v1(owned_source, owned_prefix) ||
      owned_prefix->length != 2 || owned_prefix->initialized != 4 ||
      owned_source->length != 1 || !drops.empty())
    std::abort();
  owned_prefix->length = 4; // restore before release
  lang_vec_release_v1(owned_source);
  lang_vec_release_v1(owned_prefix);
  if (drops != std::vector<int>{5, 44, 33, 22, 11}) std::abort();
  drops.clear();

  // Final length and byte-size overflow fall back unchanged so the public
  // loop can raise its Panic at the same per-element step.
  auto *overflow = lang_vec_new_v1(1, sizeof(int), nullptr, nullptr);
  int seven = 7;
  lang_vec_init_v1(overflow, &seven);
  const uint64_t saved_capacity = overflow->capacity;
  const void *saved_data = overflow->data;
  overflow->length = UINT64_MAX;
  overflow->initialized = UINT64_MAX;
  if (lang_vec_extend_trivial_v1(overflow, overflow) ||
      overflow->length != UINT64_MAX ||
      overflow->initialized != UINT64_MAX ||
      overflow->capacity != saved_capacity || overflow->data != saved_data)
    std::abort();
  overflow->length = 1;
  overflow->initialized = 1;
  overflow->stride = UINT64_MAX;
  if (lang_vec_extend_trivial_v1(overflow, overflow) ||
      overflow->length != 1 || overflow->initialized != 1 ||
      overflow->stride != UINT64_MAX || overflow->capacity != saved_capacity ||
      overflow->data != saved_data)
    std::abort();
  overflow->stride = sizeof(int);
  lang_vec_release_v1(overflow);

  // A failed growth allocation returns false and publishes no Vec change.
  auto *oom_values = lang_vec_new_v1(1, sizeof(int), nullptr, nullptr);
  int eight = 8;
  lang_vec_init_v1(oom_values, &eight);
  auto *oom_extra = lang_vec_new_v1(2, sizeof(int), nullptr, nullptr);
  for (int i : {9, 10}) lang_vec_init_v1(oom_extra, &i);
  const uint64_t oom_capacity = oom_values->capacity;
  const void *oom_data = oom_values->data;
  const auto oom_allocations = allocation;
  fail_at = allocation + 1;
  const bool oom_result = lang_vec_extend_trivial_v1(oom_values, oom_extra);
  const auto oom_used = allocation;
  fail_at = 0;
  if (oom_result || oom_used != oom_allocations + 1 ||
      oom_values->length != 1 || oom_values->initialized != 1 ||
      oom_values->capacity != oom_capacity || oom_values->data != oom_data ||
      *static_cast<int *>(lang_vec_at_v1(oom_values, 0)) != eight ||
      oom_extra->length != 2)
    std::abort();
  lang_vec_release_v1(oom_extra);
  lang_vec_release_v1(oom_values);
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
  test_trivial_range_move();
  test_trivial_extend();

  std::puts("reverse-index-and-prefix-ok");
  return 0;
}
