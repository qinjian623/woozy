// Runtime-only Vec graph-publication tests for lang-gc-program/1.
//
// These tests use the real LangVec C++ runtime and the accepted program/world
// layers. They do not compile a .wz source program and do not claim source
// automatic reclamation. The runtime publication windows are exercised with
// copy/drop callbacks that re-enter Woozy, read the same Vec, and, where the
// contract allows it, perform a structural push from a destructor.

#include "alloc.hpp"
#include "gc.hpp"
#include "gc_program.hpp"
#include "gc_world.hpp"
#include "vec.hpp"
#include "weak.hpp"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <unistd.h>

extern "C" uint64_t gc_test_alloc_live() noexcept;

namespace {

using namespace lang::native::gc_program;

[[noreturn]] void fail_at(const char *file, int line, const char *what) {
  std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, what);
  std::fflush(stderr);
  std::_Exit(1);
}

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition))                                                          \
      fail_at(__FILE__, __LINE__, #condition);                                 \
  } while (0)

uint64_t registry_live() { return lang_gc_registry_live_v1(); }

LangVec *new_inner() {
  return lang_vec_new_v1(0, sizeof(uint64_t), nullptr, nullptr);
}

LangVec **element_slot(LangVec *v, uint64_t index) {
  return reinterpret_cast<LangVec **>(
      static_cast<unsigned char *>(v->data) + index * v->stride);
}

LangVec *slot_value(LangVec *v, uint64_t index) {
  return *element_slot(v, index);
}

// ---------------------------------------------------------------------------
// Real LangVec GC ops used for the final-release registration test.

void gc_vec_trace(void *object, LangGcVisitor visitor, void *context) noexcept {
  auto *vec = static_cast<LangVec *>(object);
  for (uint64_t index = 0; index < vec->initialized; ++index) {
    LangVec *target = *element_slot(vec, index);
    if (target) visitor(target, context);
  }
}

void gc_vec_clear(void *object) noexcept {
  auto *vec = static_cast<LangVec *>(object);
  const uint64_t initialized = vec->initialized;
  vec->initialized = 0;
  vec->length = 0;
  for (uint64_t index = initialized; index > 0; --index) {
    LangVec *target = *element_slot(vec, index - 1);
    if (target) lang_vec_release_v1(target);
  }
}

void gc_vec_retain(void *object) noexcept {
  lang_vec_retain_v1(static_cast<LangVec *>(object));
}

void gc_vec_release(void *object) noexcept {
  lang_vec_release_v1(static_cast<LangVec *>(object));
}

void gc_vec_invalidate(void *object) noexcept {
  auto *vec = static_cast<LangVec *>(object);
  lang_weak_invalidate_slot_v1(&vec->weak_control);
}

const LangGcOps kVecSelfOps = {gc_vec_trace,        gc_vec_clear,
                               gc_vec_retain,       gc_vec_release,
                               gc_vec_invalidate};

bool install_filter(const sock_filter *filter, unsigned short count) {
  sock_fprog program{count, const_cast<sock_filter *>(filter)};
  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
    return false;
  return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0;
}

bool deny_gettid() {
  const sock_filter filter[] = {
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 186, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
  };
  return install_filter(filter, 4);
}

// ---------------------------------------------------------------------------
// 1. init publishes only after the element bytes are complete.

uint64_t g_init_copies = 0;
uint64_t g_init_drops = 0;

void init_copy(void *) noexcept { ++g_init_copies; }
void init_drop(void *) noexcept { ++g_init_drops; }

int mode_init_publish() {
  CHECK(lang_gc_program_init_v1() == LANG_GC_PROGRAM_OK);

  // The source aliases the descriptor's own initialized word. With the fixed
  // copy-before-publish order the element receives the old value (0). The old
  // increment-first code copied 1 into the slot.
  LangVec *trivial = lang_vec_new_v1(1, sizeof(uint64_t), nullptr, nullptr);
  lang_vec_init_v1(trivial, &trivial->initialized);
  CHECK(trivial->initialized == 1);
  CHECK(*static_cast<uint64_t *>(lang_vec_at_v1(trivial, 0)) == 0);

  LangVec *owning =
      lang_vec_new_v1(1, sizeof(uint64_t), init_copy, init_drop);
  lang_vec_init_v1(owning, &owning->initialized);
  CHECK(owning->initialized == 1);
  CHECK(*static_cast<uint64_t *>(lang_vec_at_v1(owning, 0)) == 0);
  CHECK(g_init_copies == 0); // init is a transfer, not a second copy
  CHECK(g_init_drops == 0);

  lang_vec_release_v1(owning);
  lang_vec_release_v1(trivial);
  CHECK(g_init_drops == 1);
  CHECK(lang_gc_program_finish_v1() == LANG_GC_PROGRAM_OK);
  std::puts("VEC_GRAPH_INIT_PUBLISH_OK");
  return 0;
}

// ---------------------------------------------------------------------------
// 2. set publishes the complete new element before dropping the old owner.

