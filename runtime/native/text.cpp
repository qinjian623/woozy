#include "text.hpp"
#include <cstddef>

extern "C" [[noreturn]] void lang_native_panic_v1(uint32_t, uint64_t,
                                                 uint64_t, uint64_t) noexcept;
static_assert(sizeof(LangText) == 32 && alignof(LangText) == 8 &&
              offsetof(LangText, data) == 0 && offsetof(LangText, bytes) == 8 &&
              offsetof(LangText, length) == 16 && offsetof(LangText, owner) == 24);

namespace {
constexpr int64_t kInvalid = -22;
constexpr auto kTextKind = LangHandleKind::text;
[[noreturn]] void panic(uint32_t cause) noexcept {
  lang_native_panic_v1(cause, 0, 0, 0);
}
void destroy(LangHandle *owner) noexcept { lang_free_v1(owner); }
LangText allocate(uint64_t bytes, uint64_t length) noexcept {
  if (!bytes) return {};
  if (bytes > ~uint64_t{0} - sizeof(LangHandle)) panic(6);
  auto *owner = static_cast<LangHandle *>(
      lang_alloc_v1(sizeof(LangHandle) + bytes, alignof(LangHandle)));
  if (!owner) panic(5);
  *owner = {1, kTextKind, destroy, nullptr};
  return {reinterpret_cast<const uint8_t *>(owner + 1), bytes, length, owner};
}
void copy(uint8_t *to, const uint8_t *from, uint64_t bytes) noexcept {
  auto *destination = static_cast<volatile uint8_t *>(to);
  const auto *source = static_cast<const volatile uint8_t *>(from);
  for (uint64_t i = 0; i < bytes; ++i) destination[i] = source[i];
}
bool scalar(uint32_t rune) noexcept {
  return rune <= 0x10ffff && !(rune >= 0xd800 && rune <= 0xdfff);
}
bool decode(const uint8_t *data, uint64_t bytes, uint64_t &offset,
            uint32_t &rune) noexcept {
  if (offset >= bytes) return false;
  const uint8_t first = data[offset];
  uint32_t count;
  if (first < 0x80) { rune = first; ++offset; return true; }
  if (first >= 0xc2 && first <= 0xdf) { count = 2; rune = first & 0x1f; }
  else if (first >= 0xe0 && first <= 0xef) { count = 3; rune = first & 0x0f; }
  else if (first >= 0xf0 && first <= 0xf4) { count = 4; rune = first & 0x07; }
  else return false;
  if (count > bytes - offset) return false;
  for (uint32_t i = 1; i < count; ++i) {
    const uint8_t byte = data[offset + i];
    if ((byte & 0xc0) != 0x80) return false;
    rune = (rune << 6) | (byte & 0x3f);
  }
  if ((count == 2 && rune < 0x80) || (count == 3 && rune < 0x800) ||
      (count == 4 && rune < 0x10000) || !scalar(rune)) return false;
  offset += count;
  return true;
}
bool count(const uint8_t *data, uint64_t bytes, uint64_t &length) noexcept {
  length = 0;
  for (uint64_t offset = 0; offset < bytes; ++length) {
    uint32_t rune;
    if (!decode(data, bytes, offset, rune)) return false;
  }
  return true;
}
uint64_t offset_for(const LangText &text, uint64_t index) noexcept {
  // Equal byte and scalar counts imply that this valid UTF-8 is ASCII.
  if (text.bytes == text.length) return index;
  uint64_t offset = 0;
  for (uint64_t i = 0; i < index; ++i) {
    uint32_t rune;
    if (!decode(text.data, text.bytes, offset, rune)) panic(10);
  }
  return offset;
}
void output(LangText *out, LangText result, const LangText *a,
            const LangText *b = nullptr) noexcept {
  auto *old = (out == a || out == b) ? out->owner : nullptr;
  *out = result;
  if (old) lang_handle_release_v1(old);
}
} // namespace

