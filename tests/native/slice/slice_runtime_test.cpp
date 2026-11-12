// Hosted regression harness for the private borrowed-Slice runtime ABI.
//
// The runtime objects are compiled at O0 and O2 with freestanding flags. This
// harness itself uses the host C++ library, POSIX fork/pipe for terminal Panic
// checks, and std::thread for the caller-locked aliasing stress. Only the
// runtime's lang_alloc_v1/lang_free_v1 calls are wrapped and tracked.
#include "../../../runtime/native/slice.hpp"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

static_assert(sizeof(LangVec) == 72, "LangVec ABI must stay 72 bytes");
static_assert(sizeof(LangSlice) == 24, "LangSlice ABI must stay 24 bytes");
static_assert(alignof(LangSlice) == 8, "LangSlice alignment changed");
static_assert(offsetof(LangSlice, owner) == 0, "LangSlice owner offset changed");
static_assert(offsetof(LangSlice, start) == 8, "LangSlice start offset changed");
static_assert(offsetof(LangSlice, length) == 16, "LangSlice length offset changed");
static_assert(offsetof(LangVec, length) == 8, "LangVec length offset changed");
static_assert(offsetof(LangVec, initialized) == 16,
              "LangVec initialized offset changed");
static_assert(offsetof(LangVec, iterations) == 32,
              "LangVec iterations offset changed");