LangVec *g_set_outer = nullptr;
LangVec *g_set_b = nullptr;
LangVec *g_set_c = nullptr;
LangVec *g_set_d = nullptr;
uint64_t g_set_drops = 0;
uint64_t g_set_copies = 0;
bool g_set_empty_phase = false;
LangVec *g_set_order[8] = {};
unsigned g_set_order_count = 0;

void set_copy(void *slot) noexcept {
  ++g_set_copies;
  LangVec *target = *static_cast<LangVec **>(slot);
  if (target) lang_vec_retain_v1(target);
}

void set_drop(void *slot) noexcept {
  LangVec *target = *static_cast<LangVec **>(slot);
  if (g_set_order_count < 8) g_set_order[g_set_order_count++] = target;
  ++g_set_drops;
  if (g_set_empty_phase) {
    CHECK(g_set_outer->length == 0);
    CHECK(g_set_outer->initialized == 0);
    CHECK(g_set_outer->data == nullptr);
    CHECK(g_set_outer->capacity == 0);
    CHECK(g_set_outer->refs == 0);
  } else {
    CHECK(g_set_outer->length == 2);
    CHECK(g_set_outer->initialized == 2);
    CHECK(slot_value(g_set_outer, 0) == g_set_c);
    CHECK(slot_value(g_set_outer, 1) ==
          (g_set_drops == 1 ? g_set_b : g_set_d));
    // A destructor re-enters outside the graph-critical window, so SAFE is
    // legal here. If the setter still held CriticalGuard this would Panic.
    int32_t safe = lang_gc_program_enter_safe_v1();
    CHECK(safe == 1);
    lang_gc_program_leave_safe_v1(safe);
    if (g_set_drops == 1) {
      // Replace the remaining slot while the first drop is still running.
      lang_vec_set_v1(g_set_outer, 1, &g_set_d);
      CHECK(slot_value(g_set_outer, 1) == g_set_d);
    }
  }
  lang_vec_release_v1(target);
}

int mode_set_reentrant() {
  CHECK(lang_gc_program_init_v1() == LANG_GC_PROGRAM_OK);
  const uint64_t base = gc_test_alloc_live();
  LangVec *a = new_inner();
  LangVec *b = new_inner();
  LangVec *c = new_inner();
  LangVec *d = new_inner();
  g_set_b = b;
  g_set_c = c;
  g_set_d = d;
  g_set_outer =
      lang_vec_new_v1(2, sizeof(LangVec *), set_copy, set_drop);
  lang_vec_init_v1(g_set_outer, &a);
  lang_vec_init_v1(g_set_outer, &b);
  lang_vec_set_v1(g_set_outer, 0, &c);
  CHECK(g_set_copies == 0); // set is a transfer of a pre-copied value
  CHECK(g_set_drops == 2);  // a, then the nested b replacement
  CHECK(g_set_outer->length == 2);
  CHECK(g_set_outer->initialized == 2);
  CHECK(slot_value(g_set_outer, 0) == c);
  CHECK(slot_value(g_set_outer, 1) == d);
  CHECK(g_set_order_count == 2);
  CHECK(g_set_order[0] == a);
  CHECK(g_set_order[1] == b);

  g_set_empty_phase = true;
  lang_vec_release_v1(g_set_outer);
  CHECK(g_set_drops == 4);
  CHECK(g_set_order_count == 4);
  CHECK(g_set_order[2] == d);
  CHECK(g_set_order[3] == c);
  CHECK(gc_test_alloc_live() == base);
  CHECK(lang_gc_program_finish_v1() == LANG_GC_PROGRAM_OK);
  std::puts("VEC_GRAPH_SET_REENTRANT_OK");
  return 0;
}

// ---------------------------------------------------------------------------
// 3. clear publishes empty before dropping staged owners; a destructor may
//    structurally push the same Vec.

LangVec *g_clear_outer = nullptr;
LangVec *g_clear_e = nullptr;
uint64_t g_clear_drops = 0;
uint64_t g_clear_copies = 0;
bool g_clear_empty_phase = false;
void *g_clear_data = nullptr;
uint64_t g_clear_capacity = 0;
LangVec *g_clear_order[8] = {};
unsigned g_clear_order_count = 0;

void clear_copy(void *slot) noexcept {
  ++g_clear_copies;
  LangVec *target = *static_cast<LangVec **>(slot);
  if (target) lang_vec_retain_v1(target);
}

