// Compact test-only lifecycle observer for the large binary-trees fixture.
//
// Build with -DBT_CPP_OBSERVER for the C++ reference: global new/delete are
// counted and the Node leaf flag is read before the allocation is released.
// Without that define it is linked to the frozen Source object with
// --wrap=lang_alloc_v1/lang_free_v1 and the runtime's weak Vec observation ABI.
//
// Only fixed-size per-phase/per-bucket/per-round summaries are written.  The
// hash table holds only currently live allocations and Vec identities, so its
// memory is proportional to peak live objects, not to historical nodes.  This
// observer depends on the host libc and is never part of a formal program.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#if !defined(BT_CPP_OBSERVER)
#include "vec.hpp"
#endif

namespace {

constexpr uint32_t kMaxRounds = 2;
constexpr uint32_t kMaxPhases = 10;
constexpr uint32_t kMaxSummaries = kMaxRounds * kMaxPhases;
#if defined(BT_CPP_OBSERVER)
constexpr uint32_t kNodeSize = 24;
#endif

uint64_t perfect_nodes(uint32_t depth) {
  return (uint64_t{1} << (depth + 1)) - 1;
}

struct Phase {
  uint32_t kind;  // 0 stretch, 1 long-lived, 2 bucket
  uint32_t depth;
  uint64_t trees;
  uint64_t nodes_each;
};

struct Summary {
  uint32_t kind;
  uint32_t depth;
  uint64_t expected_trees;
  uint64_t actual_trees;
  uint64_t created;
  uint64_t leaves;
  uint64_t internal;
  uint64_t released;
  uint64_t peak;
  uint64_t long_live;
  uint64_t final_active;
};

struct Tree {
  uint64_t expected;
  uint64_t created;
  uint64_t released;
  uint64_t leaves;
  uint64_t internal;
  uint64_t active;
  bool is_long;
  bool creation_complete;
};

struct Entry {
  const void *key;
  uint64_t size;
  uint8_t tag;  // 0 none, 1 current tree, 2 long-lived tree
};

struct Table {
  Entry *entries;
  size_t capacity;
  size_t used;
  size_t tombs;
};

Table g_table;
Phase g_phases[kMaxPhases];
Summary g_summaries[kMaxSummaries];
uint32_t g_depth;
uint32_t g_rounds;
uint32_t g_phase_count;
uint32_t g_round;
uint32_t g_phase_index;
uint64_t g_tree_in_phase;
uint32_t g_long_summary_index;
bool g_expect_long_release;
bool g_finished;
bool g_initialized;
bool g_printed;
bool g_failed;
Tree g_current;
Tree g_long;
std::FILE *g_out;
uint64_t g_physical_peak;

[[noreturn]] void fail(uint32_t code, const char *message) {
  if (!g_failed) {
    std::fprintf(stderr, "LARGE_OBSERVER_ERROR:%u:%s\n", code, message);
    g_failed = true;
  }
  std::_Exit(90);
}

[[noreturn]] void bad_alloc() {
  fail(95, "observer allocation failed");
}

void *xcalloc(size_t count, size_t size) {
  void *pointer = std::calloc(count, size);
  if (!pointer)
    bad_alloc();
  return pointer;
}

uint64_t hash_pointer(const void *pointer) {
  uint64_t value = reinterpret_cast<uintptr_t>(pointer);
  value ^= value >> 33;
  value *= UINT64_C(0xff51afd7ed558ccd);
  value ^= value >> 33;
  value *= UINT64_C(0xc4ceb9fe1a85ec53);
  value ^= value >> 33;
  return value;
}

size_t lookup_slot(const void *key) {
  if (g_table.capacity == 0)
    return static_cast<size_t>(-1);
  size_t index = hash_pointer(key) & (g_table.capacity - 1);
  for (;;) {
    const Entry &entry = g_table.entries[index];
    if (entry.key == key)
      return index;
    if (entry.key == nullptr)
      return static_cast<size_t>(-1);
    index = (index + 1) & (g_table.capacity - 1);
  }
}

size_t insert_slot(const void *key) {
  size_t index = hash_pointer(key) & (g_table.capacity - 1);
  size_t first_tombstone = static_cast<size_t>(-1);
  for (;;) {
    const Entry &entry = g_table.entries[index];
    if (entry.key == key)
      fail(100, "duplicate live allocation address");
    if (entry.key == reinterpret_cast<const void *>(1)) {
      if (first_tombstone == static_cast<size_t>(-1))
        first_tombstone = index;
    } else if (entry.key == nullptr) {
      return first_tombstone == static_cast<size_t>(-1) ? index : first_tombstone;
    }
    index = (index + 1) & (g_table.capacity - 1);
  }
}

void rehash(size_t capacity) {
  Entry *old = g_table.entries;
  size_t old_capacity = g_table.capacity;
  g_table.entries = static_cast<Entry *>(xcalloc(capacity, sizeof(Entry)));
  g_table.capacity = capacity;
  g_table.used = 0;
  g_table.tombs = 0;
  for (size_t i = 0; i < old_capacity; ++i) {
    if (old[i].key == nullptr || old[i].key == reinterpret_cast<const void *>(1))
      continue;
    size_t slot = insert_slot(old[i].key);
    g_table.entries[slot] = old[i];
    ++g_table.used;
  }
  std::free(old);
}

void table_insert(const void *key, uint64_t size, uint8_t tag) {
  if (key == nullptr)
    return;
  if (g_table.capacity == 0 || (g_table.used + g_table.tombs + 1) * 4 >=
                                  g_table.capacity * 3) {
    size_t capacity = g_table.capacity ? g_table.capacity * 2 : 1024;
    rehash(capacity);
  }
  if (g_table.tombs > g_table.capacity / 4)
    rehash(g_table.capacity);
  size_t slot = insert_slot(key);
  if (g_table.entries[slot].key == reinterpret_cast<const void *>(1))
    --g_table.tombs;
  g_table.entries[slot] = {key, size, tag};
  ++g_table.used;
  if (g_table.used > g_physical_peak)
    g_physical_peak = g_table.used;
}

bool table_remove(const void *key, Entry *removed) {
  size_t slot = lookup_slot(key);
  if (slot == static_cast<size_t>(-1))
    return false;
  if (removed)
    *removed = g_table.entries[slot];
  g_table.entries[slot].key = reinterpret_cast<const void *>(1);
  g_table.entries[slot].size = 0;
  g_table.entries[slot].tag = 0;
  --g_table.used;
  ++g_table.tombs;
  if (g_table.tombs > g_table.capacity / 4)
    rehash(g_table.capacity);
  return true;
}

[[maybe_unused]] Entry *table_find(const void *key) {
  size_t slot = lookup_slot(key);
  return slot == static_cast<size_t>(-1) ? nullptr : &g_table.entries[slot];
}

Summary &current_summary() {
  return g_summaries[g_round * g_phase_count + g_phase_index];
}

void init_plan() {
  if (g_initialized)
    return;
  g_initialized = true;

  const char *summary = std::getenv("BT_LARGE_SUMMARY");
  if (!summary || !summary[0])
    fail(96, "BT_LARGE_SUMMARY is not set");
  g_out = std::fopen(summary, "w");
  if (!g_out)
    fail(97, "cannot open BT_LARGE_SUMMARY");
  std::setvbuf(g_out, nullptr, _IOFBF, 1u << 20);

  const char *depth_text = std::getenv("BT_LARGE_DEPTH");
  const char *rounds_text = std::getenv("BT_LARGE_ROUNDS");
  if (!depth_text || !rounds_text)
    fail(98, "BT_LARGE_DEPTH/BT_LARGE_ROUNDS is not set");
  char *end = nullptr;
  const unsigned long depth = std::strtoul(depth_text, &end, 10);
  if (!end || *end || (depth != 14 && depth != 18))
    fail(99, "BT_LARGE_DEPTH must be 14 or 18");
  end = nullptr;
  const unsigned long rounds = std::strtoul(rounds_text, &end, 10);
  if (!end || *end || rounds < 1 || rounds > kMaxRounds)
    fail(99, "BT_LARGE_ROUNDS must be 1 or 2");

  g_depth = static_cast<uint32_t>(depth);
  g_rounds = static_cast<uint32_t>(rounds);
  const uint32_t buckets = (g_depth - 4) / 2 + 1;
  g_phase_count = 2 + buckets;
  if (g_phase_count > kMaxPhases)
    fail(99, "too many phases");

  g_phases[0] = {0, g_depth + 1, 1, perfect_nodes(g_depth + 1)};
  g_phases[1] = {1, g_depth, 1, perfect_nodes(g_depth)};
  for (uint32_t index = 0; index < buckets; ++index) {
    const uint32_t phase_depth = 4 + index * 2;
    g_phases[index + 2] = {
        2, phase_depth, uint64_t{1} << (g_depth - phase_depth + 4),
        perfect_nodes(phase_depth)};
  }

  std::memset(g_summaries, 0, sizeof(g_summaries));
  for (uint32_t round = 0; round < g_rounds; ++round) {
    for (uint32_t phase = 0; phase < g_phase_count; ++phase) {
      Summary &summary = g_summaries[round * g_phase_count + phase];
      summary.kind = g_phases[phase].kind;
      summary.depth = g_phases[phase].depth;
      summary.expected_trees = g_phases[phase].trees;
    }
  }

  g_round = 0;
  g_phase_index = 0;
  g_tree_in_phase = 0;
  g_long_summary_index = 0;
  g_expect_long_release = false;
  g_finished = false;
  g_printed = false;
  g_failed = false;
  g_current = {};
  g_long = {};
  g_physical_peak = 0;

  std::fprintf(g_out, "PLAN:%u:%u:%u\n", g_depth, g_rounds, g_phase_count);
}

void start_tree() {
  if (g_finished || g_expect_long_release)
    fail(110, "unexpected tree creation");
  if (g_phase_index >= g_phase_count)
    fail(111, "phase index out of range");
  const Phase &phase = g_phases[g_phase_index];
  if (g_tree_in_phase >= phase.trees)
    fail(112, "too many trees in phase");
  if (g_current.expected || g_current.created || g_current.active)
    fail(113, "tree state not reset");
  g_current = {};
  g_current.expected = phase.nodes_each;
  g_current.is_long = phase.kind == 1;
}

void finish_tree_creation() {
  if (g_current.created != g_current.expected)
    fail(120, "tree creation count mismatch");
  if (g_current.active != g_current.expected)
    fail(121, "tree creation active count mismatch");
  if (g_current.is_long) {
    Summary &summary = current_summary();
    summary.created = g_current.created;
    summary.leaves = g_current.leaves;
    summary.internal = g_current.internal;
    g_long = g_current;
    g_long_summary_index = g_round * g_phase_count + g_phase_index;
    g_current = {};
    ++g_phase_index;
    g_tree_in_phase = 0;
    if (g_phase_index >= g_phase_count)
      fail(123, "long-lived tree without bucket phases");
  } else {
    g_current.creation_complete = true;
  }
}

void created_one(bool known_leaf, bool leaf) {
  if (g_current.expected == 0)
    start_tree();
  ++g_current.created;
  ++g_current.active;
  if (known_leaf) {
    if (leaf)
      ++g_current.leaves;
    else
      ++g_current.internal;
  }
  Summary &summary = current_summary();
  const uint64_t live = g_current.active + g_long.active;
  if (live > summary.peak)
    summary.peak = live;
  if (g_current.created > g_current.expected)
    fail(124, "tree creation exceeded expected nodes");
  if (g_current.created == g_current.expected)
    finish_tree_creation();
}

void finish_current_tree() {
  if (!g_current.creation_complete)
    fail(130, "tree released before creation completed");
  if (g_current.released != g_current.expected)
    fail(131, "tree release count mismatch");
  if (g_current.leaves + g_current.internal != g_current.expected) {
    std::fprintf(stderr,
                 "LARGE_OBSERVER_DEBUG:132:expected=%llu:created=%llu:"
                 "leaves=%llu:internal=%llu:released=%llu\n",
                 static_cast<unsigned long long>(g_current.expected),
                 static_cast<unsigned long long>(g_current.created),
                 static_cast<unsigned long long>(g_current.leaves),
                 static_cast<unsigned long long>(g_current.internal),
                 static_cast<unsigned long long>(g_current.released));
    fail(132, "tree leaf/internal count mismatch");
  }
  Summary &summary = current_summary();
  ++summary.actual_trees;
  summary.created += g_current.created;
  summary.leaves += g_current.leaves;
  summary.internal += g_current.internal;
  summary.released += g_current.released;
  summary.final_active = 0;
  summary.long_live = g_long.active;
  ++g_tree_in_phase;
  if (g_tree_in_phase == g_phases[g_phase_index].trees) {
    if (g_phase_index + 1 < g_phase_count) {
      ++g_phase_index;
      g_tree_in_phase = 0;
    } else {
      g_phase_index = g_phase_count;
      g_expect_long_release = true;
    }
  }
  g_current = {};
}

void released_current() {
  if (g_current.active == 0)
    fail(140, "current tree active underflow");
  ++g_current.released;
  --g_current.active;
  if (g_current.active == 0)
    finish_current_tree();
}

void released_long(bool known_leaf, bool leaf) {
  if (!g_expect_long_release)
    fail(150, "long-lived tree released too early");
  if (g_long.active == 0)
    fail(151, "long-lived tree active underflow");
  if (known_leaf) {
    if (leaf)
      ++g_long.leaves;
    else
      ++g_long.internal;
  }
  ++g_long.released;
  --g_long.active;
  if (g_long.active != 0)
    return;
  if (g_long.released != g_long.expected)
    fail(152, "long-lived tree release count mismatch");
  if (g_long.leaves + g_long.internal != g_long.expected)
    fail(122, "long-lived leaf/internal count mismatch");
  Summary &summary = g_summaries[g_long_summary_index];
  summary.actual_trees = 1;
  summary.leaves = g_long.leaves;
  summary.internal = g_long.internal;
  summary.released = g_long.released;
  summary.final_active = 0;
  summary.long_live = 0;
  g_long = {};
  ++g_round;
  if (g_round < g_rounds) {
    g_phase_index = 0;
    g_tree_in_phase = 0;
    g_expect_long_release = false;
  } else {
    g_finished = true;
  }
}

void print_summaries() {
  if (g_printed)
    return;
  g_printed = true;
  uint64_t created = 0, leaves = 0, internal = 0, released = 0;
  for (uint32_t round = 0; round < g_rounds; ++round) {
    for (uint32_t phase = 0; phase < g_phase_count; ++phase) {
      const Summary &s = g_summaries[round * g_phase_count + phase];
      std::fprintf(g_out,
                   "ROUND:%u:KIND:%u:DEPTH:%u:TREES:%llu:ACTUAL:%llu:"
                   "CREATED:%llu:LEAVES:%llu:INTERNAL:%llu:RELEASED:%llu:"
                   "PEAK:%llu:LONG_LIVE:%llu:FINAL:%llu\n",
                   round, s.kind, s.depth,
                   static_cast<unsigned long long>(s.expected_trees),
                   static_cast<unsigned long long>(s.actual_trees),
                   static_cast<unsigned long long>(s.created),
                   static_cast<unsigned long long>(s.leaves),
                   static_cast<unsigned long long>(s.internal),
                   static_cast<unsigned long long>(s.released),
                   static_cast<unsigned long long>(s.peak),
                   static_cast<unsigned long long>(s.long_live),
                   static_cast<unsigned long long>(s.final_active));
      created += s.created;
      leaves += s.leaves;
      internal += s.internal;
      released += s.released;
    }
  }
  const bool ok = g_finished && g_table.used == 0 &&
                  g_current.active == 0 && g_long.active == 0;
  std::fprintf(g_out,
               "TOTAL:CREATED:%llu:LEAVES:%llu:INTERNAL:%llu:RELEASED:%llu:"
               "PHYSICAL_PEAK:%llu:PHYSICAL_FINAL:%llu\n",
               static_cast<unsigned long long>(created),
               static_cast<unsigned long long>(leaves),
               static_cast<unsigned long long>(internal),
               static_cast<unsigned long long>(released),
               static_cast<unsigned long long>(g_physical_peak),
               static_cast<unsigned long long>(g_table.used));
  std::fprintf(g_out, "STATUS:%s\n", ok ? "ok" : "error");
  std::fflush(g_out);
}

#if !defined(BT_CPP_OBSERVER)
void maybe_finish() {
  if (g_initialized && g_finished && g_table.used == 0)
    print_summaries();
}

void on_source_create(const void *object, uint64_t length) {
  Entry *entry = table_find(object);
  if (!entry || entry->tag != 0)
    fail(160, "Vec create without live allocation entry");
  if (g_current.expected == 0)
    start_tree();
  entry->tag = g_current.is_long ? 2 : 1;
  const bool leaf = length == 0;
  if (!leaf && length != 2)
    fail(161, "unexpected Vec length in tree node");
  created_one(true, leaf);
}

void on_source_release(const void *object) {
  Entry *entry = table_find(object);
  if (!entry || entry->tag == 0)
    fail(162, "Vec release without live Vec tag");
  const uint8_t tag = entry->tag;
  entry->tag = 0;
  if (tag == 1)
    released_current();
  else if (tag == 2)
    released_long(false, false);
  else
    fail(163, "bad Vec tag");
}
#endif

}  // namespace

