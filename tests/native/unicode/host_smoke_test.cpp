#include "../../../runtime/native/unicode/unicode.hpp"
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void need(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

LangText make(const std::string &bytes) {
  LangText result{};
  const auto status = lang_text_from_utf8_v1(
      &result, reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size());
  need(status == 0, "valid UTF-8 rejected");
  return result;
}

void drop(LangText &text) {
  if (text.owner) lang_handle_release_v1(text.owner);
  text = {};
}

std::string bytes(const LangText &text) {
  return std::string(reinterpret_cast<const char *>(text.data), text.bytes);
}

std::string encode(std::initializer_list<uint32_t> runes) {
  std::string result;
  for (auto rune : runes) {
    need(rune <= 0x10ffff && !(rune >= 0xd800 && rune <= 0xdfff),
         "invalid smoke-test rune");
    if (rune < 0x80) {
      result.push_back(static_cast<char>(rune));
    } else if (rune < 0x800) {
      result.push_back(static_cast<char>(0xc0 | (rune >> 6)));
      result.push_back(static_cast<char>(0x80 | (rune & 0x3f)));
    } else if (rune < 0x10000) {
      result.push_back(static_cast<char>(0xe0 | (rune >> 12)));
      result.push_back(static_cast<char>(0x80 | ((rune >> 6) & 0x3f)));
      result.push_back(static_cast<char>(0x80 | (rune & 0x3f)));
    } else {
      result.push_back(static_cast<char>(0xf0 | (rune >> 18)));
      result.push_back(static_cast<char>(0x80 | ((rune >> 12) & 0x3f)));
      result.push_back(static_cast<char>(0x80 | ((rune >> 6) & 0x3f)));
      result.push_back(static_cast<char>(0x80 | (rune & 0x3f)));
    }
  }
  return result;
}

void expect_normalize(const std::string &input, int32_t form,
                      const std::string &expected, uint64_t expected_runes,
                      const char *what) {
  auto text = make(input);
  LangText out{};
  const auto status = lang_unicode_normalize_v1(&out, &text, form);
  need(status == 0, what);
  need(bytes(out) == expected, what);
  need(out.length == expected_runes, what);
  if (!expected.empty()) need(out.owner && out.owner->kind == LangHandleKind::text, what);
  drop(out);
  drop(text);
}

void expect_casefold(const std::string &input, const std::string &expected,
                     uint64_t expected_runes, const char *what) {
  auto text = make(input);
  LangText out{};
  const auto status = lang_unicode_casefold_v1(&out, &text);
  need(status == 0, what);
  need(bytes(out) == expected, what);
  need(out.length == expected_runes, what);
  if (!expected.empty()) need(out.owner && out.owner->kind == LangHandleKind::text, what);
  drop(out);
  drop(text);
}

void expect_boundaries(const std::string &input,
                       const std::vector<uint64_t> &expected,
                       const char *what) {
  auto text = make(input);
  auto *boundaries = lang_unicode_grapheme_boundaries_v1(&text);
  need(boundaries->stride == sizeof(uint64_t), what);
  need(boundaries->length == expected.size(), what);
  need(boundaries->initialized == boundaries->length, what);
  for (uint64_t i = 0; i < boundaries->length; ++i)
    need(*static_cast<uint64_t *>(lang_vec_at_v1(boundaries, i)) == expected[i], what);
  lang_vec_release_v1(boundaries);
  drop(text);
}
LangVec *make_cursor() {
  auto *cursor = lang_vec_new_v1(0, sizeof(uint64_t), nullptr, nullptr);
  const uint64_t zero = 0;
  lang_vec_push_v1(cursor, &zero);
  return cursor;
}

uint64_t cursor_value(const LangVec *cursor) {
  return *static_cast<const uint64_t *>(lang_vec_at_v1(
      const_cast<LangVec *>(cursor), 0));
}

void expect_text_cursor() {
  auto text = make(encode({0x61, 0xe9, 0x4e2d, 0x1f600, 0x00}));
  const auto first = lang_text_cursor_at_v1(&text, 1);
  const auto middle = lang_text_cursor_at_v1(&text, 2);
  const auto end = lang_text_cursor_at_v1(&text, text.length);
  need(first == 1 && middle == 3 && end == text.bytes,
       "cursor positions respect multibyte code points");
  need(lang_text_cursor_read_v1(&text, middle) == (uint64_t{0x4e2d} << 32 | 3),
       "cursor read reports scalar and byte advance");
  need(lang_text_cursor_read_v1(&text, end - 1) == 1,
       "cursor read preserves embedded NUL");

  LangText tail{}, empty{};
  lang_text_cursor_slice_v1(&tail, &text, first, end);
  lang_text_cursor_slice_v1(&empty, &text, end, end);
  need(tail.data == text.data + first && tail.owner == text.owner &&
           tail.length == 4 && tail.bytes == end - first,
       "cursor slice retains original storage and counts code points");
  need(empty.length == 0 && !empty.owner, "empty cursor slice owns nothing");
  drop(text);
  need(bytes(tail) == encode({0xe9, 0x4e2d, 0x1f600, 0x00}),
       "cursor slice outlives the source descriptor");
  drop(tail);
}

void expect_text_take() {
  const std::string input = encode({0x61, 0x4e2d, 0x1f600, 0x65, 0x301});
  auto text = make(input);
  auto *cursor = make_cursor();
  LangText first{}, second{}, third{}, fourth{}, zero{};
  lang_unicode_text_take_v1(&first, &text, cursor, 1);
  need(bytes(first) == "a" && first.length == 1 &&
           first.owner == text.owner && cursor_value(cursor) == 1,
       "cursor take ASCII cluster");
  lang_unicode_text_take_v1(&second, &text, cursor, 1);
  need(bytes(second) == encode({0x4e2d}) && second.length == 1 &&
           second.owner == text.owner && cursor_value(cursor) == 4,
       "cursor take three-byte cluster");
  lang_unicode_text_take_v1(&third, &text, cursor, 1);
  need(bytes(third) == encode({0x1f600}) && third.length == 1 &&
           third.owner == text.owner && cursor_value(cursor) == 8,
       "cursor take four-byte cluster");
  lang_unicode_text_take_v1(&fourth, &text, cursor, 2);
  need(bytes(fourth) == encode({0x65, 0x301}) && fourth.length == 2 &&
           fourth.owner == text.owner && cursor_value(cursor) == input.size(),
       "cursor take multi-code-point cluster");
  lang_unicode_text_take_v1(&zero, &text, cursor, 0);
  need(!zero.owner && !zero.bytes && !zero.length &&
           cursor_value(cursor) == input.size(),
       "zero take is empty and does not advance");
  // Every non-empty take is an owning view. Dropping the source descriptor
  // must not invalidate results that retained the same owner.
  drop(text);
  need(bytes(first) == "a" && bytes(fourth) == encode({0x65, 0x301}),
       "cursor results outlive the source descriptor");
  drop(first);
  drop(second);
  drop(third);
  drop(fourth);
  lang_vec_release_v1(cursor);

  auto ascii = make("abc");
  auto *ascii_cursor = make_cursor();
  LangText ab{}, c{}, ascii_zero{};
  lang_unicode_text_take_v1(&ab, &ascii, ascii_cursor, 2);
  need(bytes(ab) == "ab" && ab.length == 2 && cursor_value(ascii_cursor) == 2,
       "ASCII cursor take");
  lang_unicode_text_take_v1(&c, &ascii, ascii_cursor, 1);
  need(bytes(c) == "c" && c.length == 1 && cursor_value(ascii_cursor) == 3,
       "ASCII cursor end");
  lang_unicode_text_take_v1(&ascii_zero, &ascii, ascii_cursor, 0);
  need(!ascii_zero.owner && !ascii_zero.bytes && !ascii_zero.length &&
           cursor_value(ascii_cursor) == 3,
       "ASCII zero take at end");
  drop(ab);
  drop(c);
  drop(ascii);
  lang_vec_release_v1(ascii_cursor);

  LangText empty{};
  auto *empty_cursor = make_cursor();
  LangText empty_out{};
  lang_unicode_text_take_v1(&empty_out, &empty, empty_cursor, 0);
  need(!empty_out.owner && !empty_out.bytes && !empty_out.length &&
           cursor_value(empty_cursor) == 0,
       "empty zero take");
  lang_vec_release_v1(empty_cursor);
}

} // namespace