void clear_drop(void *slot) noexcept {
  LangVec *target = *static_cast<LangVec **>(slot);
  if (g_clear_order_count < 8)
    g_clear_order[g_clear_order_count++] = target;
  ++g_clear_drops;
  if (g_clear_empty_phase) {
    CHECK(g_clear_outer->length == 0);
    CHECK(g_clear_outer->initialized == 0);
    CHECK(g_clear_outer->data == nullptr);
    CHECK(g_clear_outer->capacity == 0);
    CHECK(g_clear_outer->refs == 0);
  } else {
    if (g_clear_drops == 1) {
      CHECK(g_clear_outer->length == 0);
      CHECK(g_clear_outer->initialized == 0);
    } else {
      CHECK(g_clear_outer->length == 1);
      CHECK(g_clear_outer->initialized == 1);
      CHECK(slot_value(g_clear_outer, 0) == g_clear_e);
    }
    CHECK(g_clear_outer->data == g_clear_data);
    CHECK(g_clear_outer->capacity == g_clear_capacity);
    int32_t safe = lang_gc_program_enter_safe_v1();
    CHECK(safe == 1);
    lang_gc_program_leave_safe_v1(safe);
    if (g_clear_drops == 1) {
      lang_vec_push_v1(g_clear_outer, &g_clear_e);
      CHECK(g_clear_outer->length == 1);
      CHECK(g_clear_outer->initialized == 1);
      CHECK(slot_value(g_clear_outer, 0) == g_clear_e);
    }
  }
  lang_vec_release_v1(target);
}

int mode_clear_reentrant() {
  CHECK(lang_gc_program_init_v1() == LANG_GC_PROGRAM_OK);
  const uint64_t base = gc_test_alloc_live();
  LangVec *a = new_inner();
  LangVec *b = new_inner();
  LangVec *c = new_inner();
  LangVec *d = new_inner();
  g_clear_e = new_inner();
  g_clear_outer =
      lang_vec_new_v1(4, sizeof(LangVec *), clear_copy, clear_drop);
  lang_vec_init_v1(g_clear_outer, &a);
  lang_vec_init_v1(g_clear_outer, &b);
  lang_vec_init_v1(g_clear_outer, &c);
  lang_vec_init_v1(g_clear_outer, &d);
  g_clear_data = g_clear_outer->data;
  g_clear_capacity = g_clear_outer->capacity;

  lang_vec_clear_v1(g_clear_outer);
  CHECK(g_clear_copies == 0);
  CHECK(g_clear_drops == 4);
  CHECK(g_clear_order_count == 4);
  CHECK(g_clear_order[0] == d);
  CHECK(g_clear_order[1] == c);
  CHECK(g_clear_order[2] == b);
  CHECK(g_clear_order[3] == a);
  CHECK(g_clear_outer->length == 1);
  CHECK(g_clear_outer->initialized == 1);
  CHECK(g_clear_outer->data == g_clear_data);
  CHECK(g_clear_outer->capacity == g_clear_capacity);
  CHECK(slot_value(g_clear_outer, 0) == g_clear_e);

  g_clear_empty_phase = true;
  lang_vec_release_v1(g_clear_outer);
  CHECK(g_clear_drops == 5);
  CHECK(g_clear_order_count == 5);
  CHECK(g_clear_order[4] == g_clear_e);
  CHECK(gc_test_alloc_live() == base);
  CHECK(lang_gc_program_finish_v1() == LANG_GC_PROGRAM_OK);
  std::puts("VEC_GRAPH_CLEAR_REENTRANT_OK");
  return 0;
}

// ---------------------------------------------------------------------------
// 4. shrink publishes the shorter prefix before dropping staged old owners.

LangVec *g_shrink_outer = nullptr;
LangVec *g_shrink_a = nullptr;
LangVec *g_shrink_b = nullptr;
LangVec *g_shrink_e = nullptr;
uint64_t g_shrink_drops = 0;
uint64_t g_shrink_copies = 0;
bool g_shrink_empty_phase = false;
LangVec *g_shrink_order[8] = {};
unsigned g_shrink_order_count = 0;

void shrink_copy(void *slot) noexcept {
  ++g_shrink_copies;
  LangVec *target = *static_cast<LangVec **>(slot);
  if (target) lang_vec_retain_v1(target);
}

void shrink_drop(void *slot) noexcept {
  LangVec *target = *static_cast<LangVec **>(slot);
  if (g_shrink_order_count < 8)
    g_shrink_order[g_shrink_order_count++] = target;
  ++g_shrink_drops;
  if (g_shrink_empty_phase) {
    CHECK(g_shrink_outer->length == 0);
    CHECK(g_shrink_outer->initialized == 0);
    CHECK(g_shrink_outer->data == nullptr);
    CHECK(g_shrink_outer->capacity == 0);
    CHECK(g_shrink_outer->refs == 0);
  } else {
    CHECK(g_shrink_outer->length == 2);
    CHECK(g_shrink_outer->initialized == 2);
    CHECK(slot_value(g_shrink_outer, 0) ==
          (g_shrink_drops == 1 ? g_shrink_a : g_shrink_e));
    CHECK(slot_value(g_shrink_outer, 1) == g_shrink_b);
    int32_t safe = lang_gc_program_enter_safe_v1();
    CHECK(safe == 1);
    lang_gc_program_leave_safe_v1(safe);
    if (g_shrink_drops == 1) {
      lang_vec_set_v1(g_shrink_outer, 0, &g_shrink_e);
      CHECK(slot_value(g_shrink_outer, 0) == g_shrink_e);
    }
  }
  lang_vec_release_v1(target);
}

