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
uint32_t scalar_width(uint32_t rune) noexcept {
  return rune < 0x80 ? 1 : rune < 0x800 ? 2 : rune < 0x10000 ? 3 : 4;
}
void write_scalar(uint8_t *data, uint32_t rune, uint32_t width) noexcept {
  if (width == 1) {
    data[0] = static_cast<uint8_t>(rune);
    return;
  }
  for (auto i = width - 1; i != 0; --i) {
    data[i] = static_cast<uint8_t>(0x80 | (rune & 0x3f));
    rune >>= 6;
  }
  data[0] = static_cast<uint8_t>((width == 2 ? 0xc0 : width == 3 ? 0xe0 : 0xf0) | rune);
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
  // Inputs are checked text: valid UTF-8 with an exact scalar count, so the
  // scalars counted by length end exactly at bytes. Callers bound index by
  // length, so a suffix end is a constant-time lookup instead of a rescan.
  if (index == text.length) return text.bytes;
  // Past the midpoint the shorter side is the tail, so count scalars back
  // from the byte end instead of decoding index scalars from the front.
  // Each step skips a scalar's continuation bytes to its leading byte.
  if (index > text.length / 2) {
    // Callers bound index by length; keep the step count from wrapping if a
    // descriptor violates that precondition.
    if (index > text.length) panic(10);
    uint64_t remaining = text.length - index;
    uint64_t offset = text.bytes;
    while (remaining != 0) {
      if (!offset) panic(10);
      --offset;
      while ((text.data[offset] & 0xc0) == 0x80) {
        if (!offset) panic(10);
        --offset;
      }
      --remaining;
    }
    return offset;
  }
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
bool cursor_boundary(const LangText &text, uint64_t offset) noexcept {
  return offset <= text.bytes &&
      (offset == text.bytes || (text.data[offset] & 0xc0) != 0x80);
}
LangText cursor_view(const LangText &text, uint64_t start, uint64_t end,
                     uint64_t length) noexcept {
  if (start == end) return {};
  if (text.owner) lang_handle_retain_v1(text.owner);
  return {text.data + start, end - start, length, text.owner};
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
  if (start == end) return output(out, {}, text);
  const auto first = offset_for(*text, start), last = offset_for(*text, end);
  output(out, cursor_view(*text, first, last, end - start), text);
}
extern "C" uint64_t lang_text_cursor_at_v1(const LangText *text,
                                            uint64_t index) noexcept {
  if (!text) panic(10);
  if (index > text->length) panic(9);
  return offset_for(*text, index);
}
extern "C" uint64_t lang_text_cursor_read_v1(const LangText *text,
                                              uint64_t cursor) noexcept {
  if (!text) panic(10);
  if (cursor >= text->bytes) panic(4);
  uint32_t rune;
  auto next = cursor;
  if (!decode(text->data, text->bytes, next, rune)) panic(4);
  return (static_cast<uint64_t>(rune) << 32) | (next - cursor);
}
extern "C" void lang_text_cursor_slice_v1(LangText *out, const LangText *text,
                                           uint64_t start, uint64_t end) noexcept {
  if (!out || !text) panic(10);
  if (start > end || !cursor_boundary(*text, start) ||
      !cursor_boundary(*text, end)) panic(9);
  uint64_t length = end - start;
  if (text->bytes != text->length) {
    // The source is already validated UTF-8. Count leading bytes to form an
    // exact descriptor without revalidating or copying the substring.
    for (auto offset = start; offset < end; ++offset)
      length -= (text->data[offset] & 0xc0) == 0x80;
  }
  output(out, cursor_view(*text, start, end, length), text);
}
extern "C" uint64_t lang_text_count_rune_v1(const LangText *text,
                                              uint32_t rune) noexcept {
  if (!text || !scalar(rune)) panic(10);
  uint64_t matches = 0;
  if (rune < 0x80) {
    // An ASCII byte can only represent a complete scalar in valid UTF-8.
    for (uint64_t offset = 0; offset < text->bytes; ++offset)
      matches += text->data[offset] == static_cast<uint8_t>(rune);
  } else {
    const auto width = scalar_width(rune);
    if (width > text->bytes) return 0;
    uint8_t encoded[4];
    write_scalar(encoded, rune, width);
    // A valid leading byte cannot match a continuation byte. Equality over
    // this bounded window therefore implies one complete matching scalar.
    const auto last = text->bytes - width;
    for (uint64_t offset = 0; offset <= last; ++offset) {
      if (text->data[offset] != encoded[0]) continue;
      bool equal = true;
      for (uint32_t i = 1; i < width; ++i)
        equal &= text->data[offset + i] == encoded[i];
      matches += equal;
    }
  }
  return matches;
}
extern "C" void lang_unicode_text_take_v1(LangText *out, const LangText *text,
                                           LangVec *cursor, uint64_t count) noexcept {
  if (!out || !text || !cursor) panic(10);
  if (cursor->stride != sizeof(uint64_t) || cursor->length != 1 ||
      cursor->initialized != 1 || !cursor->data) panic(10);
  auto *state = static_cast<uint64_t *>(cursor->data);
  const uint64_t start = *state;
  uint64_t end = start;
  if (text->bytes == text->length) {
    // ASCII: code-point and byte offsets coincide.
    if (start > text->length || count > text->length - start) panic(9);
    end = start + count;
  } else {
    if (!cursor_boundary(*text, start)) panic(9);
    for (uint64_t i = 0; i < count; ++i) {
      if (end >= text->bytes) panic(9);
      uint32_t rune;
      if (!decode(text->data, text->bytes, end, rune)) panic(10);
    }
  }
  *state = end;
  output(out, cursor_view(*text, start, end, count), text);
}
extern "C" int64_t lang_text_from_utf8_v1(LangText *out,
                                            const uint8_t *data,
                                            uint64_t bytes) noexcept {
  if (!out) panic(10);
  *out = {};
  if (bytes && !data) return kInvalid;
  auto result = allocate(bytes, 0);
  auto *destination = const_cast<uint8_t *>(result.data);
  constexpr uint64_t kHighBits = 0x8080808080808080ULL;
  bool ascii = true;
  uint64_t offset = 0;
  for (; bytes - offset >= sizeof(uint64_t); offset += sizeof(uint64_t)) {
    uint64_t word;
    __builtin_memcpy(&word, data + offset, sizeof word);
    __builtin_memcpy(destination + offset, &word, sizeof word);
    ascii &= (word & kHighBits) == 0;
  }
  for (; offset < bytes; ++offset) {
    const uint8_t byte = data[offset];
    destination[offset] = byte;
    ascii &= byte < 0x80;
  }
  if (ascii) {
    result.length = bytes;
  } else if (!count(result.data, result.bytes, result.length)) {
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
  const auto width = scalar_width(rune);
  auto result = allocate(width, 1);
  write_scalar(const_cast<uint8_t *>(result.data), rune, width);
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
