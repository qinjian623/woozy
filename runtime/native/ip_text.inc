// Shared by the Linux and Darwin network runtimes inside their private namespaces.
inline int hex_digit(char byte) noexcept {
  if (byte >= '0' && byte <= '9') return byte - '0';
  if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
  if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10;
  return -1;
}

LANG_NATIVE_NO_STACK_PROTECTOR
int parse_ipv4(const char *text, uint64_t length,
               uint8_t bytes[4]) noexcept {
  if (text == nullptr || length == 0) return -1;
  uint64_t position = 0;
  for (uint64_t part = 0; part < 4; ++part) {
    if (part != 0) {
      if (position >= length || text[position] != '.') return -1;
      ++position;
    }
    if (position >= length) return -1;
    uint32_t value = 0;
    uint64_t digits = 0;
    while (position < length && text[position] >= '0' &&
           text[position] <= '9') {
      if (digits == 3) return -1;
      // Match inet_pton's strict dotted-decimal form: a leading zero is
      // allowed only for the single-digit component "0".
      if (digits == 0 && text[position] == '0' && position + 1 < length &&
          text[position + 1] >= '0' && text[position + 1] <= '9')
        return -1;
      value = value * 10 + static_cast<uint32_t>(text[position] - '0');
      if (value > 255) return -1;
      ++digits;
      ++position;
    }
    if (digits == 0) return -1;
    bytes[part] = static_cast<uint8_t>(value);
  }
  return position == length ? 0 : -1;
}

LANG_NATIVE_NO_STACK_PROTECTOR
int parse_ipv6(const char *text, uint64_t length,
               uint8_t bytes[16]) noexcept {
  if (text == nullptr || length == 0) return -1;

  uint16_t groups[8];
  int group_count = 0;
  int compress_at = -1;
  uint64_t position = 0;

  while (position < length) {
    if (text[position] == ':') {
      if (position + 1 < length && text[position + 1] == ':') {
        if (compress_at >= 0) return -1;
        compress_at = group_count;
        position += 2;
        if (position == length) break;
        continue;
      }
      return -1;
    }

    const uint64_t token_start = position;
    while (position < length && text[position] != ':') ++position;
    const uint64_t token_length = position - token_start;
    if (token_length == 0) return -1;

    bool has_dot = false;
    for (uint64_t index = token_start; index < position; ++index) {
      if (text[index] == '.') has_dot = true;
    }

    if (has_dot) {
      if (position != length) return -1;
      if (group_count > 6) return -1;
      uint8_t ipv4[4];
      if (parse_ipv4(text + token_start, token_length, ipv4) != 0) return -1;
      groups[group_count++] =
          static_cast<uint16_t>((static_cast<uint16_t>(ipv4[0]) << 8) |
                                static_cast<uint16_t>(ipv4[1]));
      groups[group_count++] =
          static_cast<uint16_t>((static_cast<uint16_t>(ipv4[2]) << 8) |
                                static_cast<uint16_t>(ipv4[3]));
      break;
    }

    if (token_length > 4) return -1;
    uint32_t value = 0;
    for (uint64_t index = token_start; index < position; ++index) {
      const int digit = hex_digit(text[index]);
      if (digit < 0) return -1;
      value = (value << 4) | static_cast<uint32_t>(digit);
    }
    if (group_count >= 8) return -1;
    groups[group_count++] = static_cast<uint16_t>(value);

    if (position == length) break;
    if (position + 1 < length && text[position + 1] == ':') {
      if (compress_at >= 0) return -1;
      compress_at = group_count;
      position += 2;
      if (position == length) break;
      continue;
    }
    ++position;
    if (position == length) return -1;
  }

  uint16_t final_groups[8];
  if (compress_at < 0) {
    if (group_count != 8) return -1;
    for (int index = 0; index < 8; ++index)
      final_groups[index] = groups[index];
  } else {
    if (group_count >= 8) return -1;
    const int zero_groups = 8 - group_count;
    int target = 0;
    for (int index = 0; index < compress_at; ++index)
      final_groups[target++] = groups[index];
    for (int index = 0; index < zero_groups; ++index)
      final_groups[target++] = 0;
    for (int index = compress_at; index < group_count; ++index)
      final_groups[target++] = groups[index];
  }

  for (int index = 0; index < 8; ++index) {
    bytes[index * 2] = static_cast<uint8_t>(final_groups[index] >> 8);
    bytes[index * 2 + 1] = static_cast<uint8_t>(final_groups[index]);
  }
  return 0;
}

LANG_NATIVE_NO_STACK_PROTECTOR
inline void append_hex_group(char *out, uint64_t *position,
                             uint16_t value) noexcept {
  if (value == 0) {
    out[(*position)++] = '0';
    return;
  }
  char digits[4];
  uint64_t count = 0;
  while (value != 0) {
    const uint16_t nibble = static_cast<uint16_t>(value & 0x0f);
    digits[count++] =
        static_cast<char>(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
    value = static_cast<uint16_t>(value >> 4);
  }
  while (count != 0) out[(*position)++] = digits[--count];
}

LANG_NATIVE_NO_STACK_PROTECTOR
uint64_t format_ipv4(const uint8_t *bytes, char *out) noexcept {
  uint64_t position = 0;
  for (uint64_t index = 0; index < 4; ++index) {
    if (index != 0) out[position++] = '.';
    const uint32_t value = bytes[index];
    if (value >= 100) out[position++] = static_cast<char>('0' + value / 100);
    if (value >= 10)
      out[position++] = static_cast<char>('0' + (value / 10) % 10);
    out[position++] = static_cast<char>('0' + value % 10);
  }
  return position;
}

LANG_NATIVE_NO_STACK_PROTECTOR
uint64_t format_ipv6(const uint8_t *bytes, char *out) noexcept {
  // The IPv4-mapped prefix ::ffff:0:0/96 uses the mixed decimal form.
  bool ipv4_mapped = true;
  for (uint64_t index = 0; index < 10; ++index) {
    if (bytes[index] != 0) {
      ipv4_mapped = false;
      break;
    }
  }
  if (ipv4_mapped && (bytes[10] != 0xff || bytes[11] != 0xff))
    ipv4_mapped = false;

  if (ipv4_mapped) {
    uint64_t position = 0;
    out[position++] = ':';
    out[position++] = ':';
    out[position++] = 'f';
    out[position++] = 'f';
    out[position++] = 'f';
    out[position++] = 'f';
    out[position++] = ':';
    return position + format_ipv4(bytes + 12, out + position);
  }

  uint16_t groups[8];
  for (uint64_t index = 0; index < 8; ++index) {
    groups[index] = static_cast<uint16_t>(
        (static_cast<uint16_t>(bytes[index * 2]) << 8) |
        static_cast<uint16_t>(bytes[index * 2 + 1]));
  }

  int best_start = -1;
  int best_length = 0;
  int run_start = -1;
  int run_length = 0;
  for (int index = 0; index < 8; ++index) {
    if (groups[index] == 0) {
      if (run_start < 0) run_start = index;
      ++run_length;
      if (run_length > best_length && run_length >= 2) {
        best_start = run_start;
        best_length = run_length;
      }
    } else {
      run_start = -1;
      run_length = 0;
    }
  }

  uint64_t position = 0;
  int index = 0;
  while (index < 8) {
    if (index == best_start) {
      out[position++] = ':';
      out[position++] = ':';
      index += best_length;
      if (index == 8) break;
      continue;
    }
    if (position != 0 && out[position - 1] != ':') out[position++] = ':';
    append_hex_group(out, &position, groups[index]);
    ++index;
  }
  return position;
}