int mode_shrink_reentrant() {
  CHECK(lang_gc_program_init_v1() == LANG_GC_PROGRAM_OK);
  const uint64_t base = gc_test_alloc_live();
  LangVec *a = new_inner();
  LangVec *b = new_inner();
  LangVec *c = new_inner();
  LangVec *d = new_inner();
  g_shrink_e = new_inner();
  g_shrink_a = a;
  g_shrink_b = b;
  g_shrink_outer =
      lang_vec_new_v1(4, sizeof(LangVec *), shrink_copy, shrink_drop);
  lang_vec_init_v1(g_shrink_outer, &a);
  lang_vec_init_v1(g_shrink_outer, &b);
  lang_vec_init_v1(g_shrink_outer, &c);
  lang_vec_init_v1(g_shrink_outer, &d);

  lang_vec_resize_v1(g_shrink_outer, 2, nullptr);
  CHECK(g_shrink_copies == 0);
  CHECK(g_shrink_drops == 3);
  CHECK(g_shrink_order_count == 3);
  CHECK(g_shrink_order[0] == d);
  CHECK(g_shrink_order[1] == a); // nested set from d's destructor
  CHECK(g_shrink_order[2] == c);
  CHECK(g_shrink_outer->length == 2);
  CHECK(g_shrink_outer->initialized == 2);
  CHECK(slot_value(g_shrink_outer, 0) == g_shrink_e);
  CHECK(slot_value(g_shrink_outer, 1) == g_shrink_b);

  g_shrink_empty_phase = true;
  lang_vec_release_v1(g_shrink_outer);
  CHECK(g_shrink_drops == 5);
  CHECK(g_shrink_order_count == 5);
  CHECK(g_shrink_order[3] == g_shrink_b);
  CHECK(g_shrink_order[4] == g_shrink_e);
  CHECK(gc_test_alloc_live() == base);
  CHECK(lang_gc_program_finish_v1() == LANG_GC_PROGRAM_OK);
  std::puts("VEC_GRAPH_SHRINK_REENTRANT_OK");
  return 0;
}

// ---------------------------------------------------------------------------
// 5. resize retains every new owner before publishing the grown prefix.

LangVec *g_resize_outer = nullptr;
LangVec *g_resize_fill = nullptr;
uint64_t g_resize_copies = 0;
uint64_t g_resize_drops = 0;
uint64_t g_resize_expected_old = 0;
LangVec *g_resize_order[8] = {};
unsigned g_resize_order_count = 0;

void resize_copy(void *slot) noexcept {
  ++g_resize_copies;
  CHECK(g_resize_outer->length == g_resize_expected_old);
  CHECK(g_resize_outer->initialized == g_resize_expected_old);
  CHECK(*static_cast<LangVec **>(slot) == g_resize_fill);
  lang_vec_retain_v1(g_resize_fill);
}

void resize_drop(void *slot) noexcept {
  LangVec *target = *static_cast<LangVec **>(slot);
  if (g_resize_order_count < 8)
    g_resize_order[g_resize_order_count++] = target;
  ++g_resize_drops;
  if (g_resize_outer->length == 1) {
    CHECK(g_resize_outer->initialized == 1);
    CHECK(slot_value(g_resize_outer, 0) == g_resize_fill);
  } else {
    CHECK(g_resize_outer->length == 0);
    CHECK(g_resize_outer->initialized == 0);
  }
  lang_vec_release_v1(target);
}

int mode_resize_copy_publish() {
  CHECK(lang_gc_program_init_v1() == LANG_GC_PROGRAM_OK);
  const uint64_t base = gc_test_alloc_live();
  g_resize_fill = new_inner();
  g_resize_outer =
      lang_vec_new_v1(0, sizeof(LangVec *), resize_copy, resize_drop);

  g_resize_expected_old = 0;
  LangVec *fill_value = g_resize_fill;
  lang_vec_resize_v1(g_resize_outer, 3, &fill_value);
  CHECK(g_resize_copies == 3);
  CHECK(g_resize_fill->refs == 4);
  CHECK(g_resize_outer->length == 3);
  CHECK(g_resize_outer->initialized == 3);
  for (uint64_t index = 0; index < 3; ++index)
    CHECK(slot_value(g_resize_outer, index) == g_resize_fill);

  g_resize_expected_old = 3;
  lang_vec_resize_v1(g_resize_outer, 1, nullptr);
  CHECK(g_resize_copies == 3);
  CHECK(g_resize_drops == 2);
  CHECK(g_resize_fill->refs == 2);
  CHECK(g_resize_outer->length == 1);
  CHECK(g_resize_outer->initialized == 1);
  CHECK(slot_value(g_resize_outer, 0) == g_resize_fill);

  lang_vec_clear_v1(g_resize_outer);
  CHECK(g_resize_drops == 3);
  CHECK(g_resize_fill->refs == 1);
  CHECK(g_resize_outer->length == 0);
  CHECK(g_resize_outer->initialized == 0);

  lang_vec_release_v1(g_resize_outer);
  lang_vec_release_v1(g_resize_fill);
  CHECK(g_resize_drops == 3);
  CHECK(gc_test_alloc_live() == base);
  CHECK(lang_gc_program_finish_v1() == LANG_GC_PROGRAM_OK);
  std::puts("VEC_GRAPH_RESIZE_COPY_PUBLISH_OK");
  return 0;
}

