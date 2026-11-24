#include "dns.hpp"

#include "alloc.hpp"
#if defined(__APPLE__) && defined(__aarch64__)
#include "../darwin/posix.hpp"
#include "gc_program.hpp"

#include <cstring>
#include <netdb.h>
#include <sys/socket.h>

#define LANG_NATIVE_NO_STACK_PROTECTOR __attribute__((no_stack_protector))
#else
#include "linux.hpp"
#endif
#include "os.hpp"
#include "system.hpp"

#include <cstdint>

#if (defined(__x86_64__) && defined(__linux__)) || \
    (defined(__APPLE__) && defined(__aarch64__))

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

namespace {

// Woozy's private network ABI uses Linux x86_64 constants.
constexpr int32_t kAfInet = 2;
constexpr int32_t kAfInet6 = 10;
constexpr int32_t kSockStream = 1;
constexpr int32_t kSockDgram = 2;
constexpr int32_t kSockNonblock = 2048;
constexpr int32_t kSockCloexec = 524288;
constexpr int32_t kSolSocket = 1;
constexpr int32_t kSoError = 4;
constexpr int16_t kPollIn = 0x001;
constexpr int16_t kPollOut = 0x004;
constexpr int16_t kPollNval = 0x020;

constexpr uint16_t kDnsTypeA = 1;
constexpr uint16_t kDnsTypeCname = 5;
constexpr uint16_t kDnsTypeAaaa = 28;
constexpr uint16_t kDnsClassIn = 1;
constexpr uint16_t kDnsFlagQr = 0x8000;
constexpr uint16_t kDnsFlagTc = 0x0200;
constexpr uint16_t kDnsFlagRd = 0x0100;
constexpr uint16_t kDnsRcodeNoError = 0;
constexpr uint16_t kDnsRcodeFormErr = 1;
constexpr uint16_t kDnsRcodeServFail = 2;
constexpr uint16_t kDnsRcodeNxDomain = 3;
constexpr uint16_t kDnsRcodeNotImp = 4;
constexpr uint16_t kDnsRcodeRefused = 5;

constexpr int64_t kErrNoEnt = -2;
constexpr int64_t kErrIo = -5;
constexpr int64_t kErrEbadf = -9;
constexpr int64_t kErrEagain = -11;
constexpr int64_t kErrNoMem = -12;
constexpr int64_t kErrAcces = -13;
constexpr int64_t kErrFault = -14;
constexpr int64_t kErrInval = -22;
constexpr int64_t kErrNoSys = -38;
constexpr int64_t kErrProto = -71;
constexpr int64_t kErrAfNoSupport = -97;
constexpr int64_t kErrConnReset = -104;
constexpr int64_t kErrTimeout = -110;
constexpr int64_t kErrEintr = -4;
constexpr int64_t kErrInProgress = -115;
constexpr int64_t kErrAlready = -114;
constexpr int64_t kErrIsConn = -106;

constexpr uint64_t kMaxHostnameBytes = 253;
constexpr uint64_t kMaxLabelBytes = 63;
constexpr uint64_t kMaxDnsMessage = 65535;
constexpr uint64_t kMaxQueryBytes = 512;
constexpr uint64_t kMaxResults = 64;
constexpr uint64_t kMaxCnameHops = 8;
constexpr uint64_t kMaxCnameRecords = 16;
constexpr uint64_t kMaxAddressRecords = 64;
#if defined(__linux__)
constexpr uint16_t kDnsPort = 53;
constexpr uint64_t kMaxNameBuffer = 256;
constexpr uint64_t kMaxNameservers = 3;
constexpr uint64_t kMaxSearchDomains = 4;
constexpr uint64_t kMaxAttempts = 5;
constexpr uint32_t kMaxTimeoutSeconds = 30;
constexpr uint32_t kMaxNdots = 15;
constexpr uint32_t kDefaultTimeoutSeconds = 5;
constexpr uint32_t kDefaultAttempts = 2;
constexpr uint32_t kDefaultNdots = 1;
constexpr uint64_t kHostsBufferBytes = 16384;
constexpr uint64_t kResolvBufferBytes = 8192;
#endif

struct Candidate {
  int32_t family;
  uint32_t scope;
  uint8_t bytes[16];
};

struct CandidateList {
  Candidate items[kMaxResults];
  uint64_t count;
};

#if defined(__linux__)
struct ResolverConfig {
  int32_t server_family[kMaxNameservers];
  uint8_t server_bytes[kMaxNameservers][16];
  uint64_t server_count;
  char search[kMaxSearchDomains][kMaxNameBuffer];
  uint64_t search_count;
  uint32_t timeout_seconds;
  uint32_t attempts;
  uint32_t ndots;
};
#endif

char ascii_lower(char byte) noexcept {
  if (byte >= 'A' && byte <= 'Z') return static_cast<char>(byte - 'A' + 'a');
  return byte;
}

bool is_name_char(char byte) noexcept {
  return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
         (byte >= '0' && byte <= '9') || byte == '-' || byte == '_';
}

uint64_t text_length(const char *text) noexcept {
  uint64_t length = 0;
  while (text[length] != '\0') ++length;
  return length;
}

bool same_text(const char *left, const char *right) noexcept {
  uint64_t index = 0;
  while (left[index] != '\0' && right[index] != '\0') {
    if (left[index] != right[index]) return false;
    ++index;
  }
  return left[index] == '\0' && right[index] == '\0';
}

bool equal_ignore_case(const char *left, uint64_t left_length,
                       const char *right, uint64_t right_length) noexcept {
  if (left_length != right_length) return false;
  for (uint64_t index = 0; index < left_length; ++index) {
    if (ascii_lower(left[index]) != ascii_lower(right[index])) return false;
  }
  return true;
}

void copy_string(char *destination, const char *source,
                 uint64_t length) noexcept {
  for (uint64_t index = 0; index < length; ++index)
    destination[index] = source[index];
  destination[length] = '\0';
}

uint16_t read_u16_be(const uint8_t *bytes) noexcept {
  return static_cast<uint16_t>(static_cast<uint16_t>(bytes[0]) << 8) |
         static_cast<uint16_t>(bytes[1]);
}

void write_u16_be(uint8_t *bytes, uint16_t value) noexcept {
  bytes[0] = static_cast<uint8_t>(value >> 8);
  bytes[1] = static_cast<uint8_t>(value);
}

LangVec *make_bytes_vec(const uint8_t *bytes, uint64_t length) noexcept {
  LangVec *vector = lang_vec_new_v1(length, 1, nullptr, nullptr);
  for (uint64_t index = 0; index < length; ++index)
    lang_vec_init_v1(vector, bytes + index);
  return vector;
}

void address_copy(void *element) noexcept {
  auto *address = static_cast<LangNetAddress *>(element);
  if (address->data != nullptr) lang_vec_retain_v1(address->data);
}

void address_drop(void *element) noexcept {
  auto *address = static_cast<LangNetAddress *>(element);
  if (address->data != nullptr) lang_vec_release_v1(address->data);
}

bool candidate_add(CandidateList *list, int32_t family,
                   const uint8_t *bytes, uint32_t scope = 0) noexcept {
  const uint64_t length = family == kAfInet ? 4 : 16;
  for (uint64_t index = 0; index < list->count; ++index) {
    if (list->items[index].family != family || list->items[index].scope != scope)
      continue;
    bool same = true;
    for (uint64_t byte = 0; byte < length; ++byte) {
      if (list->items[index].bytes[byte] != bytes[byte]) {
        same = false;
        break;
      }
    }
    if (same) return true;
  }
  if (list->count >= kMaxResults) return false;
  list->items[list->count].family = family;
  list->items[list->count].scope = scope;
  for (uint64_t byte = 0; byte < length; ++byte)
    list->items[list->count].bytes[byte] = bytes[byte];
  for (uint64_t byte = length; byte < 16; ++byte)
    list->items[list->count].bytes[byte] = 0;
  ++list->count;
  return true;
}

void candidate_merge(CandidateList *destination,
                     const CandidateList &source) noexcept {
  for (uint64_t index = 0; index < source.count; ++index) {
    const Candidate &candidate = source.items[index];
    candidate_add(destination, candidate.family, candidate.bytes, candidate.scope);
  }
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t build_result(const CandidateList &list, uint16_t port,
                     LangVec **out) noexcept {
  if (list.count == 0) return kErrNoEnt;
  LangVec *result = lang_vec_new_v1(
      list.count, sizeof(LangNetAddress), address_copy, address_drop);
  for (uint64_t index = 0; index < list.count; ++index) {
    const Candidate &candidate = list.items[index];
    const uint64_t data_length = candidate.family == kAfInet ? 4 : 16;
    LangNetAddress element;
    element.family = candidate.family;
    element.port = port;
    element.flags = 0;
    element.scope = candidate.scope;
    element.reserved = 0;
    element.data = make_bytes_vec(candidate.bytes, data_length);
    lang_vec_init_v1(result, &element);
    address_copy(lang_vec_at_v1(result, result->initialized - 1));
    lang_vec_release_v1(element.data);
  }
  *out = result;
  return 0;
}

void release_address(LangNetAddress *address) noexcept {
  if (address->data != nullptr) {
    lang_vec_release_v1(address->data);
    address->data = nullptr;
  }
}

#if defined(__linux__)
int64_t read_file(const char *path, uint64_t path_length, char *buffer,
                  uint64_t capacity, uint64_t *length_out) noexcept {
  if (capacity < 1) return kErrInval;
  const int64_t fd = lang_os_open_v1(path, path_length, 0, 0);
  if (fd < 0) return fd;

  uint64_t total = 0;
  while (total < capacity - 1) {
    const int64_t received =
        lang_os_read_v1(static_cast<int32_t>(fd), buffer + total,
                        capacity - 1 - total);
    if (received < 0) {
      if (received == kErrEintr) continue;
      lang_os_close_v1(static_cast<int32_t>(fd));
      return received;
    }
    if (received == 0) break;
    total += static_cast<uint64_t>(received);
  }
  const int64_t closed = lang_os_close_v1(static_cast<int32_t>(fd));
  buffer[total] = '\0';
  *length_out = total;
  if (closed < 0) return closed;
  return 0;
}

char *next_token(char **state, char *end) noexcept {
  char *cursor = *state;
  while (cursor < end && (*cursor == ' ' || *cursor == '\t' ||
                          *cursor == '\r' || *cursor == '\0'))
    ++cursor;
  if (cursor >= end) {
    *state = end;
    return nullptr;
  }
  char *start = cursor;
  while (cursor < end && *cursor != ' ' && *cursor != '\t' &&
         *cursor != '\r' && *cursor != '\0')
    ++cursor;
  if (cursor < end) {
    *cursor = '\0';
    ++cursor;
  }
  *state = cursor;
  return start;
}

bool parse_uint(const char *text, uint32_t minimum, uint32_t maximum,
                uint32_t *out) noexcept {
  if (text == nullptr || *text == '\0') return false;
  uint64_t value = 0;
  for (const char *cursor = text; *cursor != '\0'; ++cursor) {
    if (*cursor < '0' || *cursor > '9') return false;
    value = value * 10 + static_cast<uint32_t>(*cursor - '0');
    if (value > maximum) return false;
  }
  if (value < minimum) return false;
  *out = static_cast<uint32_t>(value);
  return true;
}

LANG_NATIVE_NO_STACK_PROTECTOR
bool parse_ip_literal(const char *text, uint64_t length, int32_t *family,
                      uint8_t bytes[16]) noexcept {
  LangNetAddress parsed;
  parsed.data = nullptr;
  int64_t result = lang_net_inet_pton_v1(kAfInet, text, length, &parsed);
  if (result == 0) {
    *family = kAfInet;
    for (uint64_t index = 0; index < 4; ++index)
      bytes[index] = static_cast<uint8_t *>(parsed.data->data)[index];
    release_address(&parsed);
    return true;
  }
  parsed.data = nullptr;
  result = lang_net_inet_pton_v1(kAfInet6, text, length, &parsed);
  if (result == 0) {
    *family = kAfInet6;
    for (uint64_t index = 0; index < 16; ++index)
      bytes[index] = static_cast<uint8_t *>(parsed.data->data)[index];
    release_address(&parsed);
    return true;
  }
  return false;
}
#endif

int64_t normalize_name(const char *data, uint64_t length, char out[256],
                       uint64_t *out_length, bool *absolute) noexcept {
  if (data == nullptr || length == 0) return kErrInval;
  for (uint64_t index = 0; index < length; ++index) {
    if (data[index] == '\0') return kErrInval;
  }

  bool trailing_dot = false;
  if (data[length - 1] == '.') {
    trailing_dot = true;
    --length;
  }
  if (length == 0 || length > kMaxHostnameBytes) return kErrInval;

  uint64_t position = 0;
  uint64_t label_length = 0;
  for (uint64_t index = 0; index < length; ++index) {
    const char byte = data[index];
    if (byte == '.') {
      if (label_length == 0 || label_length > kMaxLabelBytes)
        return kErrInval;
      label_length = 0;
      out[position++] = '.';
    } else {
      if (!is_name_char(byte)) return kErrInval;
      out[position++] = ascii_lower(byte);
      ++label_length;
    }
  }
  if (label_length == 0 || label_length > kMaxLabelBytes) return kErrInval;
  out[position] = '\0';
  *out_length = position;
  if (absolute != nullptr) *absolute = trailing_dot;
  return 0;
}

int64_t parse_dns_name(const uint8_t *message, uint16_t message_length,
                       uint16_t offset, char out[256],
                       uint16_t *next_offset) noexcept {
  uint64_t position = offset;
  uint64_t next = offset;
  uint64_t output_length = 0;
  bool following = false;
  uint32_t jumps = 0;

  for (;;) {
    if (position >= message_length) return kErrInval;
    const uint8_t length = message[position];
    if ((length & 0xc0) == 0xc0) {
      if (position + 1 >= message_length) return kErrInval;
      const uint16_t pointer = static_cast<uint16_t>(
          (static_cast<uint16_t>(length & 0x3f) << 8) | message[position + 1]);
      if (pointer >= position) return kErrInval; // forward/self pointers
      if (++jumps > 128) return kErrInval;
      if (!following) {
        next = position + 2;
        following = true;
      }
      position = pointer;
      continue;
    }
    if ((length & 0xc0) != 0) return kErrInval;
    if (length == 0) {
      if (!following) next = position + 1;
      break;
    }
    if (length > kMaxLabelBytes) return kErrInval;
    if (position + 1 + length > message_length) return kErrInval;
    if (output_length + (output_length == 0 ? 0 : 1) + length >
        kMaxHostnameBytes)
      return kErrInval;
    if (output_length != 0) out[output_length++] = '.';
    for (uint64_t index = 0; index < length; ++index) {
      const char byte = static_cast<char>(message[position + 1 + index]);
      if (!is_name_char(byte)) return kErrInval;
      out[output_length++] = ascii_lower(byte);
    }
    position += 1 + length;
    if (!following) next = position;
  }

  out[output_length] = '\0';
  if (next > message_length) return kErrInval;
  *next_offset = static_cast<uint16_t>(next);
  return 0;
}

int64_t encode_name(const char *name, uint8_t *out, uint64_t capacity,
                    uint64_t *out_length) noexcept {
  uint64_t position = 0;
  uint64_t index = 0;
  while (name[index] != '\0') {
    const uint64_t label_start = index;
    while (name[index] != '\0' && name[index] != '.') ++index;
    const uint64_t label_length = index - label_start;
    if (label_length == 0 || label_length > kMaxLabelBytes) return kErrInval;
    if (position + 1 + label_length + 1 > capacity) return kErrInval;
    if (position + 1 + label_length > 255) return kErrInval;
    out[position++] = static_cast<uint8_t>(label_length);
    for (uint64_t byte = 0; byte < label_length; ++byte)
      out[position++] = static_cast<uint8_t>(name[label_start + byte]);
    if (name[index] == '.') ++index;
  }
  if (position + 1 > capacity) return kErrInval;
  out[position++] = 0;
  *out_length = position;
  return 0;
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t build_query(uint16_t id, const char *name, int32_t family,
                    uint8_t *buffer, uint64_t capacity,
                    uint64_t *length_out) noexcept {
  if (capacity < 12) return kErrInval;
  write_u16_be(buffer, id);
  write_u16_be(buffer + 2, kDnsFlagRd);
  write_u16_be(buffer + 4, 1);
  write_u16_be(buffer + 6, 0);
  write_u16_be(buffer + 8, 0);
  write_u16_be(buffer + 10, 0);

  uint64_t name_length = 0;
  const int64_t encoded =
      encode_name(name, buffer + 12, capacity - 12, &name_length);
  if (encoded < 0) return encoded;
  const uint64_t position = 12 + name_length;
  if (position + 4 > capacity) return kErrInval;
  const uint16_t qtype = family == kAfInet ? kDnsTypeA : kDnsTypeAaaa;
  write_u16_be(buffer + position, qtype);
  write_u16_be(buffer + position + 2, kDnsClassIn);
  *length_out = position + 4;
  return 0;
}

int64_t rcode_error(uint16_t rcode) noexcept {
  switch (rcode) {
  case kDnsRcodeNoError:
    return 0;
  case kDnsRcodeFormErr:
    return kErrInval;
  case kDnsRcodeServFail:
    return kErrIo;
  case kDnsRcodeNxDomain:
    return kErrNoEnt;
  case kDnsRcodeNotImp:
    return kErrNoSys;
  case kDnsRcodeRefused:
    return kErrAcces;
  default:
    return kErrProto;
  }
}

struct CnameEdge {
  char owner[256];
  char target[256];
};

struct AddressRecord {
  char owner[256];
  int32_t family;
  uint8_t bytes[16];
};

struct ParsedAnswer {
  CandidateList addresses;
  bool has_target;
  bool truncated;
  uint64_t cname_hops;
  char target[256];
  char chain[kMaxCnameHops+1][256];
  uint64_t chain_count;
};

void init_parsed_answer(ParsedAnswer *answer) noexcept {
  answer->addresses.count = 0;
  answer->has_target = false;
  answer->truncated = false;
  answer->cname_hops = 0;
  answer->chain_count = 0;
  answer->target[0] = '\0';
}

int find_cname_edge(const CnameEdge *edges, uint64_t count,
                    const char *owner) noexcept {
  for (uint64_t index = 0; index < count; ++index) {
    if (same_text(edges[index].owner, owner)) return static_cast<int>(index);
  }
  return -1;
}

// Validates every CNAME edge in the packet, not only the chain reachable from
// the question. A name with a CNAME must not also carry A/AAAA data, and the
// packet graph must not contain a cycle.
LANG_NATIVE_NO_STACK_PROTECTOR
int64_t validate_cname_graph(const CnameEdge *edges, uint64_t edge_count,
                             const AddressRecord *records,
                             uint64_t record_count) noexcept {
  for (uint64_t edge = 0; edge < edge_count; ++edge) {
    for (uint64_t record = 0; record < record_count; ++record) {
      if (same_text(edges[edge].owner, records[record].owner))
        return kErrInval;
    }
  }

  for (uint64_t start = 0; start < edge_count; ++start) {
    char visited[kMaxCnameRecords + 1][256];
    uint64_t visited_count = 0;
    const char *current = edges[start].owner;
    for (uint64_t step = 0; step <= edge_count; ++step) {
      for (uint64_t index = 0; index < visited_count; ++index) {
        if (same_text(visited[index], current)) return kErrInval;
      }
      if (visited_count >= kMaxCnameRecords + 1) return kErrInval;
      copy_string(visited[visited_count++], current, text_length(current));
      const int edge = find_cname_edge(edges, edge_count, current);
      if (edge < 0) break;
      current = edges[static_cast<uint64_t>(edge)].target;
    }
  }
  return 0;
}

// Parses and fully validates one matching response. Candidate addresses are
// collected locally and are not exposed to the caller unless the complete
// packet (including all later RRs and the CNAME graph) validates. A packet
// whose CNAME chain ends at a name with no expected-family address reports
// that final target for a follow-up query.
LANG_NATIVE_NO_STACK_PROTECTOR
int64_t parse_response_packet(const uint8_t *message, uint16_t message_length,
                              uint16_t query_id, const char *query_name,
                              int32_t family, bool allow_truncation,
                              ParsedAnswer *answer) noexcept {
  init_parsed_answer(answer);
  if (message_length < 12) return kErrInval;
  if (read_u16_be(message) != query_id) return kErrInval;
  const uint16_t flags = read_u16_be(message + 2);
  if ((flags & kDnsFlagQr) == 0) return kErrInval;
  if (((flags >> 11) & 0x0f) != 0) return kErrInval;
  if (read_u16_be(message + 4) != 1) return kErrInval;
  const uint16_t answer_count = read_u16_be(message + 6);

  char question[256];
  uint16_t question_next = 0;
  int64_t parsed = parse_dns_name(message, message_length, 12, question,
                                  &question_next);
  if (parsed < 0) return parsed;
  if (!same_text(question, query_name)) return kErrInval;
  if (static_cast<uint32_t>(question_next) + 4 > message_length)
    return kErrInval;
  const uint16_t qtype = read_u16_be(message + question_next);
  const uint16_t qclass = read_u16_be(message + question_next + 2);
  const uint16_t expected_type =
      family == kAfInet ? kDnsTypeA : kDnsTypeAaaa;
  if (qtype != expected_type || qclass != kDnsClassIn) return kErrInval;

  if ((flags & kDnsFlagTc) != 0) {
    if (!allow_truncation) return kErrInval;
    answer->truncated = true;
    return 0;
  }

  const int64_t rcode = rcode_error(static_cast<uint16_t>(flags & 0x0f));
  if (rcode != 0) return rcode;

  CnameEdge edges[kMaxCnameRecords];
  uint64_t edge_count = 0;
  AddressRecord records[kMaxAddressRecords];
  uint64_t record_count = 0;
  uint64_t offset = static_cast<uint64_t>(question_next) + 4;

  // First pass: collect and validate every answer RR without accepting any
  // address. This permits A(target) records before CNAME(query -> target).
  for (uint16_t answer_index = 0; answer_index < answer_count;
       ++answer_index) {
    char owner[256];
    uint16_t owner_next = 0;
    parsed = parse_dns_name(message, message_length,
                            static_cast<uint16_t>(offset), owner,
                            &owner_next);
    if (parsed < 0) return parsed;
    if (static_cast<uint32_t>(owner_next) + 10 > message_length)
      return kErrInval;

    const uint16_t type = read_u16_be(message + owner_next);
    const uint16_t record_class = read_u16_be(message + owner_next + 2);
    const uint16_t rdata_length = read_u16_be(message + owner_next + 8);
    const uint64_t rdata_offset = static_cast<uint64_t>(owner_next) + 10;
    const uint64_t rdata_end = rdata_offset + rdata_length;
    if (rdata_end > message_length) return kErrInval;

    if (record_class == kDnsClassIn) {
      if (type == kDnsTypeCname) {
        char target[256];
        uint16_t target_next = 0;
        parsed = parse_dns_name(message, message_length,
                                static_cast<uint16_t>(rdata_offset), target,
                                &target_next);
        if (parsed < 0) return parsed;
        if (target_next != rdata_end) return kErrInval;
        if (target[0] == '\0' || same_text(target, owner)) return kErrInval;

        const int existing = find_cname_edge(edges, edge_count, owner);
        if (existing >= 0) {
          if (!same_text(edges[static_cast<uint64_t>(existing)].target,
                         target))
            return kErrInval;
        } else {
          if (edge_count >= kMaxCnameRecords) return kErrInval;
          copy_string(edges[edge_count].owner, owner, text_length(owner));
          copy_string(edges[edge_count].target, target, text_length(target));
          ++edge_count;
        }
      } else if (type == kDnsTypeA || type == kDnsTypeAaaa) {
        const uint16_t expected_rdata = type == kDnsTypeA ? 4 : 16;
        if (rdata_length != expected_rdata) return kErrInval;
        if (record_count < kMaxAddressRecords) {
          copy_string(records[record_count].owner, owner, text_length(owner));
          records[record_count].family =
              type == kDnsTypeA ? kAfInet : kAfInet6;
          const uint64_t byte_count = type == kDnsTypeA ? 4 : 16;
          for (uint64_t byte = 0; byte < byte_count; ++byte)
            records[record_count].bytes[byte] = message[rdata_offset + byte];
          ++record_count;
        }
      }
    }
    offset = rdata_end;
  }

  // Second pass: validate the whole packet CNAME graph, then walk only the
  // chain rooted at the question. Unrelated address records are ignored.
  parsed = validate_cname_graph(edges, edge_count, records, record_count);
  if (parsed < 0) return parsed;

  char current[256];
  copy_string(current, query_name, text_length(query_name));
  uint64_t cname_hops = 0;
  for (;;) {
    if (answer->chain_count >= kMaxCnameHops+1) return kErrInval;
    copy_string(answer->chain[answer->chain_count++],current,text_length(current));
    bool has_address = false;
    for (uint64_t record = 0; record < record_count; ++record) {
      if (records[record].family == family &&
          same_text(records[record].owner, current)) {
        candidate_add(&answer->addresses, records[record].family,
                      records[record].bytes);
        has_address = true;
      }
    }
    if (has_address) {
      answer->cname_hops = cname_hops;
      return 0;
    }

    const int edge = find_cname_edge(edges, edge_count, current);
    if (edge < 0) {
      if (same_text(current, query_name)) return kErrNoEnt;
      answer->has_target = true;
      answer->cname_hops = cname_hops;
      copy_string(answer->target, current, text_length(current));
      return 0;
    }
    if (cname_hops >= kMaxCnameHops) return kErrInval;
    ++cname_hops;
    copy_string(current, edges[static_cast<uint64_t>(edge)].target,
                text_length(edges[static_cast<uint64_t>(edge)].target));
  }
}

// Compatibility packet-only view used by the review parser probes. Production
// uses parse_response_packet so it can follow a CNAME target across packets.
// A CNAME-only packet is reported as -ENOENT here because this wrapper cannot
// issue the follow-up query.
[[maybe_unused]] LANG_NATIVE_NO_STACK_PROTECTOR
int64_t parse_response(const uint8_t *message, uint16_t message_length,
                       uint16_t query_id, const char *query_name,
                       int32_t family, bool allow_truncation,
                       CandidateList *candidates,
                       bool *truncated_out) noexcept {
  ParsedAnswer answer;
  const int64_t result =
      parse_response_packet(message, message_length, query_id, query_name,
                            family, allow_truncation, &answer);
  if (result < 0) return result;
  *truncated_out = answer.truncated;
  if (answer.truncated) return 0;
  candidate_merge(candidates, answer.addresses);
  if (answer.has_target) return kErrNoEnt;
  return candidates->count == 0 ? kErrNoEnt : 0;
}

struct FdGuard {
  int64_t fd;
  ~FdGuard() {
    if (fd >= 0) lang_os_close_v1(static_cast<int32_t>(fd));
  }
};

struct BufferGuard {
  uint8_t *data;
  ~BufferGuard() {
    if (data != nullptr) lang_free_v1(data);
  }
};

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t monotonic_ns(int64_t *out) noexcept {
  LangTimespec time;
  const int64_t result = lang_sys_clock_gettime_v1(1, &time);
  if (result < 0) return result;
  *out = time.seconds * INT64_C(1000000000) + time.nanoseconds;
  return 0;
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t remaining_ms(int64_t deadline) noexcept {
  int64_t now = 0;
  const int64_t clock_result = monotonic_ns(&now);
  if (clock_result < 0) return clock_result;
  const int64_t remaining = deadline - now;
  if (remaining <= 0) return 0;
  int64_t milliseconds = (remaining + 999999) / 1000000;
  if (milliseconds > 0x7fffffff) milliseconds = 0x7fffffff;
  return milliseconds;
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t poll_wait(int64_t fd, int16_t events, int64_t deadline) noexcept {
  for (;;) {
    const int64_t milliseconds = remaining_ms(deadline);
    if (milliseconds < 0) return milliseconds;
    if (milliseconds == 0) return kErrTimeout;
    LangPollFd row = {static_cast<int32_t>(fd), events, 0};
    const int64_t result =
        lang_net_poll_v1(&row, 1, static_cast<int32_t>(milliseconds));
    if (result < 0) {
      if (result == kErrEintr) continue;
      return result;
    }
    if (result == 0) return kErrTimeout;
    if ((row.revents & kPollNval) != 0) return kErrEbadf;
    return row.revents;
  }
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t connect_socket(int64_t fd, const LangNetAddress *server,
                       int64_t deadline) noexcept {
  int64_t result =
      lang_net_connect_v1(static_cast<int32_t>(fd), server);
  if (result == 0 || result == kErrIsConn) return 0;
  if (result != kErrInProgress && result != kErrAlready) return result;

  const int64_t events = poll_wait(fd, kPollOut, deadline);
  if (events < 0) return events;
  int32_t error = 0;
  result = lang_net_getsockopt_int_v1(static_cast<int32_t>(fd), kSolSocket,
                                      kSoError, &error);
  if (result < 0) return result;
  if (error != 0) return -static_cast<int64_t>(error);
  return 0;
}

int64_t send_datagram(int64_t fd, const uint8_t *data, uint64_t length,
                      int64_t deadline) noexcept {
  for (;;) {
    const int64_t result = lang_net_send_v1(
        static_cast<int32_t>(fd), data, length, 0);
    if (result == static_cast<int64_t>(length)) return 0;
    if (result >= 0) return kErrIo;
    if (result == kErrEintr) continue;
    if (result == kErrEagain) {
      const int64_t events = poll_wait(fd, kPollOut, deadline);
      if (events < 0) return events;
      continue;
    }
    return result;
  }
}

int64_t recv_datagram(int64_t fd, uint8_t *buffer, uint64_t capacity,
                      int64_t deadline) noexcept {
  for (;;) {
    const int64_t result = lang_net_recv_v1(
        static_cast<int32_t>(fd), buffer, capacity, 0);
    if (result > 0) return result;
    if (result == 0) return kErrIo;
    if (result == kErrEintr) continue;
    if (result == kErrEagain) {
      const int64_t events = poll_wait(fd, kPollIn, deadline);
      if (events < 0) return events;
      continue;
    }
    return result;
  }
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t send_all(int64_t fd, const uint8_t *data, uint64_t length,
                 int64_t deadline) noexcept {
  uint64_t offset = 0;
  while (offset < length) {
    const int64_t result = lang_net_send_v1(
        static_cast<int32_t>(fd), data + offset, length - offset, 0);
    if (result > 0) {
      offset += static_cast<uint64_t>(result);
      continue;
    }
    if (result == 0) return kErrIo;
    if (result == kErrEintr) continue;
    if (result == kErrEagain) {
      const int64_t events = poll_wait(fd, kPollOut, deadline);
      if (events < 0) return events;
      continue;
    }
    return result;
  }
  return 0;
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t recv_exact(int64_t fd, uint8_t *data, uint64_t length,
                   int64_t deadline) noexcept {
  uint64_t offset = 0;
  while (offset < length) {
    const int64_t result = lang_net_recv_v1(
        static_cast<int32_t>(fd), data + offset, length - offset, 0);
    if (result > 0) {
      offset += static_cast<uint64_t>(result);
      continue;
    }
    if (result == 0) return kErrConnReset;
    if (result == kErrEintr) continue;
    if (result == kErrEagain) {
      const int64_t events = poll_wait(fd, kPollIn, deadline);
      if (events < 0) return events;
      continue;
    }
    return result;
  }
  return 0;
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t dns_exchange(const char *name, int32_t family,
                     const LangNetAddress *server, int64_t deadline,
                     ParsedAnswer *answer) noexcept {
  init_parsed_answer(answer);

  uint8_t id_bytes[2];
  // Darwin getentropy has no flag argument.
#if defined(__APPLE__)
  constexpr uint32_t entropy_flags = 0;
#else
  // GRND_NONBLOCK avoids an indefinite early-boot entropy wait.
  constexpr uint32_t entropy_flags = 1;
#endif
  int64_t result =
      lang_sys_getrandom_v1(id_bytes, sizeof id_bytes, entropy_flags);
  if (result != static_cast<int64_t>(sizeof id_bytes))
    return result < 0 ? result : kErrIo;
  const uint16_t id = static_cast<uint16_t>(
      (static_cast<uint16_t>(id_bytes[0]) << 8) | id_bytes[1]);

  uint8_t query[kMaxQueryBytes];
  uint64_t query_length = 0;
  result = build_query(id, name, family, query, sizeof query, &query_length);
  if (result < 0) return result;

  auto *response =
      static_cast<uint8_t *>(lang_alloc_v1(kMaxDnsMessage, 1));
  if (response == nullptr) return kErrNoMem;
  BufferGuard response_guard{response};

  {
    FdGuard udp{lang_net_socket_v1(
        server->family, kSockDgram | kSockNonblock | kSockCloexec, 0)};
    if (udp.fd < 0) return udp.fd;

    result = connect_socket(udp.fd, server, deadline);
    if (result < 0) return result;
    result = send_datagram(udp.fd, query, query_length, deadline);
    if (result < 0) return result;

    const int64_t response_length =
        recv_datagram(udp.fd, response, kMaxDnsMessage, deadline);
    if (response_length < 0) return response_length;
    if (static_cast<uint64_t>(response_length) > kMaxDnsMessage)
      return kErrInval;

    result = parse_response_packet(response,
                                   static_cast<uint16_t>(response_length), id,
                                   name, family, true, answer);
    if (result < 0) return result;
    if (!answer->truncated) return 0;
  }

  FdGuard tcp{lang_net_socket_v1(
      server->family, kSockStream | kSockNonblock | kSockCloexec, 0)};
  if (tcp.fd < 0) return tcp.fd;
  result = connect_socket(tcp.fd, server, deadline);
  if (result < 0) return result;

  const uint8_t prefix[2] = {
      static_cast<uint8_t>(query_length >> 8),
      static_cast<uint8_t>(query_length)};
  result = send_all(tcp.fd, prefix, sizeof prefix, deadline);
  if (result < 0) return result;
  result = send_all(tcp.fd, query, query_length, deadline);
  if (result < 0) return result;

  uint8_t length_prefix[2];
  result = recv_exact(tcp.fd, length_prefix, sizeof length_prefix, deadline);
  if (result < 0) return result;
  const uint16_t response_length = read_u16_be(length_prefix);
  if (response_length < 12) return kErrInval;
  result = recv_exact(tcp.fd, response, response_length, deadline);
  if (result < 0) return result;

  result = parse_response_packet(response, response_length, id, name, family,
                                 false, answer);
  if (result < 0) return result;
  if (answer->truncated) return kErrInval;
  return 0;
}

// Follows CNAME targets across packets using the same deadline. `visited`
// prevents a chain from cycling across packets; total_hops enforces the
// eight-hop budget even when each packet is individually valid.
LANG_NATIVE_NO_STACK_PROTECTOR
int64_t dns_resolve_chain(const char *name, int32_t family,
                          const LangNetAddress *server, int64_t deadline,
                          CandidateList *out) noexcept {
  char current[256];
  copy_string(current, name, text_length(name));
  char visited[kMaxCnameHops + 2][256];
  uint64_t visited_count = 0;
  copy_string(visited[visited_count++], current, text_length(current));
  uint64_t total_hops = 0;

  for (;;) {
    ParsedAnswer answer;
    const int64_t result =
        dns_exchange(current, family, server, deadline, &answer);
    if (result < 0) return result;

    total_hops += answer.cname_hops;
    if (total_hops > kMaxCnameHops) return kErrInval;

    // Check every name from this packet before accepting terminal addresses.
    for (uint64_t step=1; step<answer.chain_count; ++step) {
      for (uint64_t index=0; index<visited_count; ++index)
        if (same_text(visited[index],answer.chain[step])) return kErrInval;
      if (visited_count >= kMaxCnameHops+2) return kErrInval;
      copy_string(visited[visited_count++],answer.chain[step],text_length(answer.chain[step]));
    }
    if (answer.addresses.count > 0) {
      candidate_merge(out, answer.addresses);
      return out->count > 0 ? 0 : kErrNoEnt;
    }
    if (!answer.has_target) return kErrNoEnt;

    copy_string(current, answer.target, text_length(answer.target));
  }
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t query_name_families(const char *name, int32_t family,
                            const LangNetAddress *server, int32_t timeout_ms,
                            CandidateList *out) noexcept {
  int64_t now = 0;
  int64_t result = monotonic_ns(&now);
  if (result < 0) return result;
  const int64_t deadline =
      now + static_cast<int64_t>(timeout_ms) * 1000000;

  if (family == kAfInet || family == kAfInet6)
    return dns_resolve_chain(name, family, server, deadline, out);
  if (family != 0) return kErrAfNoSupport;

  CandidateList ipv4;
  ipv4.count = 0;
  CandidateList ipv6;
  ipv6.count = 0;
  const int64_t ipv4_result =
      dns_resolve_chain(name, kAfInet, server, deadline, &ipv4);
  const int64_t ipv6_result =
      dns_resolve_chain(name, kAfInet6, server, deadline, &ipv6);

  // Merge only families whose complete response/chains validated. A malformed
  // packet must not contribute any address, even if another family produced a
  // usable answer.
  if (ipv4_result == 0) candidate_merge(out, ipv4);
  if (ipv6_result == 0) candidate_merge(out, ipv6);
  if (out->count > 0) return 0;
  if (ipv4_result == kErrNoEnt && ipv6_result == kErrNoEnt) return kErrNoEnt;
  if (ipv4_result < 0 && ipv4_result != kErrNoEnt) return ipv4_result;
  if (ipv6_result < 0 && ipv6_result != kErrNoEnt) return ipv6_result;
  return kErrNoEnt;
}

bool valid_server(const LangNetAddress *server) noexcept {
  if (server == nullptr) return false;
  if (server->family != kAfInet && server->family != kAfInet6) return false;
  if (server->reserved != 0 || server->flags != 0) return false;
  if (server->data == nullptr) return false;
  LangVec *data = server->data;
  if (data->stride != 1 || data->initialized != data->length) return false;
  if (data->copy != nullptr || data->drop != nullptr) return false;
  const uint64_t expected = server->family == kAfInet ? 4 : 16;
  if (data->length != expected || data->data == nullptr) return false;
  return true;
}

int64_t host_view(const LangText *hostname, const char **data,
                  uint64_t *length, bool *absolute) noexcept {
  if (hostname == nullptr) return kErrInval;
  if (hostname->data == nullptr || hostname->bytes == 0) return kErrInval;
  for (uint64_t index = 0; index < hostname->bytes; ++index) {
    if (hostname->data[index] == 0) return kErrInval;
  }
  uint64_t view_length = hostname->bytes;
  bool trailing_dot = false;
  if (hostname->data[view_length - 1] == static_cast<uint8_t>('.')) {
    trailing_dot = true;
    --view_length;
  }
  if (view_length == 0) return kErrInval;
  *data = reinterpret_cast<const char *>(hostname->data);
  *length = view_length;
  *absolute = trailing_dot;
  return 0;
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t try_numeric(const char *text, uint64_t length, int32_t family,
                    uint16_t port, LangVec **out) noexcept {
  const int32_t families[] = {kAfInet, kAfInet6};
  LangNetAddress addresses[2];
  uint64_t count = 0;
  bool found = false;
  for (int32_t candidate : families) {
    LangNetAddress address;
    if (lang_net_inet_pton_v1(candidate, text, length, &address) != 0)
      continue;
    found = true;
    if (family == 0 || family == candidate) {
      address.port = port;
      addresses[count++] = address;
    } else {
      release_address(&address);
    }
  }
  if (count == 0) return found ? kErrNoEnt : 0;

  LangVec *result = lang_vec_new_v1(
      count, sizeof(LangNetAddress), address_copy, address_drop);
  // Initialization takes ownership of each parser-produced byte Vec.
  for (uint64_t index = 0; index < count; ++index)
    lang_vec_init_v1(result, &addresses[index]);
  *out = result;
  return 1;
}

LANG_NATIVE_NO_STACK_PROTECTOR
void add_loopback_candidates(int32_t family, CandidateList *list) noexcept {
  const uint8_t ipv4[4] = {127, 0, 0, 1};
  const uint8_t ipv6[16] = {0, 0, 0, 0, 0, 0, 0, 0,
                            0, 0, 0, 0, 0, 0, 0, 1};
  if (family == 0 || family == kAfInet)
    candidate_add(list, kAfInet, ipv4);
  if (family == 0 || family == kAfInet6)
    candidate_add(list, kAfInet6, ipv6);
}

#if defined(__APPLE__)
int64_t resolve_system_name(const char *name, int32_t family,
                            CandidateList *list) noexcept {
  addrinfo hints{};
  hints.ai_family = family == kAfInet ? AF_INET :
                    family == kAfInet6 ? AF_INET6 : AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *addresses = nullptr;
  const lang::native::gc_program::SafeGuard safe;
  switch (getaddrinfo(name, nullptr, &hints, &addresses)) {
    case 0: break;
    case EAI_ADDRFAMILY:
    case EAI_NODATA:
    case EAI_NONAME: return kErrNoEnt;
    case EAI_AGAIN: return kErrEagain;
    case EAI_MEMORY: return kErrNoMem;
    case EAI_FAMILY: return kErrAfNoSupport;
    case EAI_SYSTEM: return lang::native::darwin::error(errno);
    default: return kErrIo;
  }

  for (const auto *entry = addresses; entry && list->count < kMaxResults;
       entry = entry->ai_next) {
    if (!entry->ai_addr) continue;
    if (entry->ai_family == AF_INET && family != kAfInet6 &&
        entry->ai_addrlen >= sizeof(sockaddr_in)) {
      sockaddr_in address;
      std::memcpy(&address, entry->ai_addr, sizeof(address));
      candidate_add(list, kAfInet,
                    reinterpret_cast<const uint8_t *>(&address.sin_addr));
    } else if (entry->ai_family == AF_INET6 && family != kAfInet &&
               entry->ai_addrlen >= sizeof(sockaddr_in6)) {
      sockaddr_in6 address;
      std::memcpy(&address, entry->ai_addr, sizeof(address));
      candidate_add(list, kAfInet6,
                    reinterpret_cast<const uint8_t *>(&address.sin6_addr),
                    address.sin6_scope_id);
    }
  }
  if (addresses) freeaddrinfo(addresses);
  return 0;
}
#else
bool alias_matches(const char *alias, const char *name) noexcept {
  uint64_t alias_length = text_length(alias);
  if (alias_length != 0 && alias[alias_length - 1] == '.') --alias_length;
  const uint64_t name_length = text_length(name);
  if (alias_length != name_length) return false;
  for (uint64_t index = 0; index < alias_length; ++index) {
    if (ascii_lower(alias[index]) != name[index]) return false;
  }
  return true;
}

LANG_NATIVE_NO_STACK_PROTECTOR
void parse_hosts_buffer(char *buffer, uint64_t length, bool truncated,
                        const char *name, int32_t family,
                        CandidateList *list) noexcept {
  char *cursor = buffer;
  char *end = buffer + length;
  while (cursor < end) {
    char *newline = cursor;
    while (newline < end && *newline != '\n') ++newline;
    const bool complete = newline < end || !truncated;
    if (!complete) break;
    char *line_end = newline;
    if (newline < end) {
      *newline = '\0';
      newline += 1;
    }
    if (line_end > cursor && line_end[-1] == '\r') line_end[-1] = '\0';
    for (char *comment = cursor; comment < line_end && *comment != '\0';
         ++comment) {
      if (*comment == '#') {
        *comment = '\0';
        line_end = comment;
        break;
      }
    }

    char *state = cursor;
    char *ip = next_token(&state, line_end);
    if (ip != nullptr) {
      int32_t ip_family = 0;
      uint8_t bytes[16];
      if (parse_ip_literal(ip, text_length(ip), &ip_family, bytes)) {
        for (;;) {
          char *alias = next_token(&state, line_end);
          if (alias == nullptr) break;
          if ((family == 0 || family == ip_family) &&
              alias_matches(alias, name))
            candidate_add(list, ip_family, bytes);
        }
      }
    }

    cursor = newline;
  }
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t parse_hosts(const char *name, int32_t family,
                   CandidateList *list) noexcept {
  static constexpr char kHostsPath[] = "/etc/hosts";
  char buffer[kHostsBufferBytes];
  uint64_t length = 0;
  const int64_t result =
      read_file(kHostsPath, sizeof(kHostsPath) - 1, buffer, sizeof buffer,
                &length);
  if (result == kErrNoEnt) return 0;
  if (result < 0) return result;
  parse_hosts_buffer(buffer, length, length == sizeof(buffer) - 1, name,
                     family, list);
  return 0;
}

bool starts_with(const char *text, const char *prefix) noexcept {
  uint64_t index = 0;
  while (prefix[index] != '\0') {
    if (text[index] != prefix[index]) return false;
    ++index;
  }
  return true;
}

LANG_NATIVE_NO_STACK_PROTECTOR
void parse_resolv_buffer(char *buffer, uint64_t length, bool truncated,
                         ResolverConfig *config) noexcept {
  char *cursor = buffer;
  char *end = buffer + length;
  while (cursor < end) {
    char *newline = cursor;
    while (newline < end && *newline != '\n') ++newline;
    const bool complete = newline < end || !truncated;
    if (!complete) break;
    char *line_end = newline;
    if (newline < end) {
      *newline = '\0';
      newline += 1;
    }
    if (line_end > cursor && line_end[-1] == '\r') line_end[-1] = '\0';
    for (char *comment = cursor; comment < line_end && *comment != '\0';
         ++comment) {
      if (*comment == '#') {
        *comment = '\0';
        line_end = comment;
        break;
      }
    }

    char *state = cursor;
    char *keyword = next_token(&state, line_end);
    if (keyword == nullptr) {
      cursor = newline;
      continue;
    }

    if (same_text(keyword, "nameserver")) {
      char *address = next_token(&state, line_end);
      if (address != nullptr && config->server_count < kMaxNameservers) {
        int32_t family = 0;
        uint8_t bytes[16];
        if (parse_ip_literal(address, text_length(address), &family, bytes)) {
          const uint64_t index = config->server_count;
          config->server_family[index] = family;
          const uint64_t address_length = family == kAfInet ? 4 : 16;
          for (uint64_t byte = 0; byte < address_length; ++byte)
            config->server_bytes[index][byte] = bytes[byte];
          for (uint64_t byte = address_length; byte < 16; ++byte)
            config->server_bytes[index][byte] = 0;
          config->server_count += 1;
        }
      }
    } else if (same_text(keyword, "search") ||
               same_text(keyword, "domain")) {
      config->search_count = 0;
      for (;;) {
        char *domain = next_token(&state, line_end);
        if (domain == nullptr) break;
        if (config->search_count >= kMaxSearchDomains) continue;
        char normalized[256];
        uint64_t normalized_length = 0;
        bool absolute = false;
        if (normalize_name(domain, text_length(domain), normalized,
                           &normalized_length, &absolute) == 0) {
          copy_string(config->search[config->search_count], normalized,
                      normalized_length);
          config->search_count += 1;
        }
        if (same_text(keyword, "domain")) break;
      }
    } else if (same_text(keyword, "options")) {
      for (;;) {
        char *option = next_token(&state, line_end);
        if (option == nullptr) break;
        if (starts_with(option, "timeout:")) {
          uint32_t value = 0;
          if (parse_uint(option + 8, 1, kMaxTimeoutSeconds, &value))
            config->timeout_seconds = value;
        } else if (starts_with(option, "attempts:")) {
          uint32_t value = 0;
          if (parse_uint(option + 9, 1, kMaxAttempts, &value))
            config->attempts = value;
        } else if (starts_with(option, "ndots:")) {
          uint32_t value = 0;
          if (parse_uint(option + 6, 0, kMaxNdots, &value))
            config->ndots = value;
        }
      }
    }

    cursor = newline;
  }
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t parse_resolv(ResolverConfig *config) noexcept {
  static constexpr char kResolvPath[] = "/etc/resolv.conf";
  config->server_count = 0;
  config->search_count = 0;
  config->timeout_seconds = kDefaultTimeoutSeconds;
  config->attempts = kDefaultAttempts;
  config->ndots = kDefaultNdots;

  char buffer[kResolvBufferBytes];
  uint64_t length = 0;
  const int64_t result =
      read_file(kResolvPath, sizeof(kResolvPath) - 1, buffer, sizeof buffer,
                &length);
  if (result < 0) return result;
  parse_resolv_buffer(buffer, length, length == sizeof(buffer) - 1, config);
  if (config->server_count == 0) return kErrInval;
  return 0;
}

bool add_candidate_name(char candidates[5][256], uint64_t *count,
                        const char *name) noexcept {
  for (uint64_t index = 0; index < *count; ++index) {
    if (same_text(candidates[index], name)) return true;
  }
  if (*count >= 5) return false;
  copy_string(candidates[*count], name, text_length(name));
  *count += 1;
  return true;
}

LANG_NATIVE_NO_STACK_PROTECTOR
void build_search_candidates(const char *name, bool absolute,
                             const ResolverConfig &config,
                             char candidates[5][256],
                             uint64_t *count) noexcept {
  *count = 0;
  if (absolute) {
    add_candidate_name(candidates, count, name);
    return;
  }

  uint64_t dots = 0;
  for (const char *cursor = name; *cursor != '\0'; ++cursor) {
    if (*cursor == '.') ++dots;
  }
  const bool absolute_first = dots >= config.ndots;

  if (absolute_first) add_candidate_name(candidates, count, name);
  for (uint64_t index = 0; index < config.search_count; ++index) {
    const uint64_t name_length = text_length(name);
    const uint64_t domain_length = text_length(config.search[index]);
    if (name_length + 1 + domain_length > kMaxHostnameBytes) continue;
    char combined[256];
    copy_string(combined, name, name_length);
    combined[name_length] = '.';
    copy_string(combined + name_length + 1, config.search[index],
                domain_length);
    add_candidate_name(candidates, count, combined);
  }
  if (!absolute_first) add_candidate_name(candidates, count, name);
}
#endif

} // namespace

extern "C" {

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_net_dns_query_v1(const LangText *hostname, int32_t family,
                              const LangNetAddress *server,
                              int32_t timeout_ms, LangVec **out) noexcept {
  if (out == nullptr) return kErrFault;
  if (family != 0 && family != kAfInet && family != kAfInet6)
    return kErrAfNoSupport;
  if (timeout_ms <= 0) return kErrInval;
  if (!valid_server(server)) {
    if (server != nullptr &&
        (server->family == kAfInet || server->family == kAfInet6))
      return kErrInval;
    return kErrInval;
  }

  char name[256];
  uint64_t name_length = 0;
  bool absolute = false;
  const int64_t normalized =
      hostname == nullptr
          ? kErrInval
          : normalize_name(reinterpret_cast<const char *>(hostname->data),
                           hostname->bytes, name, &name_length, &absolute);
  if (normalized < 0) return normalized;

  CandidateList candidates;
  candidates.count = 0;
  const int64_t result = query_name_families(
      name, family, server, timeout_ms, &candidates);
  if (result < 0) return result;
  return build_result(candidates, 0, out);
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_net_lookup_v1(const LangText *hostname, uint16_t port,
                           int32_t family, LangVec **out) noexcept {
  if (out == nullptr) return kErrFault;
  if (family != 0 && family != kAfInet && family != kAfInet6)
    return kErrAfNoSupport;

  const char *raw_name = nullptr;
  uint64_t raw_length = 0;
  bool raw_absolute = false;
  int64_t result =
      host_view(hostname, &raw_name, &raw_length, &raw_absolute);
  if (result < 0) return result;

  result = try_numeric(raw_name, raw_length, family, port, out);
  if (result != 0) return result < 0 ? result : 0;

  CandidateList candidates;
  candidates.count = 0;
  if (equal_ignore_case(raw_name, raw_length, "localhost", 9)) {
    add_loopback_candidates(family, &candidates);
    return build_result(candidates, port, out);
  }

  char name[256];
  uint64_t name_length = 0;
  bool absolute = false;
  result = normalize_name(reinterpret_cast<const char *>(hostname->data),
                          hostname->bytes, name, &name_length, &absolute);
  if (result < 0) return result;

#if defined(__APPLE__)
  // Preserve absolute names when handing search policy to the system resolver.
  if (absolute) {
    name[name_length] = '.';
    name[name_length + 1] = '\0';
  }
  result = resolve_system_name(name, family, &candidates);
  return result < 0 ? result : build_result(candidates, port, out);
#else
  result = parse_hosts(name, family, &candidates);
  if (result < 0) return result;
  if (candidates.count != 0) return build_result(candidates, port, out);

  ResolverConfig config;
  result = parse_resolv(&config);
  if (result < 0) return result;

  char search_candidates[5][256];
  uint64_t search_count = 0;
  build_search_candidates(name, absolute, config, search_candidates,
                          &search_count);
  if (search_count == 0) return kErrInval;

  int64_t last_error = kErrNoEnt;
  for (uint64_t candidate_index = 0; candidate_index < search_count;
       ++candidate_index) {
    for (uint32_t attempt = 0; attempt < config.attempts; ++attempt) {
      for (uint64_t server_index = 0; server_index < config.server_count;
           ++server_index) {
        const uint64_t address_length =
            config.server_family[server_index] == kAfInet ? 4 : 16;
        LangVec *server_data =
            make_bytes_vec(config.server_bytes[server_index], address_length);
        LangNetAddress server;
        server.family = config.server_family[server_index];
        server.port = kDnsPort;
        server.flags = 0;
        server.scope = 0;
        server.reserved = 0;
        server.data = server_data;

        CandidateList answers;
        answers.count = 0;
        const int64_t query_result = query_name_families(
            search_candidates[candidate_index], family, &server,
            static_cast<int32_t>(config.timeout_seconds) * 1000, &answers);
        lang_vec_release_v1(server_data);
        if (query_result == 0 && answers.count != 0) {
          candidate_merge(&candidates, answers);
          return build_result(candidates, port, out);
        }
        if (query_result == kErrNoEnt) {
          last_error = kErrNoEnt;
          break; // authoritative no-data for this candidate
        }
        last_error = query_result;
      }
    }
  }
  return last_error;
#endif
}

} // extern "C"

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#endif // Linux x86_64 or Darwin arm64