#if defined(BT_CPP_OBSERVER)

void *operator new(std::size_t size) {
  if (size != kNodeSize)
    fail(170, "unexpected C++ allocation size");
  init_plan();
  if (g_finished || g_expect_long_release)
    fail(171, "unexpected Node allocation");
  if (g_current.expected == 0)
    start_tree();
  void *pointer = std::malloc(size);
  if (!pointer)
    throw std::bad_alloc();
  const uint8_t tag = g_current.is_long ? 2 : 1;
  table_insert(pointer, size, tag);
  created_one(false, false);
  return pointer;
}

void *operator new[](std::size_t size) {
  return operator new(size);
}

void operator delete(void *pointer) noexcept {
  if (!pointer)
    return;
  init_plan();
  Entry entry = {};
  if (!table_remove(pointer, &entry))
    fail(172, "delete of unknown Node");
  if (entry.tag == 0)
    fail(173, "delete of untagged Node");
  const bool leaf = *static_cast<const bool *>(pointer);
  if (entry.tag == 1) {
    if (leaf)
      ++g_current.leaves;
    else
      ++g_current.internal;
    released_current();
  } else if (entry.tag == 2) {
    released_long(true, leaf);
  } else {
    fail(174, "bad Node tag");
  }
  std::free(pointer);
}