// ---------------------------------------------------------------------------
// 6. self-aliased push and resize fill across storage growth.

LangVec *g_alias_outer = nullptr;
LangVec *g_alias_a0 = nullptr;
uint64_t g_alias_copies = 0;
uint64_t g_alias_drops = 0;
uint64_t g_alias_a0_drops = 0;
uint64_t g_alias_expected_length = 0;

void alias_copy(void *slot) noexcept {
  ++g_alias_copies;
  CHECK(g_alias_outer->length == g_alias_expected_length);
  CHECK(g_alias_outer->initialized == g_alias_expected_length);
  CHECK(*static_cast<LangVec **>(slot) == g_alias_a0);
  lang_vec_retain_v1(g_alias_a0);
}

void alias_drop(void *slot) noexcept {
  LangVec *target = *static_cast<LangVec **>(slot);
  CHECK(g_alias_outer->length == 0);
  CHECK(g_alias_outer->initialized == 0);
  CHECK(g_alias_outer->data == nullptr);
  CHECK(g_alias_outer->capacity == 0);
  ++g_alias_drops;
  if (target == g_alias_a0) ++g_alias_a0_drops;
  lang_vec_release_v1(target);
}

int mode_selfalias_growth() {
  CHECK(lang_gc_program_init_v1() == LANG_GC_PROGRAM_OK);
  const uint64_t base = gc_test_alloc_live();
  LangVec *inners[8] = {};
  for (auto *&inner : inners) inner = new_inner();
  g_alias_a0 = inners[0];
  g_alias_outer =
      lang_vec_new_v1(0, sizeof(LangVec *), alias_copy, alias_drop);
  for (auto *inner : inners) lang_vec_push_v1(g_alias_outer, &inner);
  CHECK(g_alias_outer->length == 8);
  CHECK(g_alias_copies == 0);

  // Simulate the pre-copied transfer value, then push the slot itself across
  // growth. protect_alias must preserve the source bytes while the old buffer
  // is freed.
  lang_vec_retain_v1(g_alias_a0);
  lang_vec_push_v1(g_alias_outer, lang_vec_at_v1(g_alias_outer, 0));
  CHECK(g_alias_outer->length == 9);
  CHECK(g_alias_outer->initialized == 9);
  CHECK(slot_value(g_alias_outer, 0) == g_alias_a0);
  CHECK(slot_value(g_alias_outer, 8) == g_alias_a0);
  CHECK(g_alias_a0->refs == 2);
  CHECK(g_alias_copies == 0);

  g_alias_expected_length = 9;
  lang_vec_resize_v1(g_alias_outer, 12,
                     lang_vec_at_v1(g_alias_outer, 0));
  CHECK(g_alias_outer->length == 12);
  CHECK(g_alias_outer->initialized == 12);
  CHECK(g_alias_copies == 3);
  CHECK(g_alias_a0->refs == 5);
  for (uint64_t index = 8; index < 12; ++index)
    CHECK(slot_value(g_alias_outer, index) == g_alias_a0);

  g_alias_expected_length = 12;
  lang_vec_resize_v1(g_alias_outer, 20,
                     lang_vec_at_v1(g_alias_outer, 0));
  CHECK(g_alias_outer->length == 20);
  CHECK(g_alias_outer->initialized == 20);
  CHECK(g_alias_copies == 11);
  CHECK(g_alias_a0->refs == 13);
  for (uint64_t index = 12; index < 20; ++index)
    CHECK(slot_value(g_alias_outer, index) == g_alias_a0);

  lang_vec_release_v1(g_alias_outer);
  CHECK(g_alias_drops == 20);
  CHECK(g_alias_a0_drops == 13);
  CHECK(gc_test_alloc_live() == base);
  CHECK(lang_gc_program_finish_v1() == LANG_GC_PROGRAM_OK);
  std::puts("VEC_GRAPH_SELFALIAS_GROWTH_OK");
  return 0;
}

// ---------------------------------------------------------------------------
// 7. Final release unregisters and closes Weak before dropping, then frees
//    only after all drops.

LangVec *g_final_outer = nullptr;
LangWeakControl *g_final_weak = nullptr;
uint64_t g_final_drops = 0;
uint64_t g_final_base = 0;
LangVec *g_final_order[8] = {};
unsigned g_final_order_count = 0;

void final_copy(void *slot) noexcept {
  LangVec *target = *static_cast<LangVec **>(slot);
  if (target) lang_vec_retain_v1(target);
}

