#pragma once
#include <cstdint>
// Private runtime ABI. Element callbacks operate on storage, so no C++
// aggregate calling convention is assumed by generated LLVM code.
extern "C" {
// Null copy/drop means bit-copy/no element cleanup (trivial elements).
using LangValueOp = void (*)(void *);
struct LangVec {
  // Element bytes never overlap a live LangVec descriptor. Payloads of
  // aliased Vec values may still share element storage.
  // refs and iterations use atomic builtins without changing this layout.
  // Retain requires an existing live reference. Element access and structural
  // updates still require caller synchronization.
  uint64_t refs, length, initialized, stride, iterations;
  void *data;
  LangValueOp copy, drop;
  // Private ABI extension. Appending capacity keeps every original field at
  // its previous offset. New Vecs start with capacity == length.
  uint64_t capacity;
};
LangVec *lang_vec_new_v1(uint64_t, uint64_t, LangValueOp, LangValueOp) noexcept;
void lang_vec_retain_v1(LangVec *) noexcept;
void lang_vec_release_v1(LangVec *) noexcept;
void lang_vec_init_v1(LangVec *, const void *) noexcept;
void *lang_vec_at_v1(LangVec *, uint64_t) noexcept;
void lang_vec_set_v1(LangVec *, uint64_t, const void *) noexcept;
LangVec *lang_vec_slice_v1(LangVec *, uint64_t, uint64_t) noexcept;
uint64_t lang_vec_begin_v1(LangVec *) noexcept;
void lang_vec_end_v1(LangVec *) noexcept;
uint64_t lang_vec_length_v1(LangVec *) noexcept;
uint64_t lang_vec_capacity_v1(LangVec *) noexcept;
// Structural operations require caller synchronization and no active
// iterations whenever length or storage changes.
void lang_vec_reserve_v1(LangVec *, uint64_t minimum) noexcept;
// Like init/set, push transfers one pre-copied value into the container.
void lang_vec_push_v1(LangVec *, const void *value) noexcept;
bool lang_vec_pop_v1(LangVec *, void *out) noexcept;
void lang_vec_resize_v1(LangVec *, uint64_t length, const void *fill) noexcept;
void lang_vec_clear_v1(LangVec *) noexcept;
// Private runtime operation, not a source-language length-changing API.
void lang_vec_check_resize_v1(LangVec *) noexcept;
void *lang_alloc_v1(uint64_t, uint64_t) noexcept;
void lang_free_v1(void *) noexcept;
}
