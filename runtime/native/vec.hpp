#pragma once
#include <cstdint>
// Private runtime ABI. Element callbacks operate on storage, so no C++
// aggregate calling convention is assumed by generated LLVM code.
extern "C" {
using LangValueOp = void (*)(void *);
struct LangVec {
  uint64_t refs, length, initialized, stride, iterations;
  void *data;
  LangValueOp copy, drop;
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
// Private runtime operation, not a source-language length-changing API.
void lang_vec_check_resize_v1(LangVec *) noexcept;
}
