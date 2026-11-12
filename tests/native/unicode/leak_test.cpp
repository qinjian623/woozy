#include "../../../runtime/native/unicode/unicode.hpp"
#include <cstdint>

// Link with -Wl,--wrap=lang_alloc_v1 -Wl,--wrap=lang_free_v1. Every ordinary
// runtime allocation must be released, including utf8proc's temporary map
// buffer. This test is also linked with -nostdlib -static.
namespace {
uint64_t allocations;
uint64_t frees;

void release(LangText &text) {
  if (text.owner) lang_handle_release_v1(text.owner);
  text = {};
}

LangText make(const unsigned char *bytes, uint64_t length) {
  LangText result{};
  if (lang_text_from_utf8_v1(&result, bytes, length) != 0) return {};
  return result;
}

bool balanced() {
  return allocations == frees;
}
} // namespace

extern "C" void *__real_lang_alloc_v1(uint64_t, uint64_t);
extern "C" void __real_lang_free_v1(void *);

extern "C" void *__wrap_lang_alloc_v1(uint64_t size, uint64_t align) {
  void *result = __real_lang_alloc_v1(size, align);
  if (result) ++allocations;
  return result;
}

extern "C" void __wrap_lang_free_v1(void *pointer) {
  if (pointer) ++frees;
  __real_lang_free_v1(pointer);
}

extern "C" int main() {
  {
    const unsigned char decomposed[] = {'e', 0xcc, 0x81};
    auto input = make(decomposed, sizeof(decomposed));
    LangText output{};
    if (!input.owner || lang_unicode_normalize_v1(&output, &input, LANG_UNICODE_NFC))
      return 1;
    release(output);
    release(input);
    if (!balanced()) return 2;
  }
  {
    const uint64_t before_allocations = allocations;
    const uint64_t before_frees = frees;
    LangText empty{};
    LangText output{};
    if (lang_unicode_normalize_v1(&output, &empty, LANG_UNICODE_NFC) ||
        lang_unicode_casefold_v1(&output, &empty))
      return 3;
    if (allocations != before_allocations || frees != before_frees) return 4;
  }
  {
    const unsigned char sharp_s[] = {0xc3, 0x9f};
    auto input = make(sharp_s, sizeof(sharp_s));
    LangText output{};
    if (!input.owner || lang_unicode_casefold_v1(&output, &input)) return 5;
    release(output);
    release(input);
    if (!balanced()) return 6;
  }
  {
    const unsigned char family[] = {0xf0, 0x9f, 0x91, 0xa8, 0xe2, 0x80, 0x8d,
                                    0xf0, 0x9f, 0x91, 0xa9, 0xe2, 0x80, 0x8d,
                                    0xf0, 0x9f, 0x91, 0xa7, 0xe2, 0x80, 0x8d,
                                    0xf0, 0x9f, 0x91, 0xa6};
    auto input = make(family, sizeof(family));
    auto *boundaries = lang_unicode_grapheme_boundaries_v1(&input);
    if (!input.owner || boundaries->length != 2) return 7;
    lang_vec_release_v1(boundaries);
    release(input);
    if (!balanced()) return 8;
  }
  {
    const unsigned char decomposed[] = {'e', 0xcc, 0x81};
    auto text = make(decomposed, sizeof(decomposed));
    if (!text.owner || lang_unicode_normalize_v1(&text, &text, LANG_UNICODE_NFC))
      return 9;
    release(text);
    if (!balanced()) return 10;
  }
  // If the linker wrap did not take effect, the balance checks would be empty.
  if (!allocations) return 11;
  return 0;
}
