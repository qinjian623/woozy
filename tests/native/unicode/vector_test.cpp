#include "../../../runtime/native/unicode/unicode.hpp"
#include <cstddef>
#include <cstdint>

// This program intentionally uses only Linux x86_64 system calls for file and
// console I/O. It is linked once against the host libc for a quick hosted run
// and once with -nostdlib/-static for the real runtime check.

namespace {
constexpr uint32_t kMaxCodePoints = 256;
constexpr uint32_t kMaxUtf8Bytes = kMaxCodePoints * 4;
constexpr uint64_t kFileCapacity = 8 * 1024 * 1024;

unsigned char g_file[kFileCapacity];
unsigned char g_encoded[kMaxUtf8Bytes];
char g_path[1024];

uint64_t g_normalization_lines;
uint64_t g_normalization_checks;
uint64_t g_casefold_lines;
uint64_t g_casefold_cf;
uint64_t g_casefold_skipped;
uint64_t g_grapheme_lines;
uint64_t g_smoke_checks;

enum : long {
  kSysRead = 0,
  kSysWrite = 1,
  kSysOpen = 2,
  kSysClose = 3,
  kSysExit = 60,
  kEintr = -4,
};

long sys_read(long fd, void *data, uint64_t bytes) {
  long result;
  __asm__ volatile("syscall"
                   : "=a"(result)
                   : "a"(kSysRead), "D"(fd), "S"(data), "d"(bytes)
                   : "rcx", "r11", "memory");
  return result;
}

long sys_write(long fd, const void *data, uint64_t bytes) {
  long result;
  __asm__ volatile("syscall"
                   : "=a"(result)
                   : "a"(kSysWrite), "D"(fd), "S"(data), "d"(bytes)
                   : "rcx", "r11", "memory");
  return result;
}

long sys_open(const char *path) {
  long result;
  __asm__ volatile("syscall"
                   : "=a"(result)
                   : "a"(kSysOpen), "D"(path), "S"(0), "d"(0)
                   : "rcx", "r11", "memory");
  return result;
}

long sys_close(long fd) {
  long result;
  __asm__ volatile("syscall"
                   : "=a"(result)
                   : "a"(kSysClose), "D"(fd)
                   : "rcx", "r11", "memory");
  return result;
}

[[noreturn]] void sys_exit(int status) {
  __asm__ volatile("syscall"
                   :
                   : "a"(kSysExit), "D"(status)
                   : "rcx", "r11", "memory");
  __builtin_unreachable();
}

void emit(const char *text, uint64_t bytes) {
  while (bytes) {
    const long written = sys_write(1, text, bytes);
    if (written == kEintr) continue;
    if (written <= 0) break;
    text += written;
    bytes -= static_cast<uint64_t>(written);
  }
}

void emit(const char *text) {
  uint64_t bytes = 0;
  while (text[bytes]) ++bytes;
  emit(text, bytes);
}

void emit_u64(uint64_t value) {
  char digits[24];
  uint64_t count = 0;
  if (!value) {
    emit("0");
    return;
  }
  while (value) {
    digits[count++] = static_cast<char>('0' + value % 10);
    value /= 10;
  }
  while (count) emit(&digits[--count], 1);
}

[[noreturn]] void fail(const char *suite, uint64_t line, const char *reason) {
  emit("FAIL:");
  emit(suite);
  emit(":line=");
  emit_u64(line);
  emit(":");
  emit(reason);
  emit("\n");
  sys_exit(1);
}

const uint8_t *skip_space(const uint8_t *position, const uint8_t *end) {
  while (position < end &&
         (*position == ' ' || *position == '\t' || *position == '\r'))
    ++position;
  return position;
}

int hex_digit(uint8_t byte) {
  if (byte >= '0' && byte <= '9') return byte - '0';
  if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
  if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10;
  return -1;
}

bool scalar(uint32_t rune) {
  return rune <= 0x10ffff && !(rune >= 0xd800 && rune <= 0xdfff);
}

bool parse_hex(const uint8_t *&position, const uint8_t *end, uint32_t &value) {
  uint32_t parsed = 0;
  uint32_t digits = 0;
  while (position < end) {
    const int digit = hex_digit(*position);
    if (digit < 0) break;
    if (++digits > 6) return false;
    parsed = (parsed << 4) | static_cast<uint32_t>(digit);
    ++position;
  }
  if (!digits || !scalar(parsed)) return false;
  value = parsed;
  return true;
}

bool parse_sequence(const uint8_t *&position, const uint8_t *end,
                    uint32_t *runes, uint32_t capacity, uint32_t &count) {
  count = 0;
  for (;;) {
    position = skip_space(position, end);
    if (position >= end || *position == ';' || *position == '#') return true;
    if (count >= capacity) return false;
    if (!parse_hex(position, end, runes[count])) return false;
    ++count;
  }
}

uint64_t encode(const uint32_t *runes, uint32_t count, unsigned char *output,
                uint64_t capacity) {
  uint64_t offset = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t rune = runes[i];
    if (!scalar(rune)) return UINT64_MAX;
    if (rune < 0x80) {
      if (offset >= capacity) return UINT64_MAX;
      output[offset++] = static_cast<unsigned char>(rune);
    } else if (rune < 0x800) {
      if (capacity - offset < 2) return UINT64_MAX;
      output[offset++] = static_cast<unsigned char>(0xc0 | (rune >> 6));
      output[offset++] = static_cast<unsigned char>(0x80 | (rune & 0x3f));
    } else if (rune < 0x10000) {
      if (capacity - offset < 3) return UINT64_MAX;
      output[offset++] = static_cast<unsigned char>(0xe0 | (rune >> 12));
      output[offset++] =
          static_cast<unsigned char>(0x80 | ((rune >> 6) & 0x3f));
      output[offset++] = static_cast<unsigned char>(0x80 | (rune & 0x3f));
    } else {
      if (capacity - offset < 4) return UINT64_MAX;
      output[offset++] = static_cast<unsigned char>(0xf0 | (rune >> 18));
      output[offset++] =
          static_cast<unsigned char>(0x80 | ((rune >> 12) & 0x3f));
      output[offset++] =
          static_cast<unsigned char>(0x80 | ((rune >> 6) & 0x3f));
      output[offset++] = static_cast<unsigned char>(0x80 | (rune & 0x3f));
    }
  }
  return offset;
}

