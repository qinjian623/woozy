#pragma once
#include "handle.hpp"
#include "vec.hpp"
#include <cstdint>

struct LangText {
  const uint8_t *data;
  uint64_t bytes;
  uint64_t length;
  LangHandle *owner;
};

// Inputs contain valid UTF-8 and an exact Unicode scalar count. Literal
// storage has no owner. Outputs own one reference and need no NUL terminator.
// Output storage is uninitialized, except concat/slice may replace an input
// descriptor in place; that case releases the overwritten reference.
extern "C" {
void lang_text_concat_v1(LangText *out, const LangText *a,
                         const LangText *b) noexcept;
int32_t lang_text_compare_v1(const LangText *a, const LangText *b) noexcept;
uint32_t lang_text_at_v1(const LangText *text, uint64_t index) noexcept;
void lang_text_slice_v1(LangText *out, const LangText *text,
                        uint64_t start, uint64_t end) noexcept;
// Invalid UTF-8/scalars return -EINVAL and initialize out to the empty string.
int64_t lang_text_from_bytes_v1(LangText *out, const LangVec *bytes) noexcept;
int64_t lang_text_from_utf8_v1(LangText *out, const uint8_t *data, uint64_t bytes) noexcept;
int64_t lang_text_from_rune_v1(LangText *out, uint32_t rune) noexcept;
LangVec *lang_text_bytes_v1(const LangText *text) noexcept;
}
