#define UTF8PROC_STATIC 1
#include "unicode.hpp"
#include "vendor/utf8proc/utf8proc.h"
#include <cstddef>
#include <cstdint>

extern "C" [[noreturn]] void lang_native_panic_v1(uint32_t, uint64_t, uint64_t,
                                                   uint64_t) noexcept;

namespace {
constexpr int64_t kInvalid = -22;
constexpr int64_t kOverflow = -75;

[[noreturn]] void panic(uint32_t cause) noexcept {
  lang_native_panic_v1(cause, 0, 0, 0);
}

bool scalar(uint32_t rune) noexcept {
  return rune <= 0x10ffff && !(rune >= 0xd800 && rune <= 0xdfff);
}

// Output storage is uninitialized except for the documented in-place forms.
// The old reference is released only after the replacement is complete.
void store(LangText *out, LangText result, const LangText *input) noexcept {
  LangHandle *old = out == input ? out->owner : nullptr;
  *out = result;
  if (old) lang_handle_release_v1(old);
}

int64_t fail(LangText *out, const LangText *input, int64_t code) noexcept {
  LangHandle *old = out == input ? out->owner : nullptr;
  *out = {};
  if (old) lang_handle_release_v1(old);
  return code;
}

int64_t invalid(LangText *out, const LangText *input) noexcept {
  return fail(out, input, kInvalid);
}

int64_t transform(LangText *out, const LangText *input,
                  unsigned int options) noexcept {
  if (!out || !input) panic(10);
  if (input->bytes && !input->data) return invalid(out, input);
  if (input->bytes > static_cast<uint64_t>(PTRDIFF_MAX))
    return fail(out, input, kOverflow);
  if (!input->bytes) {
    store(out, {}, input);
    return 0;
  }

  utf8proc_uint8_t *mapped = nullptr;
  const utf8proc_ssize_t mapped_bytes = utf8proc_map(
      input->data, static_cast<utf8proc_ssize_t>(input->bytes), &mapped,
      options);
  if (mapped_bytes < 0) {
    if (mapped) utf8proc_free(mapped);
    if (mapped_bytes == UTF8PROC_ERROR_NOMEM) panic(5);
    if (mapped_bytes == UTF8PROC_ERROR_OVERFLOW) panic(6);
    return invalid(out, input);
  }

  LangText result{};
  const int64_t status = lang_text_from_utf8_v1(
      &result, mapped, static_cast<uint64_t>(mapped_bytes));
  if (mapped) utf8proc_free(mapped);
  if (status < 0) return invalid(out, input);
  store(out, result, input);
  return 0;
}

uint64_t scan_boundaries(const LangText &text, LangVec *output) noexcept {
  uint64_t count = 0;
  auto emit = [&](uint64_t position) {
    if (output) lang_vec_init_v1(output, &position);
    ++count;
  };

  emit(0);
  uint64_t offset = 0, index = 0;
  uint32_t previous = 0;
  utf8proc_int32_t state = 0;
  bool first = true;
  while (offset < text.bytes) {
    utf8proc_int32_t rune = 0;
    const utf8proc_ssize_t bytes = utf8proc_iterate(
        text.data + offset, static_cast<utf8proc_ssize_t>(text.bytes - offset),
        &rune);
    if (bytes <= 0 || static_cast<uint64_t>(bytes) > text.bytes - offset ||
        !scalar(static_cast<uint32_t>(rune)))
      panic(10);
    if (!first &&
        utf8proc_grapheme_break_stateful(static_cast<utf8proc_int32_t>(previous),
                                         rune, &state))
      emit(index);
    previous = static_cast<uint32_t>(rune);
    first = false;
    ++index;
    offset += static_cast<uint64_t>(bytes);
  }
  if (index != text.length) panic(10);
  if (index) emit(index);
  return count;
}
} // namespace

extern "C" int64_t lang_unicode_normalize_v1(LangText *out,
                                              const LangText *input,
                                              int32_t form) noexcept {
  if (!out || !input) panic(10);
  unsigned int options = UTF8PROC_STABLE;
  switch (form) {
  case LANG_UNICODE_NFC:
    options |= UTF8PROC_COMPOSE;
    break;
  case LANG_UNICODE_NFD:
    options |= UTF8PROC_DECOMPOSE;
    break;
  case LANG_UNICODE_NFKC:
    options |= UTF8PROC_COMPOSE | UTF8PROC_COMPAT;
    break;
  case LANG_UNICODE_NFKD:
    options |= UTF8PROC_DECOMPOSE | UTF8PROC_COMPAT;
    break;
  default:
    return invalid(out, input);
  }
  return transform(out, input, options);
}

extern "C" int64_t lang_unicode_casefold_v1(LangText *out,
                                             const LangText *input) noexcept {
  return transform(out, input, UTF8PROC_CASEFOLD);
}

extern "C" LangVec *lang_unicode_grapheme_boundaries_v1(
    const LangText *text) noexcept {
  if (!text) panic(10);
  if (text->bytes && !text->data) panic(10);
  if (text->bytes > static_cast<uint64_t>(PTRDIFF_MAX)) panic(10);
  const uint64_t count = scan_boundaries(*text, nullptr);
  auto *result = lang_vec_new_v1(count, sizeof(uint64_t), nullptr, nullptr);
  scan_boundaries(*text, result);
  return result;
}

extern "C" uint32_t lang_unicode_rune_to_upper_v1(uint32_t rune) noexcept {
  if (!scalar(rune)) return rune;
  return static_cast<uint32_t>(
      utf8proc_toupper(static_cast<utf8proc_int32_t>(rune)));
}

extern "C" uint32_t lang_unicode_rune_to_lower_v1(uint32_t rune) noexcept {
  if (!scalar(rune)) return rune;
  return static_cast<uint32_t>(
      utf8proc_tolower(static_cast<utf8proc_int32_t>(rune)));
}