LangText literal(const unsigned char *bytes, uint64_t length,
                 uint64_t runes) {
  return {length ? bytes : nullptr, length, runes, nullptr};
}

bool same(const LangText &text, const unsigned char *bytes, uint64_t length) {
  if (text.bytes != length) return false;
  for (uint64_t i = 0; i < length; ++i)
    if (text.data[i] != bytes[i]) return false;
  return true;
}

void release(LangText &text) {
  if (text.owner) lang_handle_release_v1(text.owner);
  text = {};
}

void check_normalize(const LangText &input, const LangText &expected,
                     int32_t form, uint64_t line) {
  LangText output{};
  if (lang_unicode_normalize_v1(&output, &input, form) != 0)
    fail("normalization", line, "non-zero status");
  if (!same(output, expected.data, expected.bytes) ||
      output.length != expected.length)
    fail("normalization", line, "output differs");
  if (expected.bytes && (!output.owner ||
                         output.owner->kind != LangHandleKind::text))
    fail("normalization", line, "missing text owner");
  if (!expected.bytes && output.owner)
    fail("normalization", line, "empty output allocated");
  release(output);
}

void normalization_file(const uint8_t *data, uint64_t size) {
  const uint8_t *position = data;
  const uint8_t *const end = data + size;
  uint64_t line_number = 0;
  while (position < end) {
    const uint8_t *line = position;
    while (position < end && *position != '\n') ++position;
    const uint8_t *line_end = position;
    if (position < end) ++position;
    if (line_end > line && line_end[-1] == '\r') --line_end;
    ++line_number;

    const uint8_t *start = skip_space(line, line_end);
    if (start >= line_end || *start == '#' || *start == '@') continue;

    uint32_t runes[5][kMaxCodePoints];
    uint32_t counts[5];
    unsigned char encoded[5][kMaxUtf8Bytes];
    uint64_t lengths[5];
    const uint8_t *field = line;
    for (uint32_t i = 0; i < 5; ++i) {
      if (!parse_sequence(field, line_end, runes[i], kMaxCodePoints, counts[i]))
        fail("normalization", line_number, "bad code point sequence");
      if (i != 4) {
        field = skip_space(field, line_end);
        if (field >= line_end || *field != ';')
          fail("normalization", line_number, "missing field separator");
        ++field;
      }
    }
    for (uint32_t i = 0; i < 5; ++i) {
      lengths[i] = encode(runes[i], counts[i], encoded[i], kMaxUtf8Bytes);
      if (lengths[i] == UINT64_MAX)
        fail("normalization", line_number, "encode failed");
    }

    LangText text[5];
    for (uint32_t i = 0; i < 5; ++i)
      text[i] = literal(encoded[i], lengths[i], counts[i]);

    // These are the invariants from NormalizationTest.txt and the upstream
    // utf8proc normtest: c1/c2/c3 share canonical forms, c4/c5 share
    // compatibility forms.
    for (int32_t form = 0; form < 4; ++form) {
      for (uint32_t input = 0; input < 5; ++input) {
        uint32_t expected = 0;
        if (form == LANG_UNICODE_NFC)
          expected = input < 3 ? 1 : 3;
        else if (form == LANG_UNICODE_NFD)
          expected = input < 3 ? 2 : 4;
        else if (form == LANG_UNICODE_NFKC)
          expected = 3;
        else
          expected = 4;
        check_normalize(text[input], text[expected], form, line_number);
        ++g_normalization_checks;
      }
    }
    ++g_normalization_lines;
  }
}

