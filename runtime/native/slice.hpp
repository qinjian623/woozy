#pragma once
#include <cstddef>
#include <cstdint>
#include "vec.hpp"

// Private borrowed-Slice ABI. A LangSlice does not own its elements: owner is
// a strong reference and the slice holds one begin/end iteration pin so that
// the Vec payload cannot be relocated or structurally shrunk while it lives.
// Element reads and stores still require caller synchronization. A bit-copy of
// a live LangSlice must be followed by lang_slice_copy_v1 before use.
extern "C" {
struct LangSlice {
  LangVec *owner;
  uint64_t start, length;
};
static_assert(sizeof(LangSlice) == 24, "LangSlice ABI must stay 24 bytes");
static_assert(alignof(LangSlice) == 8, "LangSlice alignment changed");
static_assert(offsetof(LangSlice, owner) == 0, "owner offset changed");
static_assert(offsetof(LangSlice, start) == 8, "start offset changed");
static_assert(offsetof(LangSlice, length) == 16, "length offset changed");

// Creates a new owned borrowed view over [start, end). Bounds and initialized
// checks run before the strong retain and iteration pin. Empty ranges still
// retain and pin their owner; a zeroed storage is the only empty drop state.
void lang_slice_new_v1(LangSlice *out, LangVec *owner, uint64_t start,
                       uint64_t end) noexcept;
// storage holds a bit-copy of a live LangSlice. Retains and pins that copy.
void lang_slice_copy_v1(void *storage) noexcept;
// Clears storage first, then ends its pin and releases its strong reference.
// A zeroed LangSlice may be dropped repeatedly; other malformed handles Panic.
void lang_slice_drop_v1(void *storage) noexcept;
// Creates an independently pinned sub-range relative to source.
void lang_slice_sub_v1(LangSlice *out, const LangSlice *source, uint64_t start,
                       uint64_t end) noexcept;
// Returns the owner slot for a typed backend. The pointer is not a general
// source-language raw pointer and must not be retained past the slice lifetime.
void *lang_slice_at_v1(const LangSlice *, uint64_t index) noexcept;
uint64_t lang_slice_length_v1(const LangSlice *) noexcept;
}