void final_drop(void *slot) noexcept {
  LangVec *target = *static_cast<LangVec **>(slot);
  if (g_final_order_count < 8)
    g_final_order[g_final_order_count++] = target;
  ++g_final_drops;
  CHECK(registry_live() == 0); // unregistered before payload drop
  CHECK(!lang_weak_alive_v1(g_final_weak));
  CHECK(lang_weak_upgrade_v1(g_final_weak) == nullptr);
  CHECK(g_final_outer->length == 0);
  CHECK(g_final_outer->initialized == 0);
  CHECK(g_final_outer->data == nullptr);
  CHECK(g_final_outer->capacity == 0);
  CHECK(g_final_outer->refs == 0);
  // Reading the descriptor from a destructor also proves it has not been
  // freed yet (ASAN/UBSAN checks this in the sanitizer build).
  CHECK(gc_test_alloc_live() > g_final_base);
  lang_vec_release_v1(target);
}

int mode_final_release() {
  CHECK(lang_gc_program_init_v1() == LANG_GC_PROGRAM_OK);
  g_final_base = gc_test_alloc_live();
  LangVec *a = new_inner();
  LangVec *b = new_inner();
  g_final_outer =
      lang_vec_new_v1(2, sizeof(LangVec *), final_copy, final_drop);
  lang_vec_init_v1(g_final_outer, &a);
  lang_vec_init_v1(g_final_outer, &b);
  g_final_weak =
      lang_weak_downgrade_v1(g_final_outer, &g_final_outer->weak_control);
  CHECK(g_final_weak != nullptr);
  CHECK(lang_weak_alive_v1(g_final_weak));
  CHECK(lang_gc_program_register_v1(g_final_outer, &kVecSelfOps));
  CHECK(registry_live() >= 1);

  lang_vec_release_v1(g_final_outer);
  CHECK(g_final_drops == 2);
  CHECK(g_final_order_count == 2);
  CHECK(g_final_order[0] == b);
  CHECK(g_final_order[1] == a);
  CHECK(registry_live() == 0);
  CHECK(!lang_weak_alive_v1(g_final_weak));
  CHECK(lang_weak_upgrade_v1(g_final_weak) == nullptr);
  CHECK(gc_test_alloc_live() > g_final_base); // live Weak control remains
  lang_weak_release_v1(g_final_weak);
  CHECK(gc_test_alloc_live() == g_final_base);
  CHECK(lang_gc_program_finish_v1() == LANG_GC_PROGRAM_OK);
  std::puts("VEC_GRAPH_FINAL_RELEASE_OK");
  return 0;
}

// ---------------------------------------------------------------------------
// 8. A pending stop cannot park between the new-element publication and the
//    old-owner drop.

LangVec *g_pending_outer = nullptr;
LangVec *g_pending_b = nullptr;
LangVec *g_pending_c = nullptr;
std::atomic<uint64_t> g_pending_drops{0};

void pending_copy(void *slot) noexcept {
  LangVec *target = *static_cast<LangVec **>(slot);
  if (target) lang_vec_retain_v1(target);
}

void pending_drop(void *slot) noexcept {
  LangVec *target = *static_cast<LangVec **>(slot);
  g_pending_drops.fetch_add(1, std::memory_order_relaxed);
  if (g_pending_outer->length == 0) {
    CHECK(g_pending_outer->initialized == 0);
    CHECK(g_pending_outer->data == nullptr);
    CHECK(g_pending_outer->capacity == 0);
  } else {
    CHECK(g_pending_outer->length == 2);
    CHECK(g_pending_outer->initialized == 2);
    CHECK(slot_value(g_pending_outer, 0) == g_pending_c);
    CHECK(slot_value(g_pending_outer, 1) == g_pending_b);
    int32_t safe = lang_gc_program_enter_safe_v1();
    CHECK(safe == 1);
    lang_gc_program_leave_safe_v1(safe);
    int32_t critical = lang_gc_program_enter_critical_v1();
    CHECK(critical == 1);
    lang_gc_program_leave_critical_v1(critical);
  }
  lang_vec_release_v1(target);
}