void casefold_file(const uint8_t *data, uint64_t size) {
  const uint8_t *position = data;
  const uint8_t *const end = data + size;
  uint64_t line_number = 0;
  while (position < end) {
    const uint8_t *line = position;
    while (position < end && *position != '\n') ++position;
    const uint8_t *line_end = position;
    if (position < end) ++position;
    if (line_end > line && line_end[-1] == '\r') --line_end;
    ++line_number;

    const uint8_t *start = skip_space(line, line_end);
    if (start >= line_end || *start == '#') continue;

    uint32_t code[1], mapping[kMaxCodePoints];
    uint32_t code_count = 0, mapping_count = 0;
    const uint8_t *field = line;
    if (!parse_sequence(field, line_end, code, 1, code_count) ||
        code_count != 1)
      fail("casefold", line_number, "bad source code point");
    field = skip_space(field, line_end);
    if (field >= line_end || *field != ';')
      fail("casefold", line_number, "missing status separator");
    ++field;
    field = skip_space(field, line_end);
    if (field >= line_end)
      fail("casefold", line_number, "missing status");
    const uint8_t status = *field++;
    field = skip_space(field, line_end);
    if (field >= line_end || *field != ';')
      fail("casefold", line_number, "missing mapping separator");
    ++field;
    if (!parse_sequence(field, line_end, mapping, kMaxCodePoints,
                        mapping_count))
      fail("casefold", line_number, "bad mapping");
    ++g_casefold_lines;
    if (status != 'C' && status != 'F') {
      ++g_casefold_skipped;
      continue;
    }
    if (!mapping_count)
      fail("casefold", line_number, "empty C/F mapping");

    unsigned char input_bytes[4], output_bytes[kMaxUtf8Bytes];
    const uint64_t input_length = encode(code, 1, input_bytes, sizeof(input_bytes));
    const uint64_t output_length =
        encode(mapping, mapping_count, output_bytes, sizeof(output_bytes));
    if (input_length == UINT64_MAX || output_length == UINT64_MAX)
      fail("casefold", line_number, "encode failed");

    LangText input = literal(input_bytes, input_length, 1);
    LangText output{};
    if (lang_unicode_casefold_v1(&output, &input) != 0)
      fail("casefold", line_number, "non-zero status");
    if (!same(output, output_bytes, output_length) ||
        output.length != mapping_count)
      fail("casefold", line_number, "output differs");
    release(output);
    ++g_casefold_cf;
  }
}

