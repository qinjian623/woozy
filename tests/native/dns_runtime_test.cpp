// Host-side acceptance harness for runtime/native/dns.cpp.
//
// The production resolver is freestanding. This harness uses host libc only
// for local UDP/TCP DNS fixtures, expected wire-byte construction, forks, and
// diagnostics. It never contacts an external resolver.
//
// Build (exact commands are in the task report):
//   <cxx> -std=c++20 -O2 -ffreestanding -fno-exceptions -fno-rtti
//         -fno-sanitize=all -fno-builtin-memcpy -fno-builtin-memset
//         -fno-builtin-memmove (-fno-tree-loop-distribute-patterns for GCC)
//         -c runtime/native/dns.cpp -o dns.o
//   ... compile network.cpp, system.cpp, os.cpp, panic.cpp, alloc.cpp ...
//   <cxx> tests/native/dns_runtime_test.cpp dns.o network.o system.o os.o
//         panic.o alloc.o -Wl,--wrap=lang_alloc_v1 -Wl,--wrap=lang_free_v1
//         -o dns_test
//   ./dns_test

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "../../runtime/native/dns.hpp"
#include "../../runtime/native/network.hpp"
#include "../../runtime/native/vec.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Allocation accounting. Every output Vec and its element data must be freed.

extern "C" void *__real_lang_alloc_v1(uint64_t size, uint64_t align) noexcept;
extern "C" void __real_lang_free_v1(void *pointer) noexcept;

uint64_t allocation_count = 0;
uint64_t free_count = 0;

extern "C" void *__wrap_lang_alloc_v1(uint64_t size, uint64_t align) noexcept {
  void *pointer = __real_lang_alloc_v1(size, align);
  if (pointer != nullptr) ++allocation_count;
  return pointer;
}

extern "C" void __wrap_lang_free_v1(void *pointer) noexcept {
  if (pointer != nullptr) ++free_count;
  __real_lang_free_v1(pointer);
}

