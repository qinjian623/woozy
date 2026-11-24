// Private runtime harness for shared mutable Vec growth.
//
// This file is test-only and may use host libc and POSIX fork/pipe. The
// production runtime sources remain freestanding.
#include "../../runtime/native/vec.hpp"

#include <cstddef>
#include <cstdint>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

static_assert(sizeof(LangVec) == 80, "LangVec must append capacity and Weak slot");
static_assert(offsetof(LangVec, refs) == 0, "refs offset changed");
static_assert(offsetof(LangVec, length) == 8, "length offset changed");
static_assert(offsetof(LangVec, initialized) == 16, "initialized offset changed");
static_assert(offsetof(LangVec, stride) == 24, "stride offset changed");
static_assert(offsetof(LangVec, iterations) == 32, "iterations offset changed");
static_assert(offsetof(LangVec, data) == 40, "data offset changed");
static_assert(offsetof(LangVec, copy) == 48, "copy offset changed");
static_assert(offsetof(LangVec, drop) == 56, "drop offset changed");
static_assert(offsetof(LangVec, capacity) == 64, "capacity offset changed");

static constexpr uint64_t kVecHeaderBytes =
    (static_cast<uint64_t>(sizeof(LangVec)) + 15) & ~uint64_t{15};
static_assert(kVecHeaderBytes == 80, "Vec payload offset changed");

namespace {

int g_failures = 0;

void check(bool condition, const char *text, int line) {
  if (condition) return;
  ++g_failures;
  std::fprintf(stderr, "FAIL:%d:%s\n", line, text);
}

#define CHECK(...) check((__VA_ARGS__), #__VA_ARGS__, __LINE__)

// ---------------------------------------------------------------------------
// Allocation and free tracking. Tests link with
// -Wl,--wrap=lang_alloc_v1 -Wl,--wrap=lang_free_v1.
struct AllocationRecord {
  void *pointer;
  uint64_t size;
  unsigned frees;
  bool active;
  bool used;
};

AllocationRecord g_records[8192];
unsigned g_record_count = 0;
uint64_t g_allocation_calls = 0;
uint64_t g_fail_at = 0;
void *g_last_free = nullptr;
bool g_double_free = false;
bool g_unknown_free = false;
bool g_poison_on_free = false;

void reset_tracking() {
  for (auto &record : g_records) record = {};
  g_record_count = 0;
  g_allocation_calls = 0;
  g_fail_at = 0;
  g_last_free = nullptr;
  g_double_free = false;
  g_unknown_free = false;
  g_poison_on_free = false;
}

AllocationRecord *active_record(void *pointer) {
  for (unsigned i = 0; i < g_record_count; ++i) {
    auto &record = g_records[i];
    if (record.used && record.active && record.pointer == pointer)
      return &record;
  }
  return nullptr;
}

AllocationRecord *any_record(void *pointer) {
  for (unsigned i = 0; i < g_record_count; ++i)
    if (g_records[i].used && g_records[i].pointer == pointer)
      return &g_records[i];
  return nullptr;
}

unsigned live_allocations() {
  unsigned live = 0;
  for (unsigned i = 0; i < g_record_count; ++i)
    if (g_records[i].used && g_records[i].active) ++live;
  return live;
}

unsigned free_count(void *pointer) {
  auto *record = any_record(pointer);
  return record ? record->frees : 0;
}

uint64_t allocation_size(void *pointer) {
  auto *record = any_record(pointer);
  return record ? record->size : 0;
}

} // namespace

extern "C" void *__real_lang_alloc_v1(uint64_t, uint64_t);
extern "C" void __real_lang_free_v1(void *);

extern "C" void *__wrap_lang_alloc_v1(uint64_t size, uint64_t align) {
  ++g_allocation_calls;
  if (g_fail_at && g_allocation_calls == g_fail_at) return nullptr;
  void *pointer = __real_lang_alloc_v1(size, align);
  if (!pointer) return nullptr;
  AllocationRecord *record = nullptr;
  for (unsigned i = 0; i < g_record_count; ++i) {
    if (g_records[i].used && g_records[i].pointer == pointer) {
      record = &g_records[i];
      break;
    }
  }
  if (!record && g_record_count < sizeof(g_records) / sizeof(g_records[0]))
    record = &g_records[g_record_count++];
  if (record) {
    record->pointer = pointer;
    record->size = size;
    record->frees = 0;
    record->active = true;
    record->used = true;
  }
  return pointer;
}

extern "C" void __wrap_lang_free_v1(void *pointer) {
  if (!pointer) return;
  g_last_free = pointer;
  auto *record = active_record(pointer);
  if (!record) {
    g_unknown_free = true;
  } else {
    ++record->frees;
    if (record->frees > 1) g_double_free = true;
    if (g_poison_on_free && record->size)
      std::memset(pointer, 0xA5, static_cast<size_t>(record->size));
    record->active = false;
  }
  __real_lang_free_v1(pointer);
}