void grapheme_file(const uint8_t *data, uint64_t size) {
  const uint8_t *position = data;
  const uint8_t *const end = data + size;
  uint64_t line_number = 0;
  while (position < end) {
    const uint8_t *line = position;
    while (position < end && *position != '\n') ++position;
    const uint8_t *line_end = position;
    if (position < end) ++position;
    if (line_end > line && line_end[-1] == '\r') --line_end;
    ++line_number;

    const uint8_t *start = skip_space(line, line_end);
    if (start >= line_end || *start == '#') continue;

    uint32_t runes[kMaxCodePoints];
    uint64_t expected[kMaxCodePoints + 1];
    uint32_t count = 0;
    uint32_t boundary_count = 0;
    const uint8_t *field = line;
    while (field < line_end) {
      field = skip_space(field, line_end);
      if (field >= line_end || *field == '#') break;
      if (field + 1 < line_end && field[0] == 0xc3 && field[1] == 0xb7) {
        if (!boundary_count || expected[boundary_count - 1] != count)
          expected[boundary_count++] = count;
        field += 2;
        continue;
      }
      if (field + 1 < line_end && field[0] == 0xc3 && field[1] == 0x97) {
        field += 2;
        continue;
      }
      if (count >= kMaxCodePoints)
        fail("grapheme", line_number, "too many code points");
      if (!parse_hex(field, line_end, runes[count]))
        fail("grapheme", line_number, "bad code point");
      ++count;
    }
    if (!boundary_count)
      fail("grapheme", line_number, "missing initial boundary");
    if (boundary_count && expected[0] != 0)
      fail("grapheme", line_number, "missing zero boundary");
    if (count && expected[boundary_count - 1] != count)
      fail("grapheme", line_number, "missing final boundary");
    if (!count && (boundary_count != 1 || expected[0] != 0))
      fail("grapheme", line_number, "bad empty boundaries");

    const uint64_t length = encode(runes, count, g_encoded, kMaxUtf8Bytes);
    if (length == UINT64_MAX)
      fail("grapheme", line_number, "encode failed");
    LangText input = literal(g_encoded, length, count);
    auto *actual = lang_unicode_grapheme_boundaries_v1(&input);
    if (actual->stride != sizeof(uint64_t) ||
        actual->length != boundary_count ||
        actual->initialized != actual->length)
      fail("grapheme", line_number, "boundary vector shape differs");
    for (uint32_t i = 0; i < boundary_count; ++i) {
      if (*static_cast<uint64_t *>(lang_vec_at_v1(actual, i)) != expected[i])
        fail("grapheme", line_number, "boundary position differs");
    }
    lang_vec_release_v1(actual);
    ++g_grapheme_lines;
  }
}

LangText dynamic(const unsigned char *bytes, uint64_t length) {
  LangText result{};
  if (lang_text_from_utf8_v1(&result, bytes, length) != 0)
    fail("smoke", 0, "valid UTF-8 rejected");
  return result;
}

void smoke_normalize(const unsigned char *input, uint64_t input_length,
                     int32_t form, const unsigned char *expected,
                     uint64_t expected_length, uint64_t expected_runes) {
  auto text = dynamic(input, input_length);
  LangText output{};
  if (lang_unicode_normalize_v1(&output, &text, form) != 0)
    fail("smoke", 0, "normalize status");
  if (!same(output, expected, expected_length) ||
      output.length != expected_runes)
    fail("smoke", 0, "normalize bytes");
  release(output);
  release(text);
  ++g_smoke_checks;
}

void smoke_casefold(const unsigned char *input, uint64_t input_length,
                    const unsigned char *expected, uint64_t expected_length,
                    uint64_t expected_runes) {
  auto text = dynamic(input, input_length);
  LangText output{};
  if (lang_unicode_casefold_v1(&output, &text) != 0)
    fail("smoke", 0, "casefold status");
  if (!same(output, expected, expected_length) ||
      output.length != expected_runes)
    fail("smoke", 0, "casefold bytes");
  release(output);
  release(text);
  ++g_smoke_checks;
}

void smoke_boundaries(const unsigned char *input, uint64_t input_length,
                      const uint64_t *expected, uint64_t expected_count) {
  auto text = dynamic(input, input_length);
  auto *actual = lang_unicode_grapheme_boundaries_v1(&text);
  if (actual->length != expected_count ||
      actual->initialized != actual->length)
    fail("smoke", 0, "grapheme length");
  for (uint64_t i = 0; i < expected_count; ++i)
    if (*static_cast<uint64_t *>(lang_vec_at_v1(actual, i)) != expected[i])
      fail("smoke", 0, "grapheme position");
  lang_vec_release_v1(actual);
  release(text);
  ++g_smoke_checks;
}