namespace {

int failures = 0;

void report_failure(const char *expression, int line, long long actual,
                    long long expected) {
  std::fprintf(stderr, "FAIL line %d: %s: actual %lld expected %lld\n", line,
               expression, actual, expected);
  ++failures;
}

bool check_result(const char *expression, int line, int64_t actual,
                  int64_t expected) {
  if (actual == expected) return true;
  report_failure(expression, line, static_cast<long long>(actual),
                 static_cast<long long>(expected));
  return false;
}

bool check_true(const char *expression, int line, bool value) {
  if (value) return true;
  report_failure(expression, line, value ? 1 : 0, 1);
  return false;
}

#define CHECK_RESULT(expression, expected) \
  check_result(#expression, __LINE__, (expression), static_cast<int64_t>(expected))
#define CHECK_TRUE(expression) check_true(#expression, __LINE__, (expression))

struct ExpectedAddress {
  int32_t family;
  uint8_t bytes[16];
};

LangVec borrowed_vec(uint8_t *bytes, uint64_t length) {
  LangVec vector;
  vector.refs = 1;
  vector.length = length;
  vector.initialized = length;
  vector.stride = 1;
  vector.iterations = 0;
  vector.data = length == 0 ? nullptr : bytes;
  vector.copy = nullptr;
  vector.drop = nullptr;
  return vector;
}

void release_vec(LangVec *vector) {
  if (vector != nullptr) lang_vec_release_v1(vector);
}

void check_address_element(const LangVec *result, uint64_t index,
                           const ExpectedAddress &expected, uint16_t port) {
  auto *address =
      static_cast<LangNetAddress *>(lang_vec_at_v1(
          const_cast<LangVec *>(result), index));
  CHECK_RESULT(address->family, expected.family);
  CHECK_RESULT(address->port, port);
  CHECK_RESULT(address->flags, 0);
  CHECK_RESULT(address->scope, 0);
  CHECK_RESULT(address->reserved, 0);
  CHECK_TRUE(address->data != nullptr);
  if (address->data == nullptr) return;
  const uint64_t length = expected.family == AF_INET ? 4 : 16;
  CHECK_RESULT(address->data->stride, 1);
  CHECK_RESULT(address->data->length, length);
  CHECK_RESULT(address->data->initialized, length);
  CHECK_TRUE(address->data->copy == nullptr);
  CHECK_TRUE(address->data->drop == nullptr);
  CHECK_TRUE(address->data->data != nullptr);
  if (address->data->data != nullptr)
    CHECK_TRUE(std::memcmp(address->data->data, expected.bytes,
                           static_cast<size_t>(length)) == 0);
}

void check_result_vec(LangVec *result, const ExpectedAddress *expected,
                      uint64_t count, uint16_t port) {
  CHECK_TRUE(result != nullptr);
  if (result == nullptr) return;
  CHECK_RESULT(result->stride, sizeof(LangNetAddress));
  CHECK_RESULT(result->length, count);
  CHECK_RESULT(result->initialized, count);
  CHECK_TRUE(result->copy != nullptr);
  CHECK_TRUE(result->drop != nullptr);
  if (result->length >= count && result->initialized >= count) {
    for (uint64_t index = 0; index < count; ++index)
      check_address_element(result, index, expected[index], port);
  }
}

ExpectedAddress expected_ipv4(const uint8_t bytes[4]) {
  ExpectedAddress expected = {};
  expected.family = AF_INET;
  for (int index = 0; index < 4; ++index) expected.bytes[index] = bytes[index];
  return expected;
}

ExpectedAddress expected_ipv6(const uint8_t bytes[16]) {
  ExpectedAddress expected = {};
  expected.family = AF_INET6;
  for (int index = 0; index < 16; ++index)
    expected.bytes[index] = bytes[index];
  return expected;
}

LangText make_text(const char *text) {
  LangText value = {};
  value.data = reinterpret_cast<const uint8_t *>(text);
  value.bytes = std::strlen(text);
  value.length = value.bytes;
  value.owner = nullptr;
  return value;
}

LangText make_text_bytes(const uint8_t *data, uint64_t bytes, uint64_t scalars) {
  LangText value = {};
  value.data = data;
  value.bytes = bytes;
  value.length = scalars;
  value.owner = nullptr;
  return value;
}

uint16_t read_be16(const uint8_t *bytes) {
  return static_cast<uint16_t>(static_cast<uint16_t>(bytes[0]) << 8) |
         static_cast<uint16_t>(bytes[1]);
}

// ---------------------------------------------------------------------------
// Local DNS fixture.

enum class DnsMode {
  A,
  AAAA,
  Cname,
  DuplicateA,
  Nxdomain,
  NoData,
  TcThenA,
  Loop,
  MismatchId,
  Unrelated,
  UnrelatedPlusA,
  WrongQuestion,
  Timeout,
  Dual,
  PartialMalformed,
  CnameThenA,
  CnameThenNxdomain,
  CnameChain,
  CnameLoopCross,
  CnameLoopTerminalA,
  CnameThenTcA,
  CnameCyclePacket,
  CnameReordered,
  DualMalformedA,
};

struct ServerHandle {
  pid_t pid = -1;
  uint16_t port = 0;
};

void append_u16(uint8_t *buffer, size_t *position, uint16_t value) {
  buffer[*position] = static_cast<uint8_t>(value >> 8);
  buffer[*position + 1] = static_cast<uint8_t>(value);
  *position += 2;
}

void append_u32(uint8_t *buffer, size_t *position, uint32_t value) {
  buffer[*position] = static_cast<uint8_t>(value >> 24);
  buffer[*position + 1] = static_cast<uint8_t>(value >> 16);
  buffer[*position + 2] = static_cast<uint8_t>(value >> 8);
  buffer[*position + 3] = static_cast<uint8_t>(value);
  *position += 4;
}

void append_rr_header(uint8_t *buffer, size_t *position, uint16_t owner_pointer,
                      uint16_t type, uint16_t rdata_length) {
  append_u16(buffer, position, owner_pointer);
  append_u16(buffer, position, type);
  append_u16(buffer, position, 1); // IN
  append_u32(buffer, position, 60);
  append_u16(buffer, position, rdata_length);
}

bool encode_test_name(const char *name, uint8_t *out, size_t capacity,
                      size_t *length) {
  size_t position = 0;
  size_t index = 0;
  while (name[index] != '\0') {
    const size_t start = index;
    while (name[index] != '\0' && name[index] != '.') ++index;
    const size_t label = index - start;
    if (label == 0 || label > 63 || position + 1 + label + 1 > capacity)
      return false;
    out[position++] = static_cast<uint8_t>(label);
    for (size_t byte = 0; byte < label; ++byte)
      out[position++] = static_cast<uint8_t>(name[start + byte]);
    if (name[index] == '.') ++index;
  }
  if (position + 1 > capacity) return false;
  out[position++] = 0;
  *length = position;
  return true;
}

bool append_name_rr(uint8_t *buffer, size_t capacity, size_t *position,
                    const char *owner, uint16_t type, const char *target,
                    const uint8_t *address, uint16_t rdata_length) {
  size_t owner_length = 0;
  if (!encode_test_name(owner, buffer + *position, capacity - *position,
                        &owner_length))
    return false;
  *position += owner_length;
  if (*position + 10 > capacity) return false;
  append_u16(buffer, position, type);
  append_u16(buffer, position, 1);
  append_u32(buffer, position, 60);
  const size_t rdata_length_position = *position;
  append_u16(buffer, position, 0);
  const size_t rdata_start = *position;
  if (type == 5) {
    size_t target_length = 0;
    if (!encode_test_name(target, buffer + *position,
                          capacity - *position, &target_length))
      return false;
    *position += target_length;
  } else {
    if (*position + rdata_length > capacity) return false;
    for (uint16_t byte = 0; byte < rdata_length; ++byte)
      buffer[(*position)++] = address[byte];
  }
  const size_t actual_length = *position - rdata_start;
  if (actual_length > 0xffff) return false;
  buffer[rdata_length_position] = static_cast<uint8_t>(actual_length >> 8);
  buffer[rdata_length_position + 1] = static_cast<uint8_t>(actual_length);
  return true;
}

size_t append_cname_only(uint8_t *buffer, size_t capacity,
                         const uint8_t *query, size_t query_length,
                         const char *target) {
  if (query_length < 12 || query_length > capacity) return 0;
  std::memcpy(buffer, query, query_length);
  buffer[2] = 0x81;
  buffer[3] = 0x80;
  buffer[6] = 0;
  buffer[7] = 1;
  buffer[8] = 0;
  buffer[9] = 0;
  buffer[10] = 0;
  buffer[11] = 0;
  size_t position = query_length;
  uint8_t target_wire[256];
  size_t target_length = 0;
  if (!encode_test_name(target, target_wire, sizeof target_wire,
                        &target_length))
    return 0;
  append_rr_header(buffer, &position, 0xc00c, 5,
                   static_cast<uint16_t>(target_length));
  for (size_t byte = 0; byte < target_length; ++byte)
    buffer[position++] = target_wire[byte];
  return position;
}

size_t build_response(uint8_t *buffer, size_t capacity, const uint8_t *query,
                      size_t query_length, DnsMode mode) {
  if (query_length < 12 || query_length > capacity) return 0;
  std::memcpy(buffer, query, query_length);
  buffer[2] = 0x81;
  buffer[3] = 0x80;
  buffer[6] = 0;
  buffer[7] = 0;
  buffer[8] = 0;
  buffer[9] = 0;
  buffer[10] = 0;
  buffer[11] = 0;

  size_t position = query_length;
  if (mode == DnsMode::NoData) return query_length;
  if (mode == DnsMode::Nxdomain) {
    buffer[3] = 0x83;
    return query_length;
  }
  if (mode == DnsMode::TcThenA) {
    buffer[2] = 0x83;
    return query_length;
  }
  if (mode == DnsMode::WrongQuestion) {
    if (query_length < 4) return 0;
    const uint16_t qtype = read_be16(query + query_length - 4);
    const uint16_t replacement = qtype == 1 ? 28 : 1;
    buffer[query_length - 4] = static_cast<uint8_t>(replacement >> 8);
    buffer[query_length - 3] = static_cast<uint8_t>(replacement);
    return query_length;
  }
  if (mode == DnsMode::Loop) {
    buffer[6] = 0;
    buffer[7] = 1;
    append_u16(buffer, &position,
               static_cast<uint16_t>(0xc000 | (query_length & 0x3fff)));
    append_u16(buffer, &position, 1);
    append_u16(buffer, &position, 1);
    append_u32(buffer, &position, 60);
    append_u16(buffer, &position, 4);
    buffer[position++] = 192;
    buffer[position++] = 0;
    buffer[position++] = 2;
    buffer[position++] = 1;
    return position;
  }
  if (mode == DnsMode::Unrelated) {
    buffer[6] = 0;
    buffer[7] = 1;
    append_rr_header(buffer, &position, 0xc010, 1, 4);
    buffer[position++] = 192;
    buffer[position++] = 0;
    buffer[position++] = 2;
    buffer[position++] = 1;
    return position;
  }
  if (mode == DnsMode::UnrelatedPlusA) {
    buffer[6] = 0;
    buffer[7] = 2;
    append_rr_header(buffer, &position, 0xc00c, 1, 4);
    buffer[position++] = 192;
    buffer[position++] = 0;
    buffer[position++] = 2;
    buffer[position++] = 1;
    const uint8_t unrelated[4] = {203, 0, 113, 9};
    if (!append_name_rr(buffer, capacity, &position, "example.test", 1,
                        nullptr, unrelated, 4))
      return 0;
    return position;
  }
  if (mode == DnsMode::PartialMalformed) {
    buffer[6] = 0;
    buffer[7] = 2;
    append_rr_header(buffer, &position, 0xc00c, 1, 4);
    buffer[position++] = 192;
    buffer[position++] = 0;
    buffer[position++] = 2;
    buffer[position++] = 1;
    buffer[position++] = 0xc0; // incomplete trailing RR
    return position;
  }
  if (mode == DnsMode::CnameReordered) {
    buffer[6] = 0;
    buffer[7] = 2;
    const uint8_t address[4] = {192, 0, 2, 1};
    if (!append_name_rr(buffer, capacity, &position, "b.test", 1, nullptr,
                        address, 4))
      return 0;
    if (!append_name_rr(buffer, capacity, &position, "a.test", 5, "b.test",
                        nullptr, 0))
      return 0;
    return position;
  }
  if (mode == DnsMode::CnameCyclePacket) {
    buffer[6] = 0;
    buffer[7] = 3;
    const uint8_t address[4] = {192, 0, 2, 1};
    if (!append_name_rr(buffer, capacity, &position, "a.test", 5, "b.test",
                        nullptr, 0))
      return 0;
    if (!append_name_rr(buffer, capacity, &position, "b.test", 5, "a.test",
                        nullptr, 0))
      return 0;
    if (!append_name_rr(buffer, capacity, &position, "a.test", 1, nullptr,
                        address, 4))
      return 0;
    return position;
  }
  if (mode == DnsMode::Cname) {
    if (query_length != 34) return 0;
    buffer[6] = 0;
    buffer[7] = 2;
    append_rr_header(buffer, &position, 0xc00c, 5, 8);
    buffer[position++] = 5;
    const char alias[5] = {'a', 'l', 'i', 'a', 's'};
    for (char byte : alias) buffer[position++] = static_cast<uint8_t>(byte);
    append_u16(buffer, &position, 0xc010);
    append_rr_header(buffer, &position, 0xc02e, 1, 4);
    buffer[position++] = 192;
    buffer[position++] = 0;
    buffer[position++] = 2;
    buffer[position++] = 1;
    return position;
  }

  // A/AAAA/DuplicateA and the TCP half of TcThenA.
  const bool aaaa = mode == DnsMode::AAAA;
  const uint16_t type = aaaa ? 28 : 1;
  const uint16_t rdata_length = aaaa ? 16 : 4;
  const uint16_t count = mode == DnsMode::DuplicateA ? 2 : 1;
  buffer[6] = static_cast<uint8_t>(count >> 8);
  buffer[7] = static_cast<uint8_t>(count);
  for (uint16_t record = 0; record < count; ++record) {
    append_rr_header(buffer, &position, 0xc00c, type, rdata_length);
    if (aaaa) {
      const uint8_t address[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0,
                                   0, 0, 0, 0, 0, 0, 0, 1};
      for (uint8_t byte : address) buffer[position++] = byte;
    } else {
      buffer[position++] = 192;
      buffer[position++] = 0;
      buffer[position++] = 2;
      buffer[position++] = 1;
    }
  }
  if (mode == DnsMode::MismatchId) {
    buffer[0] = static_cast<uint8_t>(buffer[0] ^ 0xff);
    buffer[1] = static_cast<uint8_t>(buffer[1] ^ 0xff);
  }
  return position;
}

void write_all_host(int fd, const void *data, size_t length) {
  const auto *bytes = static_cast<const uint8_t *>(data);
  size_t offset = 0;
  while (offset < length) {
    const ssize_t written = ::write(fd, bytes + offset, length - offset);
    if (written < 0) {
      if (errno == EINTR) continue;
      _exit(30);
    }
    if (written == 0) _exit(31);
    offset += static_cast<size_t>(written);
  }
}

void read_exact_host(int fd, void *data, size_t length) {
  auto *bytes = static_cast<uint8_t *>(data);
  size_t offset = 0;
  while (offset < length) {
    const ssize_t received = ::read(fd, bytes + offset, length - offset);
    if (received < 0) {
      if (errno == EINTR) continue;
      _exit(32);
    }
    if (received == 0) _exit(33);
    offset += static_cast<size_t>(received);
  }
}

void send_tcp_response(int fd, const uint8_t *response, size_t length) {
  const uint8_t prefix[2] = {static_cast<uint8_t>(length >> 8),
                             static_cast<uint8_t>(length)};
  write_all_host(fd, prefix, sizeof prefix);
  const size_t first = length / 2;
  write_all_host(fd, response, first);
  const struct timespec delay = {0, 5000000};
  nanosleep(&delay, nullptr);
  write_all_host(fd, response + first, length - first);
}

ssize_t receive_udp(int udp_fd, uint8_t *query, size_t capacity,
                    struct sockaddr_storage *client, socklen_t *client_length) {
  struct pollfd row = {udp_fd, POLLIN, 0};
  if (poll(&row, 1, 3000) <= 0) _exit(40);
  *client_length = sizeof *client;
  return recvfrom(udp_fd, query, capacity, 0,
                  reinterpret_cast<struct sockaddr *>(client),
                  client_length);
}

void send_udp(int udp_fd, const uint8_t *response, size_t length,
              const struct sockaddr_storage *client,
              socklen_t client_length) {
  if (sendto(udp_fd, response, length, 0,
             reinterpret_cast<const struct sockaddr *>(client),
             client_length) != static_cast<ssize_t>(length))
    _exit(41);
}

void serve_tcp_a(int tcp_fd, uint8_t *query, size_t query_capacity,
                 uint8_t *response, size_t response_capacity) {
  struct pollfd listen_row = {tcp_fd, POLLIN, 0};
  if (poll(&listen_row, 1, 3000) <= 0) _exit(42);
  const int connection = accept(tcp_fd, nullptr, nullptr);
  if (connection < 0) _exit(43);
  uint8_t prefix[2] = {};
  read_exact_host(connection, prefix, sizeof prefix);
  const uint16_t tcp_length = read_be16(prefix);
  if (tcp_length < 12 || tcp_length > query_capacity) _exit(44);
  read_exact_host(connection, query, tcp_length);
  const size_t response_length =
      build_response(response, response_capacity, query, tcp_length,
                     DnsMode::A);
  if (response_length == 0) _exit(45);
  send_tcp_response(connection, response, response_length);
  close(connection);
}

[[noreturn]] void serve_child(int udp_fd, int tcp_fd, DnsMode mode) {
  uint8_t query[512];
  uint8_t response[1024];

  if (mode == DnsMode::Timeout) {
    struct pollfd row = {udp_fd, POLLIN, 0};
    poll(&row, 1, 2000);
    _exit(0);
  }

  if (mode == DnsMode::DualMalformedA) {
    for (int query_index = 0; query_index < 2; ++query_index) {
      struct sockaddr_storage client = {};
      socklen_t client_length = 0;
      const ssize_t query_length =
          receive_udp(udp_fd, query, sizeof query, &client, &client_length);
      if (query_length < 12) _exit(46);
      const size_t response_length =
          build_response(response, sizeof response, query,
                         static_cast<size_t>(query_length),
                         query_index == 0 ? DnsMode::PartialMalformed
                                          : DnsMode::NoData);
      if (response_length == 0) _exit(47);
      send_udp(udp_fd, response, response_length, &client, client_length);
    }
    _exit(0);
  }

  if (mode == DnsMode::CnameThenA || mode == DnsMode::CnameChain ||
      mode == DnsMode::CnameLoopCross || mode == DnsMode::CnameLoopTerminalA ||
      mode == DnsMode::CnameThenNxdomain) {
    const int exchanges = mode == DnsMode::CnameChain ? 3 : 2;
    const char *targets[3] = {"b.test", "c.test", "a.test"};
    for (int query_index = 0; query_index < exchanges; ++query_index) {
      struct sockaddr_storage client = {};
      socklen_t client_length = 0;
      const ssize_t query_length =
          receive_udp(udp_fd, query, sizeof query, &client, &client_length);
      if (query_length < 12) _exit(48);
      size_t response_length = 0;
      const bool send_nxdomain =
          mode == DnsMode::CnameThenNxdomain && query_index == 1;
      const bool send_a =
          mode != DnsMode::CnameLoopCross && mode != DnsMode::CnameLoopTerminalA &&
          mode != DnsMode::CnameThenNxdomain &&
          query_index + 1 == exchanges;
      if (send_nxdomain) {
        response_length =
            build_response(response, sizeof response, query,
                           static_cast<size_t>(query_length),
                           DnsMode::Nxdomain);
      } else if (send_a) {
        response_length =
            build_response(response, sizeof response, query,
                           static_cast<size_t>(query_length), DnsMode::A);
      } else {
        const char *target =
            (mode == DnsMode::CnameLoopCross || mode == DnsMode::CnameLoopTerminalA)
                ? (query_index == 0 ? "b.test" : "a.test")
                : targets[query_index];
        response_length = append_cname_only(
            response, sizeof response, query,
            static_cast<size_t>(query_length), target);
        if (response_length == 0) _exit(49);
        if (mode == DnsMode::CnameLoopTerminalA && query_index == 1) {
          response[6] = 0; response[7] = 2;
          size_t position = response_length;
          const uint8_t address[4] = {192,0,2,1};
          if (!append_name_rr(response,sizeof response,&position,"a.test",1,nullptr,address,4)) _exit(52);
          response_length=position;
        }
      }
      if (response_length == 0) _exit(51);
      send_udp(udp_fd, response, response_length, &client, client_length);
    }
    _exit(0);
  }

  if (mode == DnsMode::CnameThenTcA) {
    struct sockaddr_storage client = {};
    socklen_t client_length = 0;
    ssize_t query_length =
        receive_udp(udp_fd, query, sizeof query, &client, &client_length);
    if (query_length < 12) _exit(52);
    size_t response_length =
        append_cname_only(response, sizeof response, query,
                          static_cast<size_t>(query_length), "b.test");
    if (response_length == 0) _exit(53);
    send_udp(udp_fd, response, response_length, &client, client_length);

    query_length =
        receive_udp(udp_fd, query, sizeof query, &client, &client_length);
    if (query_length < 12) _exit(55);
    response_length = build_response(response, sizeof response, query,
                                     static_cast<size_t>(query_length),
                                     DnsMode::TcThenA);
    if (response_length == 0) _exit(56);
    send_udp(udp_fd, response, response_length, &client, client_length);
    serve_tcp_a(tcp_fd, query, sizeof query, response, sizeof response);
    _exit(0);
  }

  if (mode == DnsMode::Dual) {
    for (int query_index = 0; query_index < 2; ++query_index) {
      struct pollfd row = {udp_fd, POLLIN, 0};
      if (poll(&row, 1, 3000) <= 0) _exit(10);
      struct sockaddr_storage client = {};
      socklen_t client_length = sizeof client;
      const ssize_t query_length =
          recvfrom(udp_fd, query, sizeof query, 0,
                   reinterpret_cast<struct sockaddr *>(&client),
                   &client_length);
      if (query_length < 16) _exit(11);
      const uint16_t qtype =
          read_be16(query + static_cast<size_t>(query_length) - 4);
      const DnsMode sub_mode = qtype == 1 ? DnsMode::A : DnsMode::AAAA;
      const size_t response_length =
          build_response(response, sizeof response, query,
                         static_cast<size_t>(query_length), sub_mode);
      if (response_length == 0) _exit(12);
      sendto(udp_fd, response, response_length, 0,
             reinterpret_cast<struct sockaddr *>(&client), client_length);
    }
    _exit(0);
  }

  struct pollfd row = {udp_fd, POLLIN, 0};
  if (poll(&row, 1, 3000) <= 0) _exit(13);
  struct sockaddr_storage client = {};
  socklen_t client_length = sizeof client;
  const ssize_t query_length =
      recvfrom(udp_fd, query, sizeof query, 0,
               reinterpret_cast<struct sockaddr *>(&client), &client_length);
  if (query_length < 12) _exit(14);

  size_t response_length =
      build_response(response, sizeof response, query,
                     static_cast<size_t>(query_length), mode);
  if (response_length == 0) _exit(15);
  sendto(udp_fd, response, response_length, 0,
         reinterpret_cast<struct sockaddr *>(&client), client_length);

  if (mode == DnsMode::TcThenA) {
    serve_tcp_a(tcp_fd, query, sizeof query, response, sizeof response);
  }

  _exit(0);
}

bool start_server(DnsMode mode, int family, ServerHandle *handle) {
  const int udp_fd = ::socket(family, SOCK_DGRAM, 0);
  if (udp_fd < 0) return false;
  if (family == AF_INET6) {
    const int one = 1;
    setsockopt(udp_fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof one);
  }

  struct sockaddr_storage address = {};
  socklen_t address_length = 0;
  if (family == AF_INET) {
    auto *ipv4 = reinterpret_cast<struct sockaddr_in *>(&address);
    ipv4->sin_family = AF_INET;
    ipv4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ipv4->sin_port = 0;
    address_length = sizeof *ipv4;
  } else {
    auto *ipv6 = reinterpret_cast<struct sockaddr_in6 *>(&address);
    ipv6->sin6_family = AF_INET6;
    ipv6->sin6_addr = in6addr_loopback;
    ipv6->sin6_port = 0;
    address_length = sizeof *ipv6;
  }

  if (::bind(udp_fd, reinterpret_cast<struct sockaddr *>(&address),
             address_length) != 0) {
    close(udp_fd);
    return false;
  }
  if (::getsockname(udp_fd, reinterpret_cast<struct sockaddr *>(&address),
                    &address_length) != 0) {
    close(udp_fd);
    return false;
  }
  uint16_t port = 0;
  if (family == AF_INET)
    port = ntohs(reinterpret_cast<struct sockaddr_in *>(&address)->sin_port);
  else
    port = ntohs(reinterpret_cast<struct sockaddr_in6 *>(&address)->sin6_port);

  const int tcp_fd = ::socket(family, SOCK_STREAM, 0);
  if (tcp_fd < 0) {
    close(udp_fd);
    return false;
  }
  const int one = 1;
  setsockopt(tcp_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  if (family == AF_INET6)
    setsockopt(tcp_fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof one);
  if (::bind(tcp_fd, reinterpret_cast<struct sockaddr *>(&address),
             address_length) != 0 ||
      ::listen(tcp_fd, 1) != 0) {
    close(udp_fd);
    close(tcp_fd);
    return false;
  }

  const pid_t child = fork();
  if (child < 0) {
    close(udp_fd);
    close(tcp_fd);
    return false;
  }
  if (child == 0) {
    serve_child(udp_fd, tcp_fd, mode);
    _exit(0);
  }

  close(udp_fd);
  close(tcp_fd);
  handle->pid = child;
  handle->port = port;
  return true;
}

bool start_server_retry(DnsMode mode, int family, ServerHandle *handle) {
  for (int attempt = 0; attempt < 5; ++attempt) {
    if (start_server(mode, family, handle)) return true;
  }
  return false;
}

void stop_server(ServerHandle *handle) {
  if (handle->pid > 0) {
    kill(handle->pid, SIGTERM);
    int status = 0;
    waitpid(handle->pid, &status, 0);
    handle->pid = -1;
  }
}

bool probe_ipv6() {
  const int fd = ::socket(AF_INET6, SOCK_DGRAM, 0);
  if (fd < 0) return false;
  struct sockaddr_in6 address = {};
  address.sin6_family = AF_INET6;
  address.sin6_addr = in6addr_loopback;
  address.sin6_port = 0;
  const bool usable =
      ::bind(fd, reinterpret_cast<struct sockaddr *>(&address),
             sizeof address) == 0;
  close(fd);
  return usable;
}

LangNetAddress make_server_address(int family, uint16_t port,
                                   const uint8_t *bytes, LangVec *storage) {
  *storage = borrowed_vec(const_cast<uint8_t *>(bytes),
                          family == AF_INET ? 4 : 16);
  LangNetAddress address = {};
  address.family = family;
  address.port = port;
  address.data = storage;
  return address;
}

void run_query_success(DnsMode mode, int server_family, const char *host,
                       int32_t family, const ExpectedAddress *expected,
                       uint64_t expected_count, uint16_t expected_port,
                       int32_t timeout_ms) {
  ServerHandle server;
  CHECK_TRUE(start_server_retry(mode, server_family, &server));
  if (server.pid <= 0) return;

  uint8_t loopback[16] = {};
  if (server_family == AF_INET) {
    loopback[0] = 127;
    loopback[3] = 1;
  } else {
    loopback[15] = 1;
  }
  LangVec server_vector = {};
  LangNetAddress server_address = make_server_address(
      server_family, server.port, loopback, &server_vector);
  LangText hostname = make_text(host);
  LangVec *result = nullptr;
  const int64_t query_result = lang_net_dns_query_v1(
      &hostname, family, &server_address, timeout_ms, &result);
  stop_server(&server);

  CHECK_RESULT(query_result, 0);
  if (query_result == 0) {
    check_result_vec(result, expected, expected_count, expected_port);
    release_vec(result);
  }
}

void run_query_error(DnsMode mode, int server_family, const char *host,
                     int32_t family, int32_t timeout_ms,
                     int64_t expected_error) {
  ServerHandle server;
  CHECK_TRUE(start_server_retry(mode, server_family, &server));
  if (server.pid <= 0) return;

  uint8_t loopback[16] = {};
  if (server_family == AF_INET) {
    loopback[0] = 127;
    loopback[3] = 1;
  } else {
    loopback[15] = 1;
  }
  LangVec server_vector = {};
  LangNetAddress server_address = make_server_address(
      server_family, server.port, loopback, &server_vector);
  LangText hostname = make_text(host);
  LangVec *result = reinterpret_cast<LangVec *>(static_cast<uintptr_t>(0x1234));
  const int64_t query_result = lang_net_dns_query_v1(
      &hostname, family, &server_address, timeout_ms, &result);
  stop_server(&server);
  CHECK_RESULT(query_result, expected_error);
  CHECK_TRUE(result == reinterpret_cast<LangVec *>(
                           static_cast<uintptr_t>(0x1234)));
}

void test_abi() {
  CHECK_RESULT(sizeof(LangText), 32);
  CHECK_RESULT(__builtin_offsetof(LangText, data), 0);
  CHECK_RESULT(__builtin_offsetof(LangText, bytes), 8);
  CHECK_RESULT(__builtin_offsetof(LangText, length), 16);
  CHECK_RESULT(__builtin_offsetof(LangText, owner), 24);
}

void test_lookup_fastpaths() {
  LangText localhost = make_text("localhost");
  LangVec *result = nullptr;
  CHECK_RESULT(lang_net_lookup_v1(&localhost, 8080, 0, &result), 0);
  const uint8_t ipv4_loopback[4] = {127, 0, 0, 1};
  const uint8_t ipv6_loopback[16] = {0, 0, 0, 0, 0, 0, 0, 0,
                                     0, 0, 0, 0, 0, 0, 0, 1};
  const ExpectedAddress both[2] = {expected_ipv4(ipv4_loopback),
                                   expected_ipv6(ipv6_loopback)};
  check_result_vec(result, both, 2, 8080);
  release_vec(result);

  result = nullptr;
  CHECK_RESULT(lang_net_lookup_v1(&localhost, 8081, AF_INET, &result), 0);
  check_result_vec(result, &both[0], 1, 8081);
  release_vec(result);

  result = nullptr;
  CHECK_RESULT(lang_net_lookup_v1(&localhost, 8082, AF_INET6, &result), 0);
  check_result_vec(result, &both[1], 1, 8082);
  release_vec(result);

  LangText numeric_v4 = make_text("127.0.0.1");
  result = nullptr;
  CHECK_RESULT(lang_net_lookup_v1(&numeric_v4, 1234, 0, &result), 0);
  check_result_vec(result, &both[0], 1, 1234);
  release_vec(result);

  LangText numeric_v6 = make_text("::1");
  result = nullptr;
  CHECK_RESULT(lang_net_lookup_v1(&numeric_v6, 1235, AF_INET6, &result), 0);
  check_result_vec(result, &both[1], 1, 1235);
  release_vec(result);

  result = nullptr;
  CHECK_RESULT(lang_net_lookup_v1(&numeric_v4, 1236, AF_INET6, &result),
               -ENOENT);
  CHECK_TRUE(result == nullptr);

  char long_label[65];
  std::memset(long_label, 'a', sizeof long_label);
  long_label[64] = '\0';
  char long_name[255];
  std::memset(long_name, 'a', sizeof long_name);
  long_name[254] = '\0';
  const char embedded[] = {'a', 'b', '\0', 'c'};
  const char non_ascii[] = {static_cast<char>(0xc3), static_cast<char>(0xa9)};

  const LangText empty_text = {nullptr, 0, 0, nullptr};
  const LangText embedded_text = make_text_bytes(
      reinterpret_cast<const uint8_t *>(embedded), sizeof embedded,
      sizeof embedded);
  const LangText long_label_text = make_text(long_label);
  const LangText long_name_text = make_text(long_name);
  const LangText non_ascii_text = make_text_bytes(
      reinterpret_cast<const uint8_t *>(non_ascii), sizeof non_ascii, 1);
  const LangText malformed[] = {empty_text, embedded_text, long_label_text,
                                long_name_text, non_ascii_text};
  for (const LangText &text : malformed) {
    LangVec *sentinel =
        reinterpret_cast<LangVec *>(static_cast<uintptr_t>(0x1234));
    CHECK_TRUE(lang_net_lookup_v1(&text, 1, 0, &sentinel) < 0);
    CHECK_TRUE(sentinel == reinterpret_cast<LangVec *>(
                               static_cast<uintptr_t>(0x1234)));
  }

  CHECK_RESULT(lang_net_lookup_v1(&localhost, 1, 3, &result),
               -EAFNOSUPPORT);
  CHECK_RESULT(lang_net_lookup_v1(nullptr, 1, 0, &result), -EINVAL);
  CHECK_RESULT(lang_net_lookup_v1(&localhost, 1, 0, nullptr), -EFAULT);
  CHECK_RESULT(lang_net_lookup_v1(&localhost, 1, AF_INET, nullptr), -EFAULT);
}

void test_dns_udp() {
  const char *host = "www.example.test";
  const uint8_t a_bytes[4] = {192, 0, 2, 1};
  const uint8_t aaaa_bytes[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0,
                                  0, 0, 0, 0, 0, 0, 0, 1};
  const ExpectedAddress a = expected_ipv4(a_bytes);
  const ExpectedAddress aaaa = expected_ipv6(aaaa_bytes);

  run_query_success(DnsMode::A, AF_INET, host, AF_INET, &a, 1, 0, 1000);
  run_query_success(DnsMode::AAAA, AF_INET, host, AF_INET6, &aaaa, 1, 0,
                    1000);
  run_query_success(DnsMode::DuplicateA, AF_INET, host, AF_INET, &a, 1, 0,
                    1000);
  run_query_success(DnsMode::Cname, AF_INET, host, AF_INET, &a, 1, 0, 1000);
  run_query_success(DnsMode::TcThenA, AF_INET, host, AF_INET, &a, 1, 0, 1000);

  const ExpectedAddress dual[2] = {a, aaaa};
  run_query_success(DnsMode::Dual, AF_INET, "dual.example.test", 0, dual, 2, 0,
                    1000);

  run_query_error(DnsMode::Nxdomain, AF_INET, host, AF_INET, 1000, -ENOENT);
  run_query_error(DnsMode::NoData, AF_INET, host, AF_INET, 1000, -ENOENT);
  run_query_error(DnsMode::MismatchId, AF_INET, host, AF_INET, 1000, -EINVAL);
  run_query_error(DnsMode::Loop, AF_INET, host, AF_INET, 1000, -EINVAL);
  run_query_error(DnsMode::Unrelated, AF_INET, host, AF_INET, 1000, -ENOENT);
  run_query_error(DnsMode::WrongQuestion, AF_INET, host, AF_INET, 1000,
                  -EINVAL);
  run_query_error(DnsMode::Timeout, AF_INET, host, AF_INET, 200, -ETIMEDOUT);
}

void test_dns_corrections() {
  const uint8_t a_bytes[4] = {192, 0, 2, 1};
  const ExpectedAddress a = expected_ipv4(a_bytes);

  // Confirmed case 1: a valid A followed by an incomplete RR must fail with
  // no address accepted from the malformed packet.
  run_query_error(DnsMode::PartialMalformed, AF_INET, "a.test", AF_INET, 1000,
                  -EINVAL);
  run_query_error(DnsMode::DualMalformedA, AF_INET, "a.test", 0, 1000,
                  -EINVAL);

  // Confirmed case 2: CNAME-only packets require a follow-up query.
  run_query_success(DnsMode::CnameThenA, AF_INET, "a.test", AF_INET, &a, 1, 0,
                    1000);
  run_query_error(DnsMode::CnameThenNxdomain, AF_INET, "a.test", AF_INET,
                  1000, -ENOENT);
  run_query_success(DnsMode::CnameChain, AF_INET, "a.test", AF_INET, &a, 1, 0,
                    1000);
  run_query_success(DnsMode::CnameThenTcA, AF_INET, "a.test", AF_INET, &a, 1,
                    0, 1000);

  // Confirmed case 3: a->b->a with A(a) must reject before accepting A(a).
  run_query_error(DnsMode::CnameCyclePacket, AF_INET, "a.test", AF_INET, 1000,
                  -EINVAL);
  run_query_error(DnsMode::CnameLoopCross, AF_INET, "a.test", AF_INET, 1000,
                  -EINVAL);
  run_query_error(DnsMode::CnameLoopTerminalA, AF_INET, "a.test", AF_INET, 1000,
                  -EINVAL);

  // RR permutation: A(target) before CNAME(query -> target) is valid.
  run_query_success(DnsMode::CnameReordered, AF_INET, "a.test", AF_INET, &a, 1,
                    0, 1000);

  // Unrelated addresses are ignored and cannot satisfy the query.
  run_query_success(DnsMode::UnrelatedPlusA, AF_INET, "a.test", AF_INET, &a, 1,
                    0, 1000);
}

void test_dns_ipv6_server(bool ipv6_available) {
  if (!ipv6_available) {
    std::fprintf(stderr, "note: IPv6 unavailable; IPv6 DNS server skipped\n");
    return;
  }
  const uint8_t aaaa_bytes[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0,
                                  0, 0, 0, 0, 0, 0, 0, 1};
  const ExpectedAddress aaaa = expected_ipv6(aaaa_bytes);
  run_query_success(DnsMode::AAAA, AF_INET6, "ipv6.example.test", AF_INET6,
                    &aaaa, 1, 0, 1000);
}

void test_bad_server() {
  uint8_t mutable_bytes[4] = {127, 0, 0, 1};
  LangVec vector = borrowed_vec(mutable_bytes, 4);
  LangNetAddress server = {};
  server.family = AF_INET;
  server.data = &vector;
  LangText host = make_text("www.example.test");
  LangVec *result = nullptr;

  CHECK_RESULT(lang_net_dns_query_v1(&host, AF_INET, nullptr, 100, &result),
               -EINVAL);
  server.family = 3;
  CHECK_RESULT(lang_net_dns_query_v1(&host, AF_INET, &server, 100, &result),
               -EINVAL);
  server.family = AF_INET;
  vector.length = 3;
  vector.initialized = 3;
  CHECK_RESULT(lang_net_dns_query_v1(&host, AF_INET, &server, 100, &result),
               -EINVAL);
  vector.length = 4;
  vector.initialized = 4;
  server.flags = 1;
  CHECK_RESULT(lang_net_dns_query_v1(&host, AF_INET, &server, 100, &result),
               -EINVAL);
  server.flags = 0;
  CHECK_RESULT(lang_net_dns_query_v1(&host, 3, &server, 100, &result),
               -EAFNOSUPPORT);
  CHECK_RESULT(lang_net_dns_query_v1(&host, AF_INET, &server, 0, &result),
               -EINVAL);
  CHECK_RESULT(lang_net_dns_query_v1(&host, AF_INET, &server, 100, nullptr),
               -EFAULT);
  CHECK_RESULT(lang_net_dns_query_v1(nullptr, AF_INET, &server, 100, &result),
               -EINVAL);
  LangText null_text = {nullptr, 1, 1, nullptr};
  CHECK_RESULT(lang_net_dns_query_v1(&null_text, AF_INET, &server, 100,
                                     &result),
               -EINVAL);
}

void test_result_callbacks() {
  const char *host = "www.example.test";
  ServerHandle server;
  CHECK_TRUE(start_server_retry(DnsMode::A, AF_INET, &server));
  if (server.pid <= 0) return;

  const uint8_t loopback[4] = {127, 0, 0, 1};
  LangVec server_vector = {};
  LangNetAddress server_address =
      make_server_address(AF_INET, server.port, loopback, &server_vector);
  LangText hostname = make_text(host);
  LangVec *result = nullptr;
  CHECK_RESULT(lang_net_dns_query_v1(&hostname, AF_INET, &server_address, 1000,
                                     &result),
               0);
  stop_server(&server);
  CHECK_TRUE(result != nullptr);
  if (result != nullptr) {
    CHECK_RESULT(result->length, 1);
    CHECK_TRUE(result->copy != nullptr);
    CHECK_TRUE(result->drop != nullptr);
    LangVec *slice = lang_vec_slice_v1(result, 0, result->length);
    CHECK_RESULT(slice->stride, sizeof(LangNetAddress));
    CHECK_RESULT(slice->length, 1);
    release_vec(slice);
    release_vec(result);
  }
}

} // namespace

int main() {
  const bool ipv6_available = probe_ipv6();

  test_abi();
  test_lookup_fastpaths();
  test_dns_udp();
  test_dns_corrections();
  test_dns_ipv6_server(ipv6_available);
  test_bad_server();
  test_result_callbacks();

  CHECK_RESULT(allocation_count, free_count);

  if (failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("dns runtime checks passed\n");
  return 0;
}
