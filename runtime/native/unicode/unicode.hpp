#pragma once
#include "../text.hpp"
#include "../vec.hpp"
#include <cstdint>

// Unicode 18.0.0 runtime entry points (utf8proc 2.12.0 data).
//
// Normalization and case folding consume the explicit byte length in LangText,
// so embedded NUL scalars are ordinary characters and are never treated as a
// terminator. Successful results own one text reference. Empty results have no
// owner and need no NUL terminator. Failures initialize out to the empty text.
// Allocation failures follow the existing runtime Panic(5)/Panic(6) rules.
enum LangUnicodeNormalizationForm : int32_t {
  LANG_UNICODE_NFC = 0,
  LANG_UNICODE_NFD = 1,
  LANG_UNICODE_NFKC = 2,
  LANG_UNICODE_NFKD = 3,
};

extern "C" {
// form is NFC=0, NFD=1, NFKC=2, NFKD=3. Returns 0 or -EINVAL.
int64_t lang_unicode_normalize_v1(LangText *out, const LangText *input,
                                  int32_t form) noexcept;
// Full default Unicode case folding (CaseFolding C + F mappings). This is not
// a locale-sensitive string case conversion. Returns 0 or -EINVAL.
int64_t lang_unicode_casefold_v1(LangText *out, const LangText *input) noexcept;
// Returns a new LangVec<usize> of code-point positions. The vector is non-empty:
// it always contains 0 and the code-point length, and an empty text yields [0].
LangVec *lang_unicode_grapheme_boundaries_v1(const LangText *text) noexcept;

// Simple one-code-point mappings from UnicodeData. They do not implement
// multi-code-point or locale-sensitive string case conversion.
uint32_t lang_unicode_rune_to_upper_v1(uint32_t rune) noexcept;
uint32_t lang_unicode_rune_to_lower_v1(uint32_t rune) noexcept;
}