void smoke() {
  const unsigned char nul[] = {'A', 0, 'B'};
  const uint64_t nul_boundaries[] = {0, 1, 2, 3};
  smoke_normalize(nul, sizeof(nul), LANG_UNICODE_NFC, nul, sizeof(nul), 3);
  smoke_normalize(nul, sizeof(nul), LANG_UNICODE_NFKD, nul, sizeof(nul), 3);
  const unsigned char folded_nul[] = {'a', 0, 'b'};
  smoke_casefold(nul, sizeof(nul), folded_nul, sizeof(folded_nul), 3);
  smoke_boundaries(nul, sizeof(nul), nul_boundaries, 4);

  smoke_normalize(nullptr, 0, LANG_UNICODE_NFC, nullptr, 0, 0);
  smoke_casefold(nullptr, 0, nullptr, 0, 0);
  const uint64_t empty_boundaries[] = {0};
  smoke_boundaries(nullptr, 0, empty_boundaries, 1);

  const unsigned char sharp_s[] = {0xc3, 0x9f};
  const unsigned char ss[] = {'s', 's'};
  smoke_casefold(sharp_s, sizeof(sharp_s), ss, sizeof(ss), 2);
  const unsigned char dotted_i[] = {0xc4, 0xb0};
  const unsigned char i_dot[] = {'i', 0xcc, 0x87};
  smoke_casefold(dotted_i, sizeof(dotted_i), i_dot, sizeof(i_dot), 2);

  const unsigned char family[] = {0xf0, 0x9f, 0x91, 0xa8, 0xe2, 0x80, 0x8d,
                                  0xf0, 0x9f, 0x91, 0xa9, 0xe2, 0x80, 0x8d,
                                  0xf0, 0x9f, 0x91, 0xa7, 0xe2, 0x80, 0x8d,
                                  0xf0, 0x9f, 0x91, 0xa6};
  const uint64_t family_boundaries[] = {0, 7};
  smoke_boundaries(family, sizeof(family), family_boundaries, 2);
  const unsigned char flags[] = {0xf0, 0x9f, 0x87, 0xba, 0xf0, 0x9f, 0x87,
                                 0xb8, 0xf0, 0x9f, 0x87, 0xa6};
  const uint64_t flag_boundaries[] = {0, 2, 3};
  smoke_boundaries(flags, sizeof(flags), flag_boundaries, 3);
}

uint64_t read_file(const char *directory, const char *name) {
  uint64_t path_length = 0;
  while (directory[path_length]) {
    if (path_length + 2 >= sizeof(g_path))
      fail("file", 0, "directory path too long");
    g_path[path_length] = directory[path_length];
    ++path_length;
  }
  if (path_length && g_path[path_length - 1] != '/') g_path[path_length++] = '/';
  for (uint64_t i = 0; name[i]; ++i) {
    if (path_length + 1 >= sizeof(g_path))
      fail("file", 0, "file path too long");
    g_path[path_length++] = name[i];
  }
  g_path[path_length] = 0;

  const long fd = sys_open(g_path);
  if (fd < 0) fail("file", 0, "open failed");
  uint64_t total = 0;
  for (;;) {
    if (total == kFileCapacity) fail("file", 0, "test file too large");
    const long read = sys_read(fd, g_file + total, kFileCapacity - total);
    if (read == kEintr) continue;
    if (read < 0) fail("file", 0, "read failed");
    if (!read) break;
    total += static_cast<uint64_t>(read);
  }
  sys_close(fd);
  return total;
}
} // namespace

#ifndef LANG_UNICODE_TEST_DATA_DIR
#define LANG_UNICODE_TEST_DATA_DIR "tests/native/unicode/data"
#endif

extern "C" int main() {
  const char *directory = LANG_UNICODE_TEST_DATA_DIR;
  const uint64_t normalization_bytes =
      read_file(directory, "NormalizationTest.txt");
  normalization_file(g_file, normalization_bytes);
  const uint64_t casefold_bytes = read_file(directory, "CaseFolding.txt");
  casefold_file(g_file, casefold_bytes);
  const uint64_t grapheme_bytes =
      read_file(directory, "GraphemeBreakTest.txt");
  grapheme_file(g_file, grapheme_bytes);
  smoke();

  emit("PASS normalization lines=");
  emit_u64(g_normalization_lines);
  emit(" checks=");
  emit_u64(g_normalization_checks);
  emit(" casefold lines=");
  emit_u64(g_casefold_lines);
  emit(" cf=");
  emit_u64(g_casefold_cf);
  emit(" skipped=");
  emit_u64(g_casefold_skipped);
  emit(" grapheme lines=");
  emit_u64(g_grapheme_lines);
  emit(" smoke=");
  emit_u64(g_smoke_checks);
  emit("\n");
  return 0;
}