int mode_pending_stop() {
  CHECK(lang_gc_program_init_v1() == LANG_GC_PROGRAM_OK);
  const uint64_t base = gc_test_alloc_live();
  LangVec *a = new_inner();
  LangVec *b = new_inner();
  LangVec *c = new_inner();
  g_pending_b = b;
  g_pending_c = c;
  g_pending_outer =
      lang_vec_new_v1(2, sizeof(LangVec *), pending_copy, pending_drop);
  lang_vec_init_v1(g_pending_outer, &a);
  lang_vec_init_v1(g_pending_outer, &b);

  std::atomic<bool> gate{false};
  std::thread stopper([&] {
    CHECK(lang_gc_world_register_v1());
    while (!gate.load(std::memory_order_acquire))
      std::this_thread::yield();
    CHECK(lang_gc_world_stop_v1(5000000000LL) == 0);
    // The main thread is parked here, after publishing the complete new
    // element and before dropping the old one.
    CHECK(g_pending_outer->length == 2);
    CHECK(g_pending_outer->initialized == 2);
    CHECK(slot_value(g_pending_outer, 0) == c);
    CHECK(slot_value(g_pending_outer, 1) == b);
    CHECK(g_pending_drops.load(std::memory_order_relaxed) == 0);
    lang_gc_world_resume_v1();
    CHECK(lang_gc_world_unregister_v1());
  });

  gate.store(true, std::memory_order_release);
  // Spin on the exported request word without taking a safepoint. Once the
  // request is pending, the setter enters its short critical publish window.
  while (!lang_gc_world_request_pending_v1())
    std::this_thread::yield();
  lang_vec_set_v1(g_pending_outer, 0, &c);
  stopper.join();

  CHECK(g_pending_drops.load(std::memory_order_relaxed) == 1);
  CHECK(slot_value(g_pending_outer, 0) == c);
  CHECK(slot_value(g_pending_outer, 1) == b);
  lang_vec_release_v1(g_pending_outer);
  CHECK(g_pending_drops.load(std::memory_order_relaxed) == 3);
  CHECK(gc_test_alloc_live() == base);
  CHECK(lang_gc_program_finish_v1() == LANG_GC_PROGRAM_OK);
  std::puts("VEC_GRAPH_PENDING_STOP_OK");
  return 0;
}

// ---------------------------------------------------------------------------
// 9. Copy callbacks receive an independent raw snapshot. A callback may set
//    the source slot to a different ARC object; the result must own the
//    originally retained snapshot, not a later re-read of the source.

LangVec *g_snapshot_source = nullptr;
LangVec *g_snapshot_replacement = nullptr;
uint64_t g_snapshot_copies = 0;
uint64_t g_snapshot_drops = 0;
bool g_snapshot_mutate = false;

void snapshot_copy(void *slot) noexcept {
  LangVec *snapshot = *static_cast<LangVec **>(slot);
  CHECK(snapshot != nullptr);
  lang_vec_retain_v1(snapshot);
  ++g_snapshot_copies;
  int32_t safe = lang_gc_program_enter_safe_v1();
  CHECK(safe == 1);
  lang_gc_program_leave_safe_v1(safe);
  if (g_snapshot_mutate) {
    g_snapshot_mutate = false;
    lang_vec_set_v1(g_snapshot_source, 0, &g_snapshot_replacement);
  }
}

void snapshot_drop(void *slot) noexcept {
  LangVec *target = *static_cast<LangVec **>(slot);
  ++g_snapshot_drops;
  lang_vec_release_v1(target);
}

int mode_slice_copy_snapshot() {
  CHECK(lang_gc_program_init_v1() == LANG_GC_PROGRAM_OK);
  const uint64_t base = gc_test_alloc_live();
  LangVec *original = new_inner();
  LangVec *replacement = new_inner();
  g_snapshot_source =
      lang_vec_new_v1(1, sizeof(LangVec *), snapshot_copy, snapshot_drop);
  lang_vec_init_v1(g_snapshot_source, &original);
  g_snapshot_replacement = replacement;
  g_snapshot_mutate = true;

  LangVec *slice = lang_vec_slice_v1(g_snapshot_source, 0, 1);
  CHECK(g_snapshot_copies == 1);
  CHECK(slot_value(slice, 0) == original);
  CHECK(original->refs == 1);
  CHECK(replacement->refs == 1);
  CHECK(g_snapshot_drops == 1); // source set dropped the old original owner

  // Release the source before reading the result. The result owns the
  // retained original snapshot, so ASAN must not see a stale source read.
  lang_vec_release_v1(g_snapshot_source);
  CHECK(g_snapshot_drops == 2);
  CHECK(original->refs == 1);
  CHECK(slot_value(slice, 0) == original);
  lang_vec_release_v1(slice);
  CHECK(g_snapshot_drops == 3);
  CHECK(gc_test_alloc_live() == base);
  CHECK(lang_gc_program_finish_v1() == LANG_GC_PROGRAM_OK);
  std::puts("VEC_GRAPH_SLICE_COPY_SNAPSHOT_OK");
  return 0;
}

int mode_resize_fill_alias_snapshot() {
  CHECK(lang_gc_program_init_v1() == LANG_GC_PROGRAM_OK);
  const uint64_t base = gc_test_alloc_live();
  LangVec *original = new_inner();
  LangVec *replacement = new_inner();
  g_snapshot_source =
      lang_vec_new_v1(1, sizeof(LangVec *), snapshot_copy, snapshot_drop);
  lang_vec_init_v1(g_snapshot_source, &original);
  g_snapshot_replacement = replacement;
  g_snapshot_mutate = true;
  LangVec *target =
      lang_vec_new_v1(0, sizeof(LangVec *), snapshot_copy, snapshot_drop);

  lang_vec_resize_v1(target, 1, lang_vec_at_v1(g_snapshot_source, 0));
  CHECK(g_snapshot_copies == 1);
  CHECK(slot_value(target, 0) == original);
  CHECK(original->refs == 1);
  CHECK(replacement->refs == 1);
  CHECK(g_snapshot_drops == 1);

  lang_vec_release_v1(g_snapshot_source);
  CHECK(g_snapshot_drops == 2);
  CHECK(original->refs == 1);
  CHECK(slot_value(target, 0) == original);
  lang_vec_release_v1(target);
  CHECK(g_snapshot_drops == 3);
  CHECK(gc_test_alloc_live() == base);
  CHECK(lang_gc_program_finish_v1() == LANG_GC_PROGRAM_OK);
  std::puts("VEC_GRAPH_RESIZE_FILL_ALIAS_SNAPSHOT_OK");
  return 0;
}