void operator delete(void *pointer, std::size_t) noexcept {
  operator delete(pointer);
}

void operator delete[](void *pointer) noexcept {
  operator delete(pointer);
}

void operator delete[](void *pointer, std::size_t) noexcept {
  operator delete(pointer);
}

int tree_program_main();

int main() {
  init_plan();
  const int result = tree_program_main();
  if (!g_finished || g_table.used != 0)
    fail(180, "C++ lifecycle did not finish cleanly");
  print_summaries();
  return result;
}

#else

extern "C" void *__real_lang_alloc_v1(uint64_t, uint64_t) noexcept;
extern "C" void __real_lang_free_v1(void *) noexcept;

extern "C" void *__wrap_lang_alloc_v1(uint64_t size,
                                      uint64_t align) noexcept {
  void *pointer = __real_lang_alloc_v1(size, align);
  if (pointer != nullptr) {
    init_plan();
    table_insert(pointer, size, 0);
  }
  return pointer;
}

extern "C" void __wrap_lang_free_v1(void *pointer) noexcept {
  if (pointer != nullptr) {
    init_plan();
    Entry entry = {};
    if (!table_remove(pointer, &entry))
      fail(181, "free of unknown allocation");
    if (entry.tag != 0)
      fail(182, "Vec object freed before final release");
    __real_lang_free_v1(pointer);
    maybe_finish();
  } else {
    __real_lang_free_v1(pointer);
  }
}

extern "C" void lang_vec_observe_v1(uint32_t event, const void *object,
                                    uint64_t detail) {
  (void)detail;
  init_plan();
  if (event == 1) {
    const auto *value = static_cast<const LangVec *>(object);
    on_source_create(object, value->length);
  } else if (event == 5) {
    on_source_release(object);
  } else {
    Entry *entry = table_find(object);
    if (!entry || entry->tag == 0)
      fail(183, "Vec event on non-live object");
  }
}

#endif