namespace {

// ---------------------------------------------------------------------------
// Observer and element callbacks.
std::vector<int> g_drops;
int g_copy_calls = 0;
int g_inner_copy_calls = 0;
int g_inner_drop_calls = 0;

void int_copy(void *) { ++g_copy_calls; }
void int_drop(void *pointer) {
  g_drops.push_back(*static_cast<int *>(pointer));
}
void inner_copy(void *pointer) {
  ++g_inner_copy_calls;
  lang_vec_retain_v1(*static_cast<LangVec **>(pointer));
}
void inner_drop(void *pointer) {
  ++g_inner_drop_calls;
  lang_vec_release_v1(*static_cast<LangVec **>(pointer));
}

unsigned g_observed_element_drops = 0;
unsigned g_observed_final_frees = 0;
unsigned g_observed_indices[64] = {};
unsigned g_observed_index_count = 0;

void reset_observer() {
  g_observed_element_drops = 0;
  g_observed_final_frees = 0;
  g_observed_index_count = 0;
  for (auto &index : g_observed_indices) index = 0;
}

// ---------------------------------------------------------------------------
// Child-process helpers for terminal Panic checks.
int run_child(void (*function)(void *), void *argument, std::string &record) {
  int fds[2];
  if (pipe(fds) != 0) return -1;
  const pid_t pid = fork();
  if (pid == 0) {
    close(fds[0]);
    if (dup2(fds[1], 2) < 0) _exit(120);
    close(fds[1]);
    function(argument);
    _exit(0);
  }
  close(fds[1]);
  record.clear();
  char buffer[256];
  for (;;) {
    const ssize_t count = read(fds[0], buffer, sizeof(buffer));
    if (count > 0) {
      record.append(buffer, static_cast<size_t>(count));
      continue;
    }
    if (count < 0 && errno == EINTR) continue;
    break;
  }
  close(fds[0]);
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) return -1;
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return -1;
}

enum ActionKind {
  act_reserve = 0,
  act_push,
  act_pop,
  act_resize_grow,
  act_resize_shrink,
  act_clear,
  act_oom_reserve,
  act_overflow_reserve,
  act_extend
};

struct ChildAction {
  ActionKind kind;
  LangVec *vec;
  uint64_t minimum;
  uint64_t length;
  const void *value;
  const void *fill;
};

void child_perform(void *raw) {
  auto *action = static_cast<ChildAction *>(raw);
  switch (action->kind) {
  case act_reserve:
    lang_vec_reserve_v1(action->vec, action->minimum);
    break;
  case act_push:
    lang_vec_push_v1(action->vec, action->value);
    break;
  case act_pop: {
    int out = 0;
    lang_vec_pop_v1(action->vec, &out);
    break;
  }
  case act_resize_grow:
    lang_vec_resize_v1(action->vec, action->length, action->fill);
    break;
  case act_resize_shrink:
    lang_vec_resize_v1(action->vec, action->length, nullptr);
    break;
  case act_clear:
    lang_vec_clear_v1(action->vec);
    break;
  case act_oom_reserve: {
    auto *vec = lang_vec_new_v1(0, sizeof(int), nullptr, nullptr);
    g_fail_at = g_allocation_calls + 1;
    lang_vec_reserve_v1(vec, 4);
    break;
  }
  case act_overflow_reserve: {
    auto *vec = lang_vec_new_v1(1, sizeof(uint64_t), nullptr, nullptr);
    g_fail_at = 0;
    lang_vec_reserve_v1(vec, UINT64_MAX / 2);
    break;
  }
  case act_extend:
    (void)lang_vec_extend_trivial_v1(action->vec, action->vec);
    break;
  }
}

bool expect_panic(const ChildAction &action, const char *cause) {
  std::string record;
  const int code = run_child(child_perform, const_cast<ChildAction *>(&action),
                             record);
  if (code != 101) {
    std::fprintf(stderr, "panic exit=%d expected=101 cause=%s\n", code, cause);
    return false;
  }
  if (record.find("LANG_NATIVE_PANIC:v1:") == std::string::npos ||
      record.find(cause) == std::string::npos) {
    std::fprintf(stderr, "panic record mismatch: %s", record.c_str());
    return false;
  }
  return true;
}

LangVec *seed_shared_vec(void *region) {
  std::memset(region, 0, kVecHeaderBytes + 4 * sizeof(int));
  auto *vec = static_cast<LangVec *>(region);
  vec->refs = 1;
  vec->length = 3;
  vec->initialized = 3;
  vec->stride = sizeof(int);
  vec->iterations = 1;
  vec->data = static_cast<unsigned char *>(region) + kVecHeaderBytes;
  vec->copy = nullptr;
  vec->drop = nullptr;
  vec->capacity = 4;
  auto *values = static_cast<int *>(vec->data);
  values[0] = 11;
  values[1] = 22;
  values[2] = 33;
  values[3] = 44;
  return vec;
}