namespace {

int g_failures = 0;

void check(bool condition, const char *text, int line) {
  if (condition) return;
  ++g_failures;
  std::fprintf(stderr, "FAIL:%d:%s\n", line, text);
}

#define CHECK(...) check((__VA_ARGS__), #__VA_ARGS__, __LINE__)

// ---------------------------------------------------------------------------
// Allocation tracking. The test is linked with
// -Wl,--wrap=lang_alloc_v1 -Wl,--wrap=lang_free_v1.
struct AllocationRecord {
  void *pointer;
  uint64_t size;
  unsigned frees;
  bool active;
  bool used;
};

constexpr unsigned kMaxRecords = 16384;
AllocationRecord g_records[kMaxRecords];
unsigned g_record_count = 0;
uint64_t g_allocation_calls = 0;
bool g_double_free = false;
bool g_unknown_free = false;

AllocationRecord *active_record(void *pointer) {
  for (unsigned i = 0; i < g_record_count; ++i) {
    auto &record = g_records[i];
    if (record.used && record.active && record.pointer == pointer)
      return &record;
  }
  return nullptr;
}

unsigned live_allocations() {
  unsigned live = 0;
  for (unsigned i = 0; i < g_record_count; ++i)
    if (g_records[i].used && g_records[i].active) ++live;
  return live;
}

void check_allocation_health() {
  CHECK(live_allocations() == 0);
  CHECK(!g_double_free);
  CHECK(!g_unknown_free);
}

extern "C" void *__real_lang_alloc_v1(uint64_t, uint64_t);
extern "C" void __real_lang_free_v1(void *);

extern "C" void *__wrap_lang_alloc_v1(uint64_t size, uint64_t align) {
  ++g_allocation_calls;
  void *pointer = __real_lang_alloc_v1(size, align);
  if (!pointer) return nullptr;
  AllocationRecord *record = nullptr;
  for (unsigned i = 0; i < g_record_count; ++i) {
    if (g_records[i].used && g_records[i].pointer == pointer) {
      record = &g_records[i];
      break;
    }
  }
  if (!record && g_record_count < kMaxRecords)
    record = &g_records[g_record_count++];
  if (!record) {
    std::fprintf(stderr, "FAIL: allocation tracking table exhausted\n");
    ++g_failures;
    return pointer;
  }
  record->pointer = pointer;
  record->size = size;
  record->frees = 0;
  record->active = true;
  record->used = true;
  return pointer;
}

extern "C" void __wrap_lang_free_v1(void *pointer) {
  if (!pointer) return;
  AllocationRecord *record = active_record(pointer);
  if (!record) {
    g_unknown_free = true;
  } else {
    ++record->frees;
    if (record->frees > 1) g_double_free = true;
    record->active = false;
  }
  __real_lang_free_v1(pointer);
}

// ---------------------------------------------------------------------------
// Element callbacks.
constexpr unsigned kTrackedValues = 32;
std::atomic<unsigned> g_int_drops{0};
std::atomic<unsigned> g_bad_drop{0};
std::atomic<unsigned> g_element_copies{0};
std::atomic<unsigned> g_drop_counts[kTrackedValues] = {};

void reset_element_counters() {
  g_int_drops.store(0, std::memory_order_relaxed);
  g_bad_drop.store(0, std::memory_order_relaxed);
  g_element_copies.store(0, std::memory_order_relaxed);
  for (auto &count : g_drop_counts) count.store(0, std::memory_order_relaxed);
}

void tracked_drop(void *pointer) {
  const int value = *static_cast<int *>(pointer);
  g_int_drops.fetch_add(1, std::memory_order_relaxed);
  if (value >= 0 && static_cast<unsigned>(value) < kTrackedValues)
    g_drop_counts[value].fetch_add(1, std::memory_order_relaxed);
  else
    g_bad_drop.fetch_add(1, std::memory_order_relaxed);
}

void tracked_copy(void *) {
  g_element_copies.fetch_add(1, std::memory_order_relaxed);
}

std::atomic<uint64_t> g_thread_drops{0};
std::atomic<uint64_t> g_thread_sum{0};

void thread_drop(void *pointer) {
  g_thread_drops.fetch_add(1, std::memory_order_relaxed);
  g_thread_sum.fetch_add(
      static_cast<uint64_t>(*static_cast<int *>(pointer)),
      std::memory_order_relaxed);
}

std::atomic<unsigned> g_values_copy_calls{0};
std::atomic<unsigned> g_outer_pointer_drops{0};
std::atomic<unsigned> g_byte_drops{0};

void owned_value_copy(void *pointer) {
  g_values_copy_calls.fetch_add(1, std::memory_order_relaxed);
  lang_vec_retain_v1(*static_cast<LangVec **>(pointer));
}

void owned_value_drop(void *pointer) {
  g_outer_pointer_drops.fetch_add(1, std::memory_order_relaxed);
  lang_vec_release_v1(*static_cast<LangVec **>(pointer));
}

void owned_byte_drop(void *pointer) {
  (void)pointer;
  g_byte_drops.fetch_add(1, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Child-process helper for terminal Panic checks.
template <typename Operation>
int run_child(Operation &&operation, std::string &record) {
  int fds[2];
  if (pipe(fds) != 0) return -1;
  const pid_t pid = fork();
  if (pid == 0) {
    close(fds[0]);
    if (dup2(fds[1], 2) < 0) _exit(120);
    close(fds[1]);
    operation();
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

template <typename Operation>
bool expect_panic(const char *name, const char *cause, Operation &&operation) {
  std::string record;
  const int code = run_child(operation, record);
  const std::string marker =
      std::string("LANG_NATIVE_PANIC:v1:") + cause + ":";
  if (code == 101 && record.find(marker) != std::string::npos) return true;
  std::fprintf(stderr, "FAIL: panic %s exit=%d record=%s\n", name, code,
               record.c_str());
  ++g_failures;
  return false;
}

// ---------------------------------------------------------------------------
void test_layout_and_new() {
  reset_element_counters();
  LangVec *v = lang_vec_new_v1(4, sizeof(int), tracked_copy, tracked_drop);
  CHECK(v != nullptr);
  CHECK(v->refs == 1);
  CHECK(v->iterations == 0);
  const int values[] = {10, 11, 12, 13};
  for (int value : values) lang_vec_init_v1(v, &value);
  CHECK(v->initialized == 4);

  LangSlice slice{};
  lang_slice_new_v1(&slice, v, 1, 3);
  CHECK(slice.owner == v);
  CHECK(slice.start == 1);
  CHECK(slice.length == 2);
  CHECK(v->refs == 2);
  CHECK(v->iterations == 1);
  CHECK(lang_slice_length_v1(&slice) == 2);
  CHECK(*static_cast<int *>(lang_slice_at_v1(&slice, 0)) == 11);
  CHECK(*static_cast<int *>(lang_slice_at_v1(&slice, 1)) == 12);

  lang_vec_release_v1(v);
  CHECK(v->refs == 1);
  CHECK(v->iterations == 1);
  CHECK(*static_cast<int *>(lang_slice_at_v1(&slice, 0)) == 11);
  *static_cast<int *>(lang_slice_at_v1(&slice, 1)) = 19;
  CHECK(*static_cast<int *>(lang_slice_at_v1(&slice, 0)) == 11);
  CHECK(*static_cast<int *>(lang_slice_at_v1(&slice, 1)) == 19);

  lang_slice_drop_v1(&slice);
  CHECK(slice.owner == nullptr && slice.start == 0 && slice.length == 0);
  CHECK(g_int_drops.load() == 4);
  CHECK(g_drop_counts[10].load() == 1);
  CHECK(g_drop_counts[11].load() == 1);
  CHECK(g_drop_counts[19].load() == 1);
  CHECK(g_drop_counts[13].load() == 1);
  CHECK(g_bad_drop.load() == 0);
  CHECK(g_element_copies.load() == 0);
  check_allocation_health();
}

void test_copy_and_aggregate_storage() {
  reset_element_counters();
  LangVec *v = lang_vec_new_v1(5, sizeof(int), nullptr, tracked_drop);
  CHECK(v != nullptr);
  const int values[] = {1, 2, 3, 4, 5};
  for (int value : values) lang_vec_init_v1(v, &value);

  LangSlice root{};
  lang_slice_new_v1(&root, v, 1, 4);
  lang_vec_release_v1(v);
  CHECK(v->refs == 1 && v->iterations == 1);

  alignas(8) unsigned char raw[sizeof(LangSlice)];
  std::memcpy(raw, &root, sizeof(LangSlice));
  lang_slice_copy_v1(raw);
  CHECK(v->refs == 2 && v->iterations == 2);
  const auto *raw_slice = reinterpret_cast<const LangSlice *>(raw);
  CHECK(raw_slice->owner == v && raw_slice->start == 1 && raw_slice->length == 3);
  CHECK(*static_cast<int *>(lang_slice_at_v1(raw_slice, 2)) == 4);

  struct Aggregate {
    uint64_t tag;
    LangSlice slice;
    uint64_t tail;
  };
  Aggregate first{};
  first.tag = 0x1122334455667788ULL;
  first.tail = 0x8877665544332211ULL;
  std::memcpy(&first.slice, raw, sizeof(LangSlice));
  lang_slice_copy_v1(&first.slice);
  CHECK(v->refs == 3 && v->iterations == 3);

  Aggregate second = first;
  lang_slice_copy_v1(&second.slice);
  CHECK(v->refs == 4 && v->iterations == 4);
  CHECK(first.tag == second.tag && first.tail == second.tail);
  CHECK(*static_cast<int *>(lang_slice_at_v1(&second.slice, 0)) == 2);

  lang_slice_drop_v1(raw);
  CHECK(v->refs == 3 && v->iterations == 3);
  lang_slice_drop_v1(&first.slice);
  CHECK(v->refs == 2 && v->iterations == 2);
  CHECK(g_int_drops.load() == 0);
  lang_slice_drop_v1(&second.slice);
  CHECK(v->refs == 1 && v->iterations == 1);
  CHECK(g_int_drops.load() == 0);
  lang_slice_drop_v1(&root);
  CHECK(g_int_drops.load() == 5);
  CHECK(g_bad_drop.load() == 0);

  // A zeroed aggregate slot is the only owner-less state. Copying it is a
  // no-op and dropping it repeatedly has no effect.
  LangSlice blank{};
  CHECK(lang_slice_length_v1(&blank) == 0);
  lang_slice_copy_v1(&blank);
  lang_slice_drop_v1(&blank);
  lang_slice_drop_v1(&blank);
  CHECK(blank.owner == nullptr && blank.start == 0 && blank.length == 0);
  check_allocation_health();
}

void test_subslices_and_terminal_empty() {
  LangVec *v = lang_vec_new_v1(6, sizeof(int), nullptr, nullptr);
  CHECK(v != nullptr);
  const int values[] = {10, 11, 12, 13, 14, 15};
  for (int value : values) lang_vec_init_v1(v, &value);

  LangSlice root{};
  lang_slice_new_v1(&root, v, 0, 6);
  lang_vec_release_v1(v);
  CHECK(v->refs == 1 && v->iterations == 1);

  LangSlice mid{};
  lang_slice_sub_v1(&mid, &root, 1, 5);
  CHECK(mid.owner == v && mid.start == 1 && mid.length == 4);
  CHECK(v->refs == 2 && v->iterations == 2);
  CHECK(*static_cast<int *>(lang_slice_at_v1(&mid, 0)) == 11);
  CHECK(*static_cast<int *>(lang_slice_at_v1(&mid, 3)) == 14);

  LangSlice empty{};
  lang_slice_sub_v1(&empty, &mid, 2, 2);
  CHECK(empty.owner == v && empty.start == 3 && empty.length == 0);
  CHECK(v->refs == 3 && v->iterations == 3);
  CHECK(lang_slice_length_v1(&empty) == 0);

  LangSlice terminal{};
  lang_slice_new_v1(&terminal, v, 6, 6);
  CHECK(terminal.owner == v && terminal.start == 6 && terminal.length == 0);
  CHECK(v->refs == 4 && v->iterations == 4);
  CHECK(lang_slice_length_v1(&terminal) == 0);

  LangSlice empty_sub{};
  lang_slice_sub_v1(&empty_sub, &empty, 0, 0);
  CHECK(empty_sub.owner == v && empty_sub.start == 3 && empty_sub.length == 0);
  CHECK(v->refs == 5 && v->iterations == 5);

  // out == source replaces a live slice without leaking its old pin/strong
  // reference.
  LangSlice same{};
  lang_slice_new_v1(&same, v, 0, 4);
  CHECK(v->refs == 6 && v->iterations == 6);
  lang_slice_sub_v1(&same, &same, 1, 3);
  CHECK(same.owner == v && same.start == 1 && same.length == 2);
  CHECK(v->refs == 6 && v->iterations == 6);
  CHECK(*static_cast<int *>(lang_slice_at_v1(&same, 0)) == 11);

  lang_slice_drop_v1(&same);
  CHECK(v->refs == 5 && v->iterations == 5);
  lang_slice_drop_v1(&empty_sub);
  CHECK(v->refs == 4 && v->iterations == 4);
  lang_slice_drop_v1(&terminal);
  CHECK(v->refs == 3 && v->iterations == 3);
  lang_slice_drop_v1(&empty);
  CHECK(v->refs == 2 && v->iterations == 2);
  lang_slice_drop_v1(&mid);
  CHECK(v->refs == 1 && v->iterations == 1);
  lang_slice_drop_v1(&root);
  check_allocation_health();

  // Even an empty borrowed view retains and pins its owner until dropped.
  reset_element_counters();
  LangVec *empty_owner = lang_vec_new_v1(0, sizeof(int), nullptr, tracked_drop);
  CHECK(empty_owner != nullptr);
  LangSlice owner_backed_empty{};
  lang_slice_new_v1(&owner_backed_empty, empty_owner, 0, 0);
  CHECK(owner_backed_empty.owner == empty_owner &&
        owner_backed_empty.length == 0);
  CHECK(empty_owner->refs == 2 && empty_owner->iterations == 1);
  lang_vec_release_v1(empty_owner);
  CHECK(empty_owner->refs == 1 && empty_owner->iterations == 1);
  CHECK(lang_slice_length_v1(&owner_backed_empty) == 0);
  lang_slice_drop_v1(&owner_backed_empty);
  CHECK(g_int_drops.load() == 0);
  check_allocation_health();
}

void test_panic_edges() {
  LangVec *v = lang_vec_new_v1(4, sizeof(int), nullptr, nullptr);
  CHECK(v != nullptr);
  const int values[] = {1, 2};
  for (int value : values) lang_vec_init_v1(v, &value);

  expect_panic("new-null-out", "runtime-handle-misuse",
               [v] { lang_slice_new_v1(nullptr, v, 0, 1); });
  expect_panic("new-null-owner", "runtime-handle-misuse", [] {
    LangSlice out{};
    lang_slice_new_v1(&out, nullptr, 0, 0);
  });
  expect_panic("new-start-after-end", "vec-slice", [v] {
    LangSlice out{};
    lang_slice_new_v1(&out, v, 3, 2);
  });
  expect_panic("new-end-after-length", "vec-slice", [v] {
    LangSlice out{};
    lang_slice_new_v1(&out, v, 0, 5);
  });
  expect_panic("new-end-after-initialized", "vec-index", [v] {
    LangSlice out{};
    lang_slice_new_v1(&out, v, 0, 3);
  });

  expect_panic("at-index-out-of-bounds", "vec-index", [v] {
    LangSlice slice{};
    lang_slice_new_v1(&slice, v, 0, 2);
    lang_slice_at_v1(&slice, 2);
  });
  expect_panic("at-blank", "vec-index", [] {
    LangSlice blank{};
    lang_slice_at_v1(&blank, 0);
  });
  expect_panic("at-owner-missing", "runtime-handle-misuse", [] {
    LangSlice malformed{nullptr, 0, 1};
    lang_slice_at_v1(&malformed, 0);
  });
  expect_panic("at-start-overflow", "vec-slice", [v] {
    LangSlice forged{v, ~uint64_t{0} - 1, 3};
    lang_slice_at_v1(&forged, 2);
  });
  expect_panic("at-null-slice", "runtime-handle-misuse",
               [] { lang_slice_at_v1(nullptr, 0); });
  expect_panic("length-owner-missing", "runtime-handle-misuse", [] {
    LangSlice malformed{nullptr, 0, 1};
    lang_slice_length_v1(&malformed);
  });
  expect_panic("length-null", "runtime-handle-misuse",
               [] { lang_slice_length_v1(nullptr); });

  expect_panic("copy-null-storage", "runtime-handle-misuse",
               [] { lang_slice_copy_v1(nullptr); });
  expect_panic("copy-blank-malformed", "runtime-handle-misuse", [] {
    LangSlice malformed{nullptr, 1, 0};
    lang_slice_copy_v1(&malformed);
  });
  expect_panic("copy-owner-missing", "runtime-handle-misuse", [] {
    LangSlice malformed{nullptr, 0, 1};
    lang_slice_copy_v1(&malformed);
  });
  expect_panic("copy-bad-range", "vec-slice", [v] {
    LangSlice malformed{v, 2, 3};
    lang_slice_copy_v1(&malformed);
  });
  expect_panic("copy-uninitialized", "vec-index", [v] {
    LangSlice malformed{v, 0, 3};
    lang_slice_copy_v1(&malformed);
  });

  expect_panic("drop-null-storage", "runtime-handle-misuse",
               [] { lang_slice_drop_v1(nullptr); });
  expect_panic("drop-blank-malformed", "runtime-handle-misuse", [] {
    LangSlice malformed{nullptr, 0, 1};
    lang_slice_drop_v1(&malformed);
  });
  expect_panic("drop-bad-range", "vec-slice", [v] {
    LangSlice malformed{v, 2, 3};
    lang_slice_drop_v1(&malformed);
  });
  expect_panic("drop-uninitialized", "vec-index", [v] {
    LangSlice malformed{v, 0, 3};
    lang_slice_drop_v1(&malformed);
  });

  expect_panic("sub-null-out", "runtime-handle-misuse", [v] {
    LangSlice source{};
    lang_slice_new_v1(&source, v, 0, 2);
    lang_slice_sub_v1(nullptr, &source, 0, 1);
  });
  expect_panic("sub-null-source", "runtime-handle-misuse", [] {
    LangSlice out{};
    lang_slice_sub_v1(&out, nullptr, 0, 0);
  });
  expect_panic("sub-owner-missing", "runtime-handle-misuse", [] {
    LangSlice source{nullptr, 0, 1};
    LangSlice out{};
    lang_slice_sub_v1(&out, &source, 0, 0);
  });
  expect_panic("sub-start-after-end", "vec-slice", [v] {
    LangSlice source{};
    lang_slice_new_v1(&source, v, 0, 2);
    LangSlice out{};
    lang_slice_sub_v1(&out, &source, 2, 1);
  });
  expect_panic("sub-end-after-length", "vec-slice", [v] {
    LangSlice source{};
    lang_slice_new_v1(&source, v, 0, 2);
    LangSlice out{};
    lang_slice_sub_v1(&out, &source, 0, 3);
  });
  expect_panic("sub-source-overflow", "vec-slice", [v] {
    LangSlice source{v, ~uint64_t{0} - 1, 2};
    LangSlice out{};
    lang_slice_sub_v1(&out, &source, 0, 1);
  });

  CHECK(v->refs == 1);
  CHECK(v->iterations == 0);
  lang_vec_release_v1(v);
  check_allocation_health();
}

// ---------------------------------------------------------------------------
// Structural Vec changes while a Slice pin is active must panic before any
// length or storage mutation. The seed lives in shared anonymous memory so the
// parent can compare descriptor fields and payload after the child Panic.
constexpr uint64_t kVecHeaderBytes =
    (static_cast<uint64_t>(sizeof(LangVec)) + 15) & ~uint64_t{15};
constexpr size_t kSharedPayload = 4 * sizeof(int);

LangVec *seed_shared_vec(void *region) {
  auto *v = static_cast<LangVec *>(region);
  v->refs = 1;
  v->length = 3;
  v->initialized = 3;
  v->stride = sizeof(int);
  v->iterations = 0;
  v->data = static_cast<unsigned char *>(region) + kVecHeaderBytes;
  v->copy = nullptr;
  v->drop = nullptr;
  v->capacity = 4;
  auto *values = static_cast<int *>(v->data);
  values[0] = 11;
  values[1] = 22;
  values[2] = 33;
  values[3] = 44;
  return v;
}

enum class Mutation {
  reserve_grow,
  push,
  pop,
  resize_grow,
  resize_shrink,
  clear
};

void test_structural_mutation_guard() {
  constexpr size_t kRegionBytes = kVecHeaderBytes + kSharedPayload;
  void *region = mmap(nullptr, kRegionBytes, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  CHECK(region != MAP_FAILED);
  if (region == MAP_FAILED) return;
  LangVec *v = seed_shared_vec(region);
  const int payload_before[4] = {11, 22, 33, 44};

  const Mutation mutations[] = {Mutation::reserve_grow, Mutation::push,
                                Mutation::pop, Mutation::resize_grow,
                                Mutation::resize_shrink, Mutation::clear};
  const char *names[] = {"reserve-grow", "push", "pop", "resize-grow",
                         "resize-shrink", "clear"};
  for (unsigned index = 0; index < 6; ++index) {
    const Mutation mutation = mutations[index];
    __atomic_store_n(&v->refs, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&v->iterations, 0u, __ATOMIC_RELAXED);
    const uint64_t length_before = v->length;
    const uint64_t initialized_before = v->initialized;
    const uint64_t capacity_before = v->capacity;
    void *data_before = v->data;
    std::string record;
    const int code = run_child(
        [&] {
          LangSlice slice{};
          lang_slice_new_v1(&slice, v, 0, 3);
          switch (mutation) {
          case Mutation::reserve_grow:
            lang_vec_reserve_v1(v, 5);
            break;
          case Mutation::push: {
            int value = 55;
            lang_vec_push_v1(v, &value);
            break;
          }
          case Mutation::pop: {
            int out = 0;
            lang_vec_pop_v1(v, &out);
            break;
          }
          case Mutation::resize_grow: {
            int fill = 55;
            lang_vec_resize_v1(v, 4, &fill);
            break;
          }
          case Mutation::resize_shrink:
            lang_vec_resize_v1(v, 2, nullptr);
            break;
          case Mutation::clear:
            lang_vec_clear_v1(v);
            break;
          }
        },
        record);
    const bool panicked =
        code == 101 && record.find("active-iteration") != std::string::npos;
    CHECK(panicked);
    CHECK(__atomic_load_n(&v->refs, __ATOMIC_RELAXED) == 2);
    CHECK(__atomic_load_n(&v->iterations, __ATOMIC_RELAXED) == 1);
    CHECK(v->length == length_before);
    CHECK(v->initialized == initialized_before);
    CHECK(v->capacity == capacity_before);
    CHECK(v->data == data_before);
    CHECK(v->stride == sizeof(int));
    CHECK(std::memcmp(v->data, payload_before, sizeof(payload_before)) == 0);
    if (!panicked)
      std::fprintf(stderr, "FAIL: mutation %s exit=%d record=%s\n",
                   names[index], code, record.c_str());
  }
  __atomic_store_n(&v->refs, 1u, __ATOMIC_RELAXED);
  __atomic_store_n(&v->iterations, 0u, __ATOMIC_RELAXED);
  munmap(region, kRegionBytes);

  // reserve within existing capacity is not a structural change and is legal
  // while a Slice pin is active.
  LangVec *vec = lang_vec_new_v1(3, sizeof(int), nullptr, nullptr);
  CHECK(vec != nullptr);
  const int values[] = {7, 8, 9};
  for (int value : values) lang_vec_init_v1(vec, &value);
  LangSlice slice{};
  lang_slice_new_v1(&slice, vec, 0, 3);
  lang_vec_release_v1(vec);
  void *data = vec->data;
  const uint64_t length = vec->length;
  const uint64_t capacity = vec->capacity;
  lang_vec_reserve_v1(vec, capacity);
  CHECK(vec->data == data);
  CHECK(vec->length == length);
  CHECK(vec->capacity == capacity);
  CHECK(*static_cast<int *>(lang_slice_at_v1(&slice, 2)) == 9);
  lang_slice_drop_v1(&slice);
  check_allocation_health();

  // Once every Slice pin is gone, structural changes succeed again.
  LangVec *mut = lang_vec_new_v1(3, sizeof(int), nullptr, nullptr);
  CHECK(mut != nullptr);
  for (int value : values) lang_vec_init_v1(mut, &value);
  LangSlice transient{};
  lang_slice_new_v1(&transient, mut, 0, 3);
  lang_slice_drop_v1(&transient);
  CHECK(mut->refs == 1 && mut->iterations == 0);
  int four = 4;
  lang_vec_push_v1(mut, &four);
  CHECK(mut->length == 4);
  CHECK(*static_cast<int *>(lang_vec_at_v1(mut, 3)) == 4);
  lang_vec_resize_v1(mut, 2, nullptr);
  CHECK(mut->length == 2);
  lang_vec_clear_v1(mut);
  CHECK(mut->length == 0);
  lang_vec_reserve_v1(mut, 16);
  CHECK(mut->capacity >= 16);
  lang_vec_release_v1(mut);
  check_allocation_health();
}

// ---------------------------------------------------------------------------
void test_threaded_alias_copy_drop() {
  constexpr unsigned kThreads = 8;
  constexpr unsigned kIterations = 1000;
  reset_element_counters();
  g_thread_drops.store(0, std::memory_order_relaxed);
  g_thread_sum.store(0, std::memory_order_relaxed);

  LangVec *v = lang_vec_new_v1(4, sizeof(int), nullptr, thread_drop);
  CHECK(v != nullptr);
  const int values[] = {0, 1, 2, 3};
  for (int value : values) lang_vec_init_v1(v, &value);
  LangSlice root{};
  lang_slice_new_v1(&root, v, 0, 4);
  lang_vec_release_v1(v);
  CHECK(v->refs == 1 && v->iterations == 1);

  std::mutex copy_lock;
  std::atomic<unsigned> bad_reads{0};
  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (unsigned thread_index = 0; thread_index < kThreads; ++thread_index) {
    workers.emplace_back([&, thread_index] {
      for (unsigned iteration = 0; iteration < kIterations; ++iteration) {
        LangSlice local{};
        {
          std::lock_guard<std::mutex> guard(copy_lock);
          local = root;
        }
        lang_slice_copy_v1(&local);
        LangSlice sub{};
        lang_slice_sub_v1(&sub, &local, 1, 3);

        auto *local_slot = static_cast<int *>(
            lang_slice_at_v1(&local, (thread_index + iteration) % 4));
        __atomic_fetch_add(local_slot, 1, __ATOMIC_RELAXED);
        auto *sub_slot = static_cast<int *>(
            lang_slice_at_v1(&sub, (thread_index + 2 * iteration) % 2));
        __atomic_fetch_add(sub_slot, 1, __ATOMIC_RELAXED);
        if (__atomic_load_n(local_slot, __ATOMIC_RELAXED) <= 0)
          bad_reads.fetch_add(1, std::memory_order_relaxed);

        lang_slice_drop_v1(&sub);
        lang_slice_drop_v1(&local);
      }
    });
  }
  for (auto &worker : workers) worker.join();

  CHECK(bad_reads.load() == 0);
  CHECK(v->refs == 1);
  CHECK(v->iterations == 1);
  uint64_t element_sum = 0;
  for (unsigned index = 0; index < 4; ++index)
    element_sum += static_cast<uint64_t>(
        __atomic_load_n(static_cast<int *>(v->data) + index,
                        __ATOMIC_RELAXED));
  const uint64_t expected_sum =
      6 + static_cast<uint64_t>(kThreads) * kIterations * 2;
  CHECK(element_sum == expected_sum);

  lang_slice_drop_v1(&root);
  CHECK(g_thread_drops.load() == 4);
  CHECK(g_thread_sum.load() == expected_sum);
  check_allocation_health();
}

void test_owned_payload_drops_once() {
  reset_element_counters();
  g_values_copy_calls.store(0, std::memory_order_relaxed);
  g_outer_pointer_drops.store(0, std::memory_order_relaxed);
  g_byte_drops.store(0, std::memory_order_relaxed);

  LangVec *values = lang_vec_new_v1(2, sizeof(int), nullptr, tracked_drop);
  CHECK(values != nullptr);
  const int initial[] = {7, 8};
  for (int value : initial) lang_vec_init_v1(values, &value);

  LangVec *bytes = lang_vec_new_v1(3, 1, nullptr, owned_byte_drop);
  CHECK(bytes != nullptr);
  const unsigned char text[] = {'h', 'i', '!'};
  for (unsigned char byte : text) lang_vec_init_v1(bytes, &byte);

  LangVec *outer = lang_vec_new_v1(0, sizeof(LangVec *), owned_value_copy,
                                   owned_value_drop);
  CHECK(outer != nullptr);
  LangVec *value_pointer = values;
  lang_vec_resize_v1(outer, 2, &value_pointer);
  CHECK(values->refs == 3);
  lang_vec_set_v1(outer, 1, &bytes);
  CHECK(values->refs == 2);
  lang_vec_retain_v1(bytes);
  lang_vec_release_v1(values);
  lang_vec_release_v1(bytes);
  CHECK(values->refs == 1);
  CHECK(bytes->refs == 1);
  CHECK(g_values_copy_calls.load() == 2);
  CHECK(g_outer_pointer_drops.load() == 1);

  LangSlice root{};
  lang_slice_new_v1(&root, outer, 0, 2);
  lang_vec_release_v1(outer);
  CHECK(outer->refs == 1 && outer->iterations == 1);
  LangSlice copy = root;
  lang_slice_copy_v1(&copy);
  CHECK(outer->refs == 2 && outer->iterations == 2);
  lang_slice_drop_v1(&root);
  CHECK(outer->refs == 1 && outer->iterations == 1);
  CHECK(g_values_copy_calls.load() == 2);
  CHECK(g_outer_pointer_drops.load() == 1);
  CHECK(g_int_drops.load() == 0);
  CHECK(g_byte_drops.load() == 0);

  lang_slice_drop_v1(&copy);
  CHECK(g_outer_pointer_drops.load() == 3);
  CHECK(g_int_drops.load() == 2);
  CHECK(g_byte_drops.load() == 3);
  CHECK(g_bad_drop.load() == 0);
  check_allocation_health();
}

} // namespace

int main() {
  test_layout_and_new();
  test_copy_and_aggregate_storage();
  test_subslices_and_terminal_empty();
  test_panic_edges();
  test_structural_mutation_guard();
  test_threaded_alias_copy_drop();
  test_owned_payload_drops_once();
  check_allocation_health();
  if (g_failures) {
    std::fprintf(stderr, "slice runtime harness: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("slice runtime harness: passed\n");
  return 0;
}