int main() {
  try {
    const std::string composed = encode({0x00e9});
    const std::string decomposed = encode({0x0065, 0x0301});
    expect_normalize(decomposed, LANG_UNICODE_NFC, composed, 1,
                     "NFC e+acute");
    expect_normalize(composed, LANG_UNICODE_NFD, decomposed, 2,
                     "NFD e acute");
    expect_normalize(decomposed, LANG_UNICODE_NFD, decomposed, 2,
                     "NFD is idempotent");
    expect_normalize(encode({0x0061, 0x0315, 0x0300}), LANG_UNICODE_NFD,
                     encode({0x0061, 0x0300, 0x0315}), 3,
                     "canonical combining order");
    expect_normalize(encode({0xac01}), LANG_UNICODE_NFD,
                     encode({0x1100, 0x1161, 0x11a8}), 3, "Hangul NFD");
    expect_normalize(encode({0x1100, 0x1161, 0x11a8}), LANG_UNICODE_NFC,
                     encode({0xac01}), 1, "Hangul NFC");
    expect_normalize(encode({0xfb01}), LANG_UNICODE_NFKC, "fi", 2,
                     "NFKC compatibility");
    expect_normalize(encode({0x2460}), LANG_UNICODE_NFKD, "1", 1,
                     "NFKD compatibility");

    expect_casefold(encode({0x00df}), "ss", 2, "sharp s full folding");
    expect_casefold(encode({0x1e9e}), "ss", 2, "capital sharp s full folding");
    expect_casefold(encode({0x0130}), encode({0x0069, 0x0307}), 2,
                     "dotted I one-to-many folding");
    expect_casefold(encode({0xfb01}), "fi", 2, "ligature full folding");
    expect_casefold(encode({0x0053, 0x0074, 0x0072, 0x0061, 0x00df, 0x0065}),
                     "strasse", 7, "string full folding");

    const std::string family = encode(
        {0x1f468, 0x200d, 0x1f469, 0x200d, 0x1f467, 0x200d, 0x1f466});
    expect_boundaries(family, {0, 7}, "emoji ZWJ family cluster");
    expect_boundaries(encode({0x1f1fa, 0x1f1f8}), {0, 2}, "flag RI pair");
    expect_boundaries(encode({0x1f1fa, 0x1f1f8, 0x1f1e6}), {0, 2, 3},
                      "regional indicator pairing");
    expect_boundaries(encode({0x0061, 0x0301, 0x0062}), {0, 2, 3},
                      "combining mark cluster");
    expect_boundaries(std::string("A\0B", 3), {0, 1, 2, 3},
                      "embedded NUL has ordinary control boundaries");
    expect_boundaries("", {0}, "empty grapheme boundaries");
    expect_text_cursor();
    expect_text_take();

    const std::string nul_input("A\0B", 3);
    expect_normalize(nul_input, LANG_UNICODE_NFC, nul_input, 3,
                     "embedded NUL NFC");
    expect_normalize(nul_input, LANG_UNICODE_NFKD, nul_input, 3,
                     "embedded NUL NFKD");
    const std::string folded_nul("a\0b", 3);
    expect_casefold(nul_input, folded_nul, 3, "embedded NUL casefold");
    expect_normalize("", LANG_UNICODE_NFC, "", 0, "empty NFC");
    expect_casefold("", "", 0, "empty casefold");

    LangText empty{};
    expect_boundaries("", {0}, "empty again");
    need(!empty.owner && !empty.bytes && !empty.length, "empty descriptor");

    // In-place output must release the old text reference only after replacing
    // the descriptor.
    auto in_place = make(decomposed);
    auto *old_owner = in_place.owner;
    need(lang_unicode_normalize_v1(&in_place, &in_place, LANG_UNICODE_NFC) == 0,
         "in-place NFC status");
    need(bytes(in_place) == composed && in_place.owner != old_owner,
         "in-place NFC result");
    drop(in_place);

    auto folded = make(encode({0x00df}));
    old_owner = folded.owner;
    need(lang_unicode_casefold_v1(&folded, &folded) == 0, "in-place fold status");
    need(bytes(folded) == "ss" && folded.owner != old_owner, "in-place fold result");
    drop(folded);

    LangText invalid_form{};
    auto valid = make("A");
    need(lang_unicode_normalize_v1(&invalid_form, &valid, 9) == -22,
         "invalid form status");
    need(!invalid_form.owner && !invalid_form.bytes, "invalid form output");
    drop(valid);

    const uint8_t bad_utf8[] = {0xc0};
    LangText bad{bad_utf8, 1, 0, nullptr};
    LangText bad_out{};
    need(lang_unicode_normalize_v1(&bad_out, &bad, LANG_UNICODE_NFC) == -22,
         "invalid UTF-8 status");
    need(!bad_out.owner && !bad_out.bytes, "invalid UTF-8 output");

    need(lang_unicode_rune_to_lower_v1('A') == 'a', "simple lower A");
    need(lang_unicode_rune_to_upper_v1('a') == 'A', "simple upper a");
    need(lang_unicode_rune_to_lower_v1(0x0130) == 0x0069, "simple lower dotted I");
    need(lang_unicode_rune_to_upper_v1(0xd800) == 0xd800, "invalid rune unchanged");

    std::cout << "host unicode smoke: passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "host unicode smoke: failed: " << error.what() << "\n";
    return 1;
  }
}