int mode_self_set_pre_copied() {
  CHECK(lang_gc_program_init_v1() == LANG_GC_PROGRAM_OK);
  const uint64_t base = gc_test_alloc_live();
  LangVec *original = new_inner();
  g_snapshot_source =
      lang_vec_new_v1(1, sizeof(LangVec *), snapshot_copy, snapshot_drop);
  lang_vec_init_v1(g_snapshot_source, &original);

  // The caller has already retained one pre-copied owner for the transfer.
  lang_vec_retain_v1(original);
  CHECK(original->refs == 2);
  lang_vec_set_v1(g_snapshot_source, 0,
                  lang_vec_at_v1(g_snapshot_source, 0));
  CHECK(original->refs == 1);
  CHECK(g_snapshot_drops == 1); // exactly one old-owner drop
  CHECK(g_snapshot_copies == 0); // set is a transfer, not a logical copy
  CHECK(slot_value(g_snapshot_source, 0) == original);

  lang_vec_release_v1(g_snapshot_source);
  CHECK(g_snapshot_drops == 2);
  CHECK(gc_test_alloc_live() == base);
  CHECK(lang_gc_program_finish_v1() == LANG_GC_PROGRAM_OK);
  std::puts("VEC_GRAPH_SELF_SET_PRE_COPIED_OK");
  return 0;
}

// ---------------------------------------------------------------------------
// 10. Default disabled path: no gettid, no registry, owning Vec still correct.

uint64_t g_fast_copies = 0;
uint64_t g_fast_drops = 0;

void fast_copy(void *) noexcept { ++g_fast_copies; }
void fast_drop(void *) noexcept { ++g_fast_drops; }

int mode_disabled_fast() {
  CHECK(!enabled());
  CHECK(deny_gettid());
  CHECK(!enabled());

  LangVec *vec = lang_vec_new_v1(2, sizeof(uint64_t), fast_copy, fast_drop);
  uint64_t first = 1;
  uint64_t second = 2;
  lang_vec_init_v1(vec, &first);
  lang_vec_init_v1(vec, &second);
  uint64_t replacement = 9;
  lang_vec_set_v1(vec, 0, &replacement);
  uint64_t pushed = 3;
  lang_vec_push_v1(vec, &pushed);
  uint64_t fill = 7;
  lang_vec_resize_v1(vec, 5, &fill);
  lang_vec_resize_v1(vec, 3, nullptr);
  LangVec *slice = lang_vec_slice_v1(vec, 1, 3);
  lang_vec_release_v1(slice);
  lang_vec_clear_v1(vec);
  lang_vec_release_v1(vec);

  CHECK(g_fast_copies == 4);
  CHECK(g_fast_drops == 8);
  CHECK(!enabled());
  std::puts("VEC_GRAPH_DISABLED_FAST_OK");
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <mode>\n", argv[0]);
    return 2;
  }
  if (std::strcmp(argv[1], "init-publish") == 0)
    return mode_init_publish();
  if (std::strcmp(argv[1], "set-reentrant") == 0)
    return mode_set_reentrant();
  if (std::strcmp(argv[1], "clear-reentrant") == 0)
    return mode_clear_reentrant();
  if (std::strcmp(argv[1], "shrink-reentrant") == 0)
    return mode_shrink_reentrant();
  if (std::strcmp(argv[1], "resize-copy-publish") == 0)
    return mode_resize_copy_publish();
  if (std::strcmp(argv[1], "selfalias-growth") == 0)
    return mode_selfalias_growth();
  if (std::strcmp(argv[1], "final-release") == 0)
    return mode_final_release();
  if (std::strcmp(argv[1], "pending-stop") == 0)
    return mode_pending_stop();
  if (std::strcmp(argv[1], "slice-copy-snapshot") == 0)
    return mode_slice_copy_snapshot();
  if (std::strcmp(argv[1], "resize-fill-alias-snapshot") == 0)
    return mode_resize_fill_alias_snapshot();
  if (std::strcmp(argv[1], "self-set-pre-copied") == 0)
    return mode_self_set_pre_copied();
  if (std::strcmp(argv[1], "disabled-fast") == 0)
    return mode_disabled_fast();
  std::fprintf(stderr, "unknown mode: %s\n", argv[1]);
  return 2;
}