// Verify that every length/storage mutation is rejected before a byte of the
// shared descriptor or payload changes.
bool expect_iteration_panic_without_mutation(ActionKind kind,
                                             const char *cause) {
  constexpr size_t kRegionBytes = kVecHeaderBytes + 4 * sizeof(int);
  void *region = mmap(nullptr, kRegionBytes, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (region == MAP_FAILED) return false;
  LangVec *vec = seed_shared_vec(region);
  std::vector<unsigned char> before(
      static_cast<unsigned char *>(region),
      static_cast<unsigned char *>(region) + kRegionBytes);
  auto *values = static_cast<int *>(vec->data);

  ChildAction action{};
  action.kind = kind;
  action.vec = vec;
  action.minimum = 5;
  action.length = kind == act_resize_shrink ? 2 : 4;
  action.value = values + 1;
  action.fill = values;
  std::string record;
  const int code = run_child(child_perform, &action, record);
  const bool unchanged =
      std::memcmp(region, before.data(), kRegionBytes) == 0;
  const bool panicked =
      code == 101 && record.find("LANG_NATIVE_PANIC:v1:") != std::string::npos &&
      record.find(cause) != std::string::npos;
  if (!panicked || !unchanged) {
    std::fprintf(stderr,
                 "iteration guard kind=%d exit=%d unchanged=%d record=%s",
                 static_cast<int>(kind), code, unchanged ? 1 : 0,
                 record.c_str());
  }
  munmap(region, kRegionBytes);
  return panicked && unchanged;
}

// ---------------------------------------------------------------------------
void test_layout_and_new_capacity() {
  reset_tracking();
  LangVec *vec = lang_vec_new_v1(3, sizeof(int), nullptr, nullptr);
  CHECK(vec != nullptr);
  CHECK(vec->capacity == 3);
  CHECK(vec->length == 3);
  CHECK(vec->initialized == 0);
  CHECK(vec->data == reinterpret_cast<unsigned char *>(vec) + kVecHeaderBytes);
  CHECK(reinterpret_cast<uintptr_t>(vec->data) % 16 == 0);
  CHECK(live_allocations() == 1);
  const uint64_t allocations = g_allocation_calls;
  lang_vec_reserve_v1(vec, 3);
  CHECK(g_allocation_calls == allocations);
  CHECK(vec->capacity == 3);
  CHECK(vec->data == reinterpret_cast<unsigned char *>(vec) + kVecHeaderBytes);
  for (int value : {4, 5, 6}) lang_vec_init_v1(vec, &value);
  CHECK(*static_cast<int *>(lang_vec_at_v1(vec, 2)) == 6);
  lang_vec_release_v1(vec);
  CHECK(free_count(vec) == 1);
  CHECK(live_allocations() == 0);
}

void test_repeated_geometric_growth_and_aliases() {
  reset_tracking();
  LangVec *vec = lang_vec_new_v1(0, sizeof(int), nullptr, nullptr);
  CHECK(vec != nullptr);
  CHECK(vec->capacity == 0);
  CHECK(vec->data == nullptr);
  lang_vec_retain_v1(vec);
  LangVec *alias = vec;
  uint64_t previous = 0;
  for (int i = 1; i <= 33; ++i) {
    const int value = i * 3;
    lang_vec_push_v1(vec, &value);
    CHECK(vec->length == static_cast<uint64_t>(i));
    CHECK(vec->initialized == static_cast<uint64_t>(i));
    CHECK(alias->length == vec->length);
    CHECK(alias->capacity == vec->capacity);
    CHECK(alias->data == vec->data);
    CHECK(vec->capacity >= static_cast<uint64_t>(i));
    if (vec->capacity != previous) {
      CHECK(previous == 0 ? vec->capacity == 1
                          : vec->capacity == previous * 2);
      previous = vec->capacity;
    }
    for (int j = 0; j < i; ++j)
      CHECK(*static_cast<int *>(lang_vec_at_v1(vec, j)) == (j + 1) * 3);
  }
  lang_vec_release_v1(alias);
  CHECK(vec->refs == 1);
  lang_vec_release_v1(vec);
  CHECK(live_allocations() == 0);
  CHECK(!g_double_free && !g_unknown_free);
}

void test_reserve_noop_growth_and_single_free() {
  reset_tracking();
  g_drops.clear();
  g_copy_calls = 0;
  LangVec *vec = lang_vec_new_v1(2, sizeof(int), int_copy, int_drop);
  CHECK(vec != nullptr);
  for (int value : {7, 8}) lang_vec_init_v1(vec, &value);
  void *descriptor = vec;
  void *inline_data = vec->data;
  const uint64_t allocations = g_allocation_calls;

  lang_vec_reserve_v1(vec, 4);
  CHECK(vec->capacity == 4);
  CHECK(vec->data != inline_data);
  CHECK(reinterpret_cast<uintptr_t>(vec->data) % 16 == 0);
  CHECK(vec->length == 2);
  CHECK(vec->initialized == 2);
  CHECK(*static_cast<int *>(lang_vec_at_v1(vec, 0)) == 7);
  CHECK(*static_cast<int *>(lang_vec_at_v1(vec, 1)) == 8);
  CHECK(g_copy_calls == 0);
  CHECK(g_drops.empty());
  CHECK(g_allocation_calls == allocations + 1);
  void *separate_one = vec->data;
  CHECK(allocation_size(separate_one) == 4 * sizeof(int));
  CHECK(free_count(separate_one) == 0);

  const uint64_t after_growth = g_allocation_calls;
  lang_vec_reserve_v1(vec, 4);
  lang_vec_reserve_v1(vec, 3);
  CHECK(g_allocation_calls == after_growth);
  CHECK(vec->capacity == 4);
  CHECK(vec->data == separate_one);

  lang_vec_reserve_v1(vec, 9);
  CHECK(vec->capacity >= 9);
  void *separate_two = vec->data;
  CHECK(separate_two != separate_one);
  CHECK(free_count(separate_one) == 1);
  CHECK(allocation_size(separate_two) == vec->capacity * sizeof(int));
  CHECK(*static_cast<int *>(lang_vec_at_v1(vec, 0)) == 7);
  CHECK(*static_cast<int *>(lang_vec_at_v1(vec, 1)) == 8);
  CHECK(g_copy_calls == 0);
  CHECK(g_drops.empty());
  CHECK(free_count(descriptor) == 0);
  CHECK(live_allocations() == 2);

  lang_vec_release_v1(vec);
  CHECK(free_count(separate_two) == 1);
  CHECK(free_count(descriptor) == 1);
  CHECK(g_last_free == descriptor);
  CHECK(g_drops == std::vector<int>{8, 7});
  CHECK(live_allocations() == 0);
  CHECK(!g_double_free && !g_unknown_free);
}

void test_partial_prefix_reserve() {
  reset_tracking();
  g_drops.clear();
  g_copy_calls = 0;
  LangVec *vec = lang_vec_new_v1(4, sizeof(int), int_copy, int_drop);
  for (int value : {7, 8}) lang_vec_init_v1(vec, &value);
  lang_vec_reserve_v1(vec, 8);
  CHECK(vec->capacity == 8);
  CHECK(vec->length == 4);
  CHECK(vec->initialized == 2);
  CHECK(g_copy_calls == 0);
  CHECK(g_drops.empty());
  CHECK(*static_cast<int *>(lang_vec_at_v1(vec, 0)) == 7);
  CHECK(*static_cast<int *>(lang_vec_at_v1(vec, 1)) == 8);
  lang_vec_release_v1(vec);
  CHECK(g_drops == std::vector<int>{8, 7});
  CHECK(live_allocations() == 0);
}

void test_pop_clear_resize() {
  reset_tracking();
  g_drops.clear();
  g_copy_calls = 0;
  LangVec *vec = lang_vec_new_v1(0, sizeof(int), int_copy, int_drop);
  for (int value : {1, 2, 3, 4, 5}) lang_vec_push_v1(vec, &value);
  const uint64_t capacity = vec->capacity;
  void *data = vec->data;
  CHECK(capacity >= 5);
  CHECK(vec->initialized == 5);

  int out = -1;
  CHECK(lang_vec_pop_v1(vec, &out));
  CHECK(out == 5);
  CHECK(vec->length == 4);
  CHECK(vec->initialized == 4);
  CHECK(vec->capacity == capacity);
  CHECK(vec->data == data);
  CHECK(g_drops.empty()); // transfer, not drop

  lang_vec_clear_v1(vec);
  CHECK(vec->length == 0);
  CHECK(vec->initialized == 0);
  CHECK(vec->capacity == capacity);
  CHECK(vec->data == data);
  CHECK(g_drops == std::vector<int>{4, 3, 2, 1});
  g_drops.clear();

  int unchanged = 123;
  CHECK(!lang_vec_pop_v1(vec, &unchanged));
  CHECK(unchanged == 123);
  CHECK(vec->length == 0);
  CHECK(g_drops.empty());

  int nine = 9;
  lang_vec_push_v1(vec, &nine);
  int fill = 7;
  lang_vec_resize_v1(vec, 4, &fill);
  CHECK(vec->length == 4);
  CHECK(vec->initialized == 4);
  CHECK(g_drops.empty());
  CHECK(*static_cast<int *>(lang_vec_at_v1(vec, 0)) == 9);
  for (uint64_t i = 1; i < 4; ++i)
    CHECK(*static_cast<int *>(lang_vec_at_v1(vec, i)) == 7);

  lang_vec_resize_v1(vec, 2, nullptr); // fill must not be read while shrinking
  CHECK(vec->length == 2);
  CHECK(vec->initialized == 2);
  CHECK(g_drops == std::vector<int>{7, 7});
  g_drops.clear();
  CHECK(*static_cast<int *>(lang_vec_at_v1(vec, 0)) == 9);
  CHECK(*static_cast<int *>(lang_vec_at_v1(vec, 1)) == 7);

  lang_vec_clear_v1(vec);
  CHECK(vec->length == 0);
  CHECK(vec->initialized == 0);
  CHECK(vec->capacity == capacity);
  CHECK(g_drops == std::vector<int>{7, 9});
  g_drops.clear();

  lang_vec_release_v1(vec);
  CHECK(g_drops.empty());
  CHECK(live_allocations() == 0);
}

void test_owning_nested_vec_growth_and_pop() {
  reset_tracking();
  g_inner_copy_calls = 0;
  g_inner_drop_calls = 0;
  LangVec *outer =
      lang_vec_new_v1(0, sizeof(LangVec *), inner_copy, inner_drop);
  constexpr int kCount = 32;
  LangVec *inners[kCount] = {};
  for (int i = 0; i < kCount; ++i) {
    LangVec *inner = lang_vec_new_v1(1, sizeof(int), nullptr, nullptr);
    const int value = i * 10;
    lang_vec_init_v1(inner, &value);
    inners[i] = inner;
    lang_vec_push_v1(outer, &inner); // ownership transfer, no element copy
    CHECK(inner->refs == 1);
    CHECK(outer->length == static_cast<uint64_t>(i + 1));
  }
  CHECK(g_inner_copy_calls == 0);
  CHECK(g_inner_drop_calls == 0);
  for (int i = 0; i < kCount; ++i) {
    auto *stored = *static_cast<LangVec **>(lang_vec_at_v1(outer, i));
    CHECK(stored == inners[i]);
    CHECK(stored->refs == 1);
    CHECK(*static_cast<int *>(lang_vec_at_v1(stored, 0)) == i * 10);
  }

  LangVec *popped = nullptr;
  CHECK(lang_vec_pop_v1(outer, &popped));
  CHECK(popped == inners[kCount - 1]);
  CHECK(popped->refs == 1);
  CHECK(g_inner_drop_calls == 0); // transfer, not drop
  CHECK(outer->length == kCount - 1);
  lang_vec_release_v1(popped);

  lang_vec_release_v1(outer);
  CHECK(g_inner_drop_calls == kCount - 1);
  CHECK(live_allocations() == 0);
  CHECK(!g_double_free && !g_unknown_free);
}

void test_resize_fill_retains_and_shrink_releases() {
  reset_tracking();
  g_inner_copy_calls = 0;
  g_inner_drop_calls = 0;
  LangVec *fill = lang_vec_new_v1(1, sizeof(int), nullptr, nullptr);
  const int value = 77;
  lang_vec_init_v1(fill, &value);
  LangVec *outer =
      lang_vec_new_v1(0, sizeof(LangVec *), inner_copy, inner_drop);

  lang_vec_resize_v1(outer, 3, &fill);
  CHECK(outer->length == 3);
  CHECK(outer->initialized == 3);
  CHECK(g_inner_copy_calls == 3);
  CHECK(fill->refs == 4);
  for (uint64_t i = 0; i < 3; ++i)
    CHECK(*static_cast<LangVec **>(lang_vec_at_v1(outer, i)) == fill);

  lang_vec_resize_v1(outer, 1, nullptr);
  CHECK(outer->length == 1);
  CHECK(outer->initialized == 1);
  CHECK(g_inner_drop_calls == 2);
  CHECK(fill->refs == 2);

  lang_vec_clear_v1(outer);
  CHECK(outer->length == 0);
  CHECK(outer->initialized == 0);
  CHECK(g_inner_drop_calls == 3);
  CHECK(fill->refs == 1);
  CHECK(outer->capacity >= 3);

  lang_vec_release_v1(fill);
  lang_vec_release_v1(outer);
  CHECK(g_inner_drop_calls == 3);
  CHECK(live_allocations() == 0);
}

void test_self_alias_push_and_fill_at_growth() {
  reset_tracking();
  g_poison_on_free = true;
  LangVec *vec = lang_vec_new_v1(0, sizeof(int), nullptr, nullptr);
  for (int value : {10, 20, 30, 40, 50, 60, 70, 80})
    lang_vec_push_v1(vec, &value);
  CHECK(vec->capacity == 8);
  CHECK(vec->length == 8);
  const int source = *static_cast<int *>(lang_vec_at_v1(vec, 0));
  const void *self = lang_vec_at_v1(vec, 0);
  lang_vec_push_v1(vec, self); // growth frees the old separate buffer
  CHECK(vec->length == 9);
  for (int i = 0; i < 8; ++i)
    CHECK(*static_cast<int *>(lang_vec_at_v1(vec, i)) == (i + 1) * 10);
  CHECK(*static_cast<int *>(lang_vec_at_v1(vec, 8)) == source);

  const void *fill = lang_vec_at_v1(vec, 0);
  CHECK(vec->capacity == 16);
  lang_vec_resize_v1(vec, 17, fill); // growth frees the old separate buffer
  CHECK(vec->length == 17);
  CHECK(vec->capacity >= 17);
  CHECK(*static_cast<int *>(lang_vec_at_v1(vec, 0)) == source);
  for (uint64_t i = 9; i < 17; ++i)
    CHECK(*static_cast<int *>(lang_vec_at_v1(vec, i)) == source);
  lang_vec_release_v1(vec);
  CHECK(live_allocations() == 0);
  g_poison_on_free = false;

  reset_tracking();
  g_poison_on_free = true;
  LangVec *inline_vec = lang_vec_new_v1(2, sizeof(int), nullptr, nullptr);
  for (int value : {5, 6}) lang_vec_init_v1(inline_vec, &value);
  const void *inline_self = lang_vec_at_v1(inline_vec, 1);
  lang_vec_push_v1(inline_vec, inline_self);
  CHECK(inline_vec->length == 3);
  CHECK(*static_cast<int *>(lang_vec_at_v1(inline_vec, 0)) == 5);
  CHECK(*static_cast<int *>(lang_vec_at_v1(inline_vec, 1)) == 6);
  CHECK(*static_cast<int *>(lang_vec_at_v1(inline_vec, 2)) == 6);
  lang_vec_release_v1(inline_vec);
  CHECK(live_allocations() == 0);
  g_poison_on_free = false;
}

void test_noop_reserve_during_iteration() {
  reset_tracking();
  LangVec *vec = lang_vec_new_v1(3, sizeof(int), nullptr, nullptr);
  for (int value : {1, 2, 3}) lang_vec_init_v1(vec, &value);
  lang_vec_begin_v1(vec);
  const uint64_t length = vec->length;
  const uint64_t initialized = vec->initialized;
  const uint64_t capacity = vec->capacity;
  void *data = vec->data;
  lang_vec_reserve_v1(vec, capacity); // no-op is not a structural change
  CHECK(vec->length == length);
  CHECK(vec->initialized == initialized);
  CHECK(vec->capacity == capacity);
  CHECK(vec->data == data);
  lang_vec_end_v1(vec);
  lang_vec_release_v1(vec);
  CHECK(live_allocations() == 0);
}

void test_observer_release_order_preserved() {
  reset_tracking();
  reset_observer();
  LangVec *vec = lang_vec_new_v1(3, sizeof(int), nullptr, nullptr);
  for (int value : {1, 2, 3}) lang_vec_init_v1(vec, &value);
  lang_vec_release_v1(vec);
  CHECK(g_observed_element_drops == 3);
  CHECK(g_observed_final_frees == 1);
  CHECK(g_observed_index_count == 3);
  CHECK(g_observed_indices[0] == 2);
  CHECK(g_observed_indices[1] == 1);
  CHECK(g_observed_indices[2] == 0);
  CHECK(live_allocations() == 0);
}

void test_overlapping_trivial_set() {
  for (uint64_t stride : {1, 2, 3, 4, 8, 16}) {
    reset_tracking();
    unsigned char initial[16];
    for (uint64_t i = 0; i < stride; ++i)
      initial[i] = static_cast<unsigned char>(i + 1);
    LangVec *vec = lang_vec_new_v1(3, stride, nullptr, nullptr);
    for (int i = 0; i < 3; ++i) lang_vec_init_v1(vec, initial);
    auto *middle = static_cast<unsigned char *>(lang_vec_at_v1(vec, 1));
    for (int offset : {-1, 1}) {
      unsigned char expected[16];
      std::memcpy(expected, middle + offset, stride);
      lang_vec_set_v1(vec, 1, middle + offset);
      CHECK(std::memcmp(middle, expected, stride) == 0);
    }
    lang_vec_release_v1(vec);
    CHECK(live_allocations() == 0);
  }
}

void test_in_capacity_push_overlap() {
  // An in-capacity push also copies a source whose range overlaps the append
  // slot. The bytes after the initialized prefix are unspecified, so the
  // expectation is the snapshot taken before the push, not a formula.
  for (uint64_t stride : {2, 4, 8, 16}) {
    reset_tracking();
    unsigned char initial[16];
    for (uint64_t i = 0; i < stride; ++i)
      initial[i] = static_cast<unsigned char>(0x40 + i);
    LangVec *vec = lang_vec_new_v1(1, stride, nullptr, nullptr);
    lang_vec_init_v1(vec, initial);
    lang_vec_reserve_v1(vec, 4); // spare capacity: no growth during the push
    CHECK(vec->length == 1);
    CHECK(vec->capacity == 4);
    auto *payload = static_cast<unsigned char *>(vec->data);
    auto *target = payload + stride;
    auto *source = target - stride / 2;
    unsigned char expected[16];
    std::memcpy(expected, source, stride);
    const uint64_t allocations = g_allocation_calls;
    lang_vec_push_v1(vec, source);
    CHECK(vec->length == 2);
    CHECK(vec->initialized == 2);
    CHECK(vec->capacity == 4);
    CHECK(g_allocation_calls == allocations); // within capacity: no growth
    CHECK(std::memcmp(target, expected, stride) == 0);
    CHECK(std::memcmp(payload, initial, stride) == 0);
    lang_vec_release_v1(vec);
    CHECK(live_allocations() == 0);
  }
}

uint64_t expected_capacity(uint64_t current, uint64_t minimum) {
  uint64_t capacity = current ? current : 1;
  while (capacity < minimum) capacity *= 2;
  return capacity;
}

unsigned char *raw_element(LangVec *vec, uint64_t index) {
  return static_cast<unsigned char *>(vec->data) + index * vec->stride;
}

void test_resize_fill_crosses_inline_prefix() {
  // A fill pointer can start in the descriptor bytes and cross into the first
  // destination element. The full-range overlap test must select the exact
  // per-element loop; the seeded/doubling path would not re-read the bytes an
  // earlier iteration rewrote. The Vec is shrunk to zero first so the stale
  // payload capacity stays inline and the source is entirely in the allocation.
  for (uint64_t stride : {2ULL, 3ULL, 4ULL, 5ULL, 8ULL, 16ULL}) {
    for (uint64_t lead : {uint64_t{1}, stride - 1}) {
      reset_tracking();
      LangVec *vec = lang_vec_new_v1(4, stride, nullptr, nullptr);
      unsigned char pattern[16];
      for (uint64_t b = 0; b < stride; ++b)
        pattern[b] = static_cast<unsigned char>(0x60 + b * 5);
      for (uint64_t i = 0; i < 4; ++i) lang_vec_init_v1(vec, pattern);
      lang_vec_resize_v1(vec, 0, nullptr);
      CHECK(vec->length == 0 && vec->initialized == 0);
      CHECK(vec->capacity == 4);
      auto *payload = static_cast<unsigned char *>(vec->data);
      CHECK(payload == reinterpret_cast<unsigned char *>(vec) + kVecHeaderBytes);
      for (uint64_t b = 0; b < 4 * stride; ++b)
        payload[b] = static_cast<unsigned char>(0x31 + b);

      std::vector<unsigned char> model(lead + 4 * stride);
      std::memcpy(model.data(), payload - lead, lead);
      std::memcpy(model.data() + lead, payload, 4 * stride);
      for (uint64_t index = 0; index < 4; ++index)
        std::memmove(model.data() + lead + index * stride, model.data(),
                     stride);

      const uint64_t allocations = g_allocation_calls;
      lang_vec_resize_v1(vec, 4, payload - lead);
      CHECK(g_allocation_calls == allocations);
      CHECK(vec->length == 4 && vec->initialized == 4);
      CHECK(vec->capacity == 4);
      CHECK(std::memcmp(payload, model.data() + lead, 4 * stride) == 0);
      lang_vec_release_v1(vec);
      CHECK(live_allocations() == 0);
    }
  }
}

void test_resize_grow_trivial_fill() {
  // Trivial resize growth must reproduce the per-element byte loop for every
  // stride/length, both in capacity and with a grow-and-snapshot step.
  for (uint64_t stride : {1ULL, 2ULL, 3ULL, 4ULL, 7ULL, 8ULL, 16ULL, 64ULL}) {
    unsigned char pattern[64];
    for (uint64_t i = 0; i < stride; ++i)
      pattern[i] = static_cast<unsigned char>(0x30 + i * 7);
    for (uint64_t initial : {0ULL, 1ULL, 2ULL, 3ULL}) {
      for (uint64_t target : {1ULL, 2ULL, 3ULL, 5ULL, 9ULL, 17ULL}) {
        if (target <= initial) continue;
        for (uint64_t extra : {0ULL, 8ULL}) {
          reset_tracking();
          LangVec *vec = lang_vec_new_v1(initial, stride, nullptr, nullptr);
          unsigned char prefix[3 * 64];
          for (uint64_t i = 0; i < initial; ++i) {
            for (uint64_t b = 0; b < stride; ++b) prefix[i * stride + b] =
                static_cast<unsigned char>(i * 13 + b);
            lang_vec_init_v1(vec, prefix + i * stride);
          }
          uint64_t capacity = initial;
          if (extra) {
            lang_vec_reserve_v1(vec, initial + extra);
            capacity = expected_capacity(capacity, initial + extra);
          }
          CHECK(vec->capacity == capacity);
          void *data = vec->data;
          const uint64_t before_capacity = capacity;
          lang_vec_resize_v1(vec, target, pattern);
          CHECK(vec->length == target);
          CHECK(vec->initialized == target);
          if (target > capacity)
            capacity = expected_capacity(capacity, target);
          CHECK(vec->capacity == capacity);
          if (target <= before_capacity) CHECK(vec->data == data);
          for (uint64_t i = 0; i < initial; ++i)
            CHECK(std::memcmp(raw_element(vec, i), prefix + i * stride,
                              stride) == 0);
          for (uint64_t i = initial; i < target; ++i)
            CHECK(std::memcmp(raw_element(vec, i), pattern, stride) == 0);
          lang_vec_release_v1(vec);
          CHECK(live_allocations() == 0);
        }
      }
    }
  }
}

void test_resize_grow_trivial_alias_edges() {
  // In-capacity fill from the initialized prefix: the seeded/doubling path
  // repeats the snapshot bytes and leaves the source element untouched.
  for (uint64_t stride : {1ULL, 3ULL, 8ULL, 16ULL}) {
    reset_tracking();
    LangVec *vec = lang_vec_new_v1(3, stride, nullptr, nullptr);
    unsigned char values[3 * 16];
    for (uint64_t i = 0; i < 3 * stride; ++i)
      values[i] = static_cast<unsigned char>(0x51 + i);
    for (uint64_t i = 0; i < 3; ++i)
      lang_vec_init_v1(vec, values + i * stride);
    lang_vec_reserve_v1(vec, 8);
    CHECK(vec->capacity == expected_capacity(3, 8));
    void *data = vec->data;
    unsigned char fill[16];
    std::memcpy(fill, raw_element(vec, 1), stride);
    lang_vec_resize_v1(vec, 6, raw_element(vec, 1));
    CHECK(vec->capacity == expected_capacity(3, 8));
    CHECK(vec->data == data);
    for (uint64_t i = 0; i < 3; ++i)
      CHECK(std::memcmp(raw_element(vec, i), values + i * stride, stride) == 0);
    for (uint64_t i = 3; i < 6; ++i)
      CHECK(std::memcmp(raw_element(vec, i), fill, stride) == 0);
    lang_vec_release_v1(vec);
    CHECK(live_allocations() == 0);
  }

  // A fill that overlaps its destination, including an unaligned odd offset
  // in spare capacity, keeps exact per-element sequential semantics.
  for (uint64_t stride : {2ULL, 3ULL, 5ULL, 8ULL, 16ULL}) {
    reset_tracking();
    LangVec *vec = lang_vec_new_v1(2, stride, nullptr, nullptr);
    for (uint64_t i = 0; i < 2; ++i) {
      unsigned char value[16];
      for (uint64_t b = 0; b < stride; ++b)
        value[b] = static_cast<unsigned char>(0x20 + i * 9 + b);
      lang_vec_init_v1(vec, value);
    }
    lang_vec_reserve_v1(vec, 6);
    const uint64_t capacity = expected_capacity(2, 6);
    CHECK(vec->capacity == capacity);
    std::vector<unsigned char> model(capacity * stride);
    std::memcpy(model.data(), vec->data, 2 * stride);
    unsigned char *source = raw_element(vec, 3) + 1;
    for (uint64_t b = 0; b < stride; ++b)
      source[b] = static_cast<unsigned char>(0x90 + b * 3);
    std::memcpy(model.data() + 3 * stride + 1, source, stride);
    for (uint64_t index = 2; index < 6; ++index)
      std::memmove(model.data() + index * stride,
                   model.data() + 3 * stride + 1, stride);
    const uint64_t allocations = g_allocation_calls;
    lang_vec_resize_v1(vec, 6, source);
    CHECK(g_allocation_calls == allocations);
    CHECK(vec->capacity == capacity);
    CHECK(std::memcmp(vec->data, model.data(), 6 * stride) == 0);
    lang_vec_release_v1(vec);
    CHECK(live_allocations() == 0);
  }

  // Growing with a fill inside the old buffer snapshots before the realloc.
  reset_tracking();
  g_poison_on_free = true;
  LangVec *vec = lang_vec_new_v1(4, sizeof(int), nullptr, nullptr);
  for (int i = 0; i < 4; ++i) lang_vec_init_v1(vec, &i);
  const int saved = *static_cast<int *>(lang_vec_at_v1(vec, 1));
  void *old_data = vec->data;
  lang_vec_resize_v1(vec, 9, lang_vec_at_v1(vec, 1));
  CHECK(vec->data != old_data);
  CHECK(vec->capacity >= 9);
  CHECK(*static_cast<int *>(lang_vec_at_v1(vec, 0)) == 0);
  for (uint64_t i = 4; i < 9; ++i)
    CHECK(*static_cast<int *>(lang_vec_at_v1(vec, i)) == saved);
  lang_vec_release_v1(vec);
  CHECK(live_allocations() == 0);
  g_poison_on_free = false;

  // Length 0 and 1 edges, including cleared storage that keeps capacity.
  reset_tracking();
  unsigned char pattern[4] = {0xA1, 0xB2, 0xC3, 0xD4};
  LangVec *empty = lang_vec_new_v1(0, 4, nullptr, nullptr);
  lang_vec_resize_v1(empty, 1, pattern);
  CHECK(empty->length == 1 && empty->initialized == 1);
  CHECK(std::memcmp(raw_element(empty, 0), pattern, 4) == 0);
  lang_vec_resize_v1(empty, 2, pattern);
  CHECK(empty->length == 2 && empty->initialized == 2);
  CHECK(std::memcmp(raw_element(empty, 1), pattern, 4) == 0);
  lang_vec_clear_v1(empty);
  const uint64_t kept = empty->capacity;
  lang_vec_resize_v1(empty, 2, pattern);
  CHECK(empty->capacity == kept);
  CHECK(std::memcmp(raw_element(empty, 0), pattern, 4) == 0);
  CHECK(std::memcmp(raw_element(empty, 1), pattern, 4) == 0);
  lang_vec_release_v1(empty);
  CHECK(live_allocations() == 0);
}

void test_terminal_panics() {
  {
    ChildAction action{};
    action.kind = act_oom_reserve;
    CHECK(expect_panic(action, "allocation-failed"));
  }
  {
    ChildAction action{};
    action.kind = act_overflow_reserve;
    CHECK(expect_panic(action, "allocation-size-overflow"));
  }

  CHECK(expect_iteration_panic_without_mutation(act_reserve,
                                                "active-iteration"));
  CHECK(expect_iteration_panic_without_mutation(act_push,
                                                "active-iteration"));
  CHECK(expect_iteration_panic_without_mutation(act_pop,
                                                "active-iteration"));
  CHECK(expect_iteration_panic_without_mutation(act_resize_grow,
                                                "active-iteration"));
  CHECK(expect_iteration_panic_without_mutation(act_resize_shrink,
                                                "active-iteration"));
  CHECK(expect_iteration_panic_without_mutation(act_clear,
                                                "active-iteration"));
  CHECK(expect_iteration_panic_without_mutation(act_extend,
                                                "active-iteration"));
}

} // namespace