extern "C" void lang_text_concat_v1(LangText *out, const LangText *a,
                                     const LangText *b) noexcept {
  if (!out || !a || !b) panic(10);
  if (a->bytes > ~uint64_t{0} - b->bytes ||
      a->length > ~uint64_t{0} - b->length) panic(6);
  LangText result;
  if (!a->bytes || !b->bytes) {
    result = a->bytes ? *a : *b;
    if (!result.bytes) result = {};
    if (result.owner) lang_handle_retain_v1(result.owner);
  } else {
    result = allocate(a->bytes + b->bytes, a->length + b->length);
    auto *data = const_cast<uint8_t *>(result.data);
    copy(data, a->data, a->bytes);
    copy(data + a->bytes, b->data, b->bytes);
  }
  output(out, result, a, b);
}
extern "C" int32_t lang_text_compare_v1(const LangText *a,
                                         const LangText *b) noexcept {
  if (!a || !b) panic(10);
  const uint64_t bytes = a->bytes < b->bytes ? a->bytes : b->bytes;
  for (uint64_t i = 0; i < bytes; ++i)
    if (a->data[i] != b->data[i]) return a->data[i] < b->data[i] ? -1 : 1;
  return a->bytes == b->bytes ? 0 : a->bytes < b->bytes ? -1 : 1;
}
extern "C" uint32_t lang_text_at_v1(const LangText *text,
                                     uint64_t index) noexcept {
  if (!text) panic(10);
  if (index >= text->length) panic(4);
  if (text->bytes == text->length) return text->data[index];
  auto offset = offset_for(*text, index);
  uint32_t rune;
  if (!decode(text->data, text->bytes, offset, rune)) panic(10);
  return rune;
}
extern "C" void lang_text_slice_v1(LangText *out, const LangText *text,
                                    uint64_t start, uint64_t end) noexcept {
  if (!out || !text) panic(10);
  if (start > end || end > text->length) panic(9);
  LangText result{};
  if (start != end) {
    const auto first = offset_for(*text, start), last = offset_for(*text, end);
    result = {text->data + first, last - first, end - start, text->owner};
    if (result.owner) lang_handle_retain_v1(result.owner);
  }
  output(out, result, text);
}
extern "C" void lang_unicode_text_take_v1(LangText *out, const LangText *text,
                                           LangVec *cursor, uint64_t count) noexcept {
  if (!out || !text || !cursor) panic(10);
  if (cursor->stride != sizeof(uint64_t) || cursor->length != 1 ||
      cursor->initialized != 1 || !cursor->data) panic(10);
  auto *state = static_cast<uint64_t *>(cursor->data);
  const uint64_t start = *state;
  LangText result{};
  uint64_t end = start;
  if (text->bytes == text->length) {
    // ASCII: code-point and byte offsets coincide.
    if (start > text->length || count > text->length - start) panic(9);
    end = start + count;
  } else {
    if (start > text->bytes) panic(9);
    for (uint64_t i = 0; i < count; ++i) {
      if (end >= text->bytes) panic(9);
      uint32_t rune;
      if (!decode(text->data, text->bytes, end, rune)) panic(10);
    }
  }
  if (end != start) {
    result = {text->data + start, end - start, count, text->owner};
    if (result.owner) lang_handle_retain_v1(result.owner);
    *state = end;
  }
  output(out, result, text);
}
extern "C" int64_t lang_text_from_utf8_v1(LangText *out,
                                            const uint8_t *data,
                                            uint64_t bytes) noexcept {
  if (!out) panic(10);
  *out = {};
  if (bytes && !data) return kInvalid;
  auto result = allocate(bytes, 0);
  copy(const_cast<uint8_t *>(result.data), data, bytes);
  if (!count(result.data, result.bytes, result.length)) {
    if (result.owner) lang_handle_release_v1(result.owner);
    return kInvalid;
  }
  *out = result;
  return 0;
}
extern "C" int64_t lang_text_from_bytes_v1(LangText *out,
                                             const LangVec *bytes) noexcept {
  if (!out) panic(10);
  *out = {};
  if (!bytes || bytes->stride != 1 || bytes->initialized != bytes->length ||
      bytes->copy || bytes->drop || (bytes->length && !bytes->data)) return kInvalid;
  return lang_text_from_utf8_v1(out, static_cast<const uint8_t *>(bytes->data), bytes->length);
}
extern "C" int64_t lang_text_from_rune_v1(LangText *out,
                                            uint32_t rune) noexcept {
  if (!out) panic(10);
  *out = {};
  if (!scalar(rune)) return kInvalid;
  const uint64_t bytes = rune < 0x80 ? 1 : rune < 0x800 ? 2 : rune < 0x10000 ? 3 : 4;
  auto result = allocate(bytes, 1);
  auto *data = const_cast<uint8_t *>(result.data);
  if (bytes == 1) data[0] = static_cast<uint8_t>(rune);
  else {
    uint32_t value = rune;
    for (uint64_t i = bytes - 1; i; --i) {
      data[i] = static_cast<uint8_t>(0x80 | (value & 0x3f));
      value >>= 6;
    }
    data[0] = static_cast<uint8_t>((bytes == 2 ? 0xc0 : bytes == 3 ? 0xe0 : 0xf0) | value);
  }
  *out = result;
  return 0;
}
extern "C" LangVec *lang_text_bytes_v1(const LangText *text) noexcept {
  if (!text) panic(10);
  auto *bytes = lang_vec_new_v1(text->bytes, 1, nullptr, nullptr);
  copy(static_cast<uint8_t *>(bytes->data), text->data, text->bytes);
  bytes->initialized = text->bytes;
  return bytes;
}
