#include "slice.hpp"

extern "C" [[noreturn]] void lang_native_panic_v1(uint32_t, uint64_t,
                                                 uint64_t, uint64_t) noexcept;

// The borrowed-Slice ABI depends on the published LangVec layout.
static_assert(sizeof(LangVec) == 72, "LangVec ABI must stay 72 bytes");
static_assert(offsetof(LangVec, length) == 8, "LangVec length offset changed");
static_assert(offsetof(LangVec, initialized) == 16,
              "LangVec initialized offset changed");
static_assert(offsetof(LangVec, iterations) == 32,
              "LangVec iterations offset changed");

namespace {

constexpr uint64_t kMaxU64 = ~uint64_t{0};

[[noreturn]] void slice_panic(uint32_t cause) noexcept {
  lang_native_panic_v1(cause, 0, 0, 0);
}

// A zeroed LangSlice is the only owner-less state. It represents no borrow and
// can be copied, inspected, or dropped repeatedly without touching a Vec.
void require_blank_or_owner(const LangSlice &slice) noexcept {
  if (!slice.owner && (slice.start || slice.length)) slice_panic(10);
}

// start + length must not wrap, stay inside the Vec length, and (for a new
// borrowed view) cover only initialized elements.
void validate_range(LangVec *owner, uint64_t start, uint64_t length,
                    bool check_initialized) noexcept {
  if (!owner) slice_panic(10);
  if (start > kMaxU64 - length) slice_panic(9);
  const uint64_t end = start + length;
  if (end > owner->length) slice_panic(9);
  if (check_initialized && end > owner->initialized) slice_panic(4);
}

void validate_live(const LangSlice &slice, bool check_initialized) noexcept {
  require_blank_or_owner(slice);
  if (!slice.owner) return;
  validate_range(slice.owner, slice.start, slice.length, check_initialized);
}

// Pin after the strong retain. The caller must already synchronize structural
// Vec changes; rechecking against the pinned length makes an unsynchronized
// structural change fail rather than silently relocate the slice's data.
void retain_and_pin(LangVec *owner, uint64_t end, bool check_initialized) noexcept {
  lang_vec_retain_v1(owner);
  const uint64_t pinned_length = lang_vec_begin_v1(owner);
  if (end > pinned_length) {
    lang_vec_end_v1(owner);
    lang_vec_release_v1(owner);
    slice_panic(9);
  }
  if (check_initialized && end > owner->initialized) {
    lang_vec_end_v1(owner);
    lang_vec_release_v1(owner);
    slice_panic(4);
  }
}

} // namespace

extern "C" void lang_slice_new_v1(LangSlice *out, LangVec *owner,
                                  uint64_t start, uint64_t end) noexcept {
  if (!out || !owner) slice_panic(10);
  if (start > end) slice_panic(9);
  const uint64_t length = end - start;
  validate_range(owner, start, length, true);
  retain_and_pin(owner, end, true);
  out->owner = owner;
  out->start = start;
  out->length = length;
}

extern "C" void lang_slice_copy_v1(void *storage) noexcept {
  if (!storage) slice_panic(10);
  auto &slice = *static_cast<LangSlice *>(storage);
  require_blank_or_owner(slice);
  if (!slice.owner) return;

  const uint64_t start = slice.start;
  const uint64_t length = slice.length;
  validate_range(slice.owner, start, length, true);
  retain_and_pin(slice.owner, start + length, true);
}

extern "C" void lang_slice_drop_v1(void *storage) noexcept {
  if (!storage) slice_panic(10);
  auto &slice = *static_cast<LangSlice *>(storage);
  LangVec *owner = slice.owner;
  const uint64_t start = slice.start;
  const uint64_t length = slice.length;
  slice.owner = nullptr;
  slice.start = 0;
  slice.length = 0;

  if (!owner) {
    if (start || length) slice_panic(10);
    return;
  }
  validate_range(owner, start, length, true);
  // End the pin before the final release: final release rejects active
  // iterations, and element drop callbacks must no longer see this pin.
  lang_vec_end_v1(owner);
  lang_vec_release_v1(owner);
}

extern "C" void lang_slice_sub_v1(LangSlice *out, const LangSlice *source,
                                  uint64_t start, uint64_t end) noexcept {
  if (!out || !source) slice_panic(10);
  LangVec *owner = source->owner;
  const uint64_t base_start = source->start;
  const uint64_t base_length = source->length;

  if (!owner) {
    if (base_start || base_length) slice_panic(10);
    if (start > end || end > base_length) slice_panic(9);
    out->owner = nullptr;
    out->start = 0;
    out->length = 0;
    return;
  }

  validate_range(owner, base_start, base_length, true);
  if (start > end || end > base_length) slice_panic(9);
  // Check the absolute endpoint before adding: a malformed source must not
  // make start + index wrap into a valid-looking Vec element.
  if (base_start > kMaxU64 - start || base_start > kMaxU64 - end)
    slice_panic(9);
  const uint64_t absolute_start = base_start + start;
  const uint64_t absolute_end = base_start + end;
  if (absolute_end < absolute_start) slice_panic(9);
  if (out == source) {
    // Replace an in-place source without leaking its old pin/reference. The
    // new view is fully pinned before the old view is released.
    LangSlice result{};
    lang_slice_new_v1(&result, owner, absolute_start, absolute_end);
    lang_vec_end_v1(owner);
    lang_vec_release_v1(owner);
    *out = result;
    return;
  }
  lang_slice_new_v1(out, owner, absolute_start, absolute_end);
}

extern "C" void *lang_slice_at_v1(const LangSlice *slice,
                                  uint64_t index) noexcept {
  if (!slice) slice_panic(10);
  LangVec *owner = slice->owner;
  if (!owner) {
    if (slice->start || slice->length) slice_panic(10);
    // A blank slice has no elements, so every index is out of bounds.
    slice_panic(4);
  }
  if (index >= slice->length) slice_panic(4);
  if (slice->start > kMaxU64 - index) slice_panic(9);
  // lang_vec_at_v1 rechecks the absolute length and initialized prefix and is
  // the only producer of the typed backend's slot pointer.
  return lang_vec_at_v1(owner, slice->start + index);
}

extern "C" uint64_t lang_slice_length_v1(const LangSlice *slice) noexcept {
  if (!slice) slice_panic(10);
  validate_live(*slice, false);
  return slice->length;
}