extern "C" void lang_vec_observe_v1(uint32_t event, const void *, uint64_t detail) {
  if (event == 1) return;
  if (event == 4) {
    ++g_observed_element_drops;
    if (g_observed_index_count <
        sizeof(g_observed_indices) / sizeof(g_observed_indices[0]))
      g_observed_indices[g_observed_index_count++] =
          static_cast<unsigned>(detail);
  }
  if (event == 5) ++g_observed_final_frees;
}

int main() {
  test_layout_and_new_capacity();
  test_repeated_geometric_growth_and_aliases();
  test_reserve_noop_growth_and_single_free();
  test_partial_prefix_reserve();
  test_pop_clear_resize();
  test_owning_nested_vec_growth_and_pop();
  test_resize_fill_retains_and_shrink_releases();
  test_self_alias_push_and_fill_at_growth();
  test_noop_reserve_during_iteration();
  test_observer_release_order_preserved();
  test_overlapping_trivial_set();
  test_in_capacity_push_overlap();
  test_resize_grow_trivial_fill();
  test_resize_grow_trivial_alias_edges();
  test_resize_fill_crosses_inline_prefix();
  test_terminal_panics();
  if (g_failures) {
    std::fprintf(stderr, "vec-growth: failed (%d checks)\n", g_failures);
    return 1;
  }
  std::puts("vec-growth-ok");
  return 0;
}
