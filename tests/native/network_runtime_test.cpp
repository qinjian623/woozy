// Host-side acceptance harness for runtime/native/network.cpp.
//
// The runtime object under test is freestanding. This harness uses host libc
// only for temporary files, sockets used as oracles, signals, and diagnostics.
// It provides no fallback implementation of any lang_net_* entry point.
//
// Build (exact commands are in the task report):
//   <cxx> -std=c++20 -O2 -ffreestanding -fno-exceptions -fno-rtti
//         -fno-sanitize=all -fno-builtin-memcpy -fno-builtin-memset
//         -fno-builtin-memmove (-fno-tree-loop-distribute-patterns for GCC)
//         -c runtime/native/network.cpp -o network.o
//   <cxx> ... -c runtime/native/panic.cpp runtime/native/alloc.cpp
//   <cxx> tests/native/network_runtime_test.cpp network.o panic.o alloc.o
//         -Wl,--wrap=lang_alloc_v1 -Wl,--wrap=lang_free_v1 -o network_test
//   ./network_test

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

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
#include <ctime>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Allocation accounting. lang_vec_release_v1 must free every output address.

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

void dummy_value_op(void *) {}

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

bool make_path(char *buffer, size_t capacity, const char *directory,
               const char *name) {
  const int length = std::snprintf(buffer, capacity, "%s/%s", directory, name);
  return length > 0 && static_cast<size_t>(length) < capacity;
}

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

void set_address(LangNetAddress *address, int32_t family, uint16_t port,
                 uint16_t flags, uint32_t scope, LangVec *data) {
  address->family = family;
  address->port = port;
  address->flags = flags;
  address->scope = scope;
  address->reserved = 0;
  address->data = data;
}

void release_address(LangNetAddress *address) {
  if (address->data != nullptr) {
    lang_vec_release_v1(address->data);
    address->data = nullptr;
  }
}

void check_vec(const LangVec *vector, const uint8_t *expected, uint64_t length) {
  CHECK_TRUE(vector != nullptr);
  if (vector == nullptr) return;
  CHECK_RESULT(vector->stride, 1);
  CHECK_RESULT(vector->length, length);
  CHECK_RESULT(vector->initialized, length);
  CHECK_TRUE(vector->copy == nullptr);
  CHECK_TRUE(vector->drop == nullptr);
  if (length != 0) {
    CHECK_TRUE(vector->data != nullptr);
    if (vector->data != nullptr)
      CHECK_TRUE(std::memcmp(vector->data, expected,
                             static_cast<size_t>(length)) == 0);
  }
}

void check_address(const LangNetAddress &address, int32_t family, uint16_t port,
                   uint16_t flags, uint32_t scope, const uint8_t *expected,
                   uint64_t length) {
  CHECK_RESULT(address.family, family);
  CHECK_RESULT(address.port, port);
  CHECK_RESULT(address.flags, flags);
  CHECK_RESULT(address.scope, scope);
  CHECK_RESULT(address.reserved, 0);
  check_vec(address.data, expected, length);
}

void close_fd(int64_t fd) {
  if (fd >= 0) ::close(static_cast<int>(fd));
}

struct TcpFixture {
  int64_t listener = -1;
  int64_t accepted = -1;
  int64_t client = -1;
  LangNetAddress bound = {};
  LangNetAddress peer = {};
};

void close_tcp_fixture(TcpFixture *fixture) {
  close_fd(fixture->listener);
  close_fd(fixture->accepted);
  close_fd(fixture->client);
  release_address(&fixture->bound);
  release_address(&fixture->peer);
  fixture->listener = -1;
  fixture->accepted = -1;
  fixture->client = -1;
}

int64_t make_bound_socket(int32_t family, int32_t type, const uint8_t *bytes,
                          uint64_t length, uint16_t flags,
                          LangNetAddress *bound) {
  const int64_t socket_fd = lang_net_socket_v1(family, type, 0);
  if (socket_fd < 0) return socket_fd;

  uint8_t mutable_bytes[128];
  if (length > sizeof mutable_bytes) {
    close_fd(socket_fd);
    return -EINVAL;
  }
  for (uint64_t index = 0; index < length; ++index)
    mutable_bytes[index] = bytes[index];

  LangVec vector = borrowed_vec(mutable_bytes, length);
  LangNetAddress address;
  set_address(&address, family, 0, flags, 0, &vector);
  int64_t result = lang_net_bind_v1(static_cast<int32_t>(socket_fd), &address);
  if (result < 0) {
    close_fd(socket_fd);
    return result;
  }
  result = lang_net_getsockname_v1(static_cast<int32_t>(socket_fd), bound);
  if (result < 0) {
    close_fd(socket_fd);
    return result;
  }
  return socket_fd;
}

bool make_tcp_fixture(int32_t family, const uint8_t *bytes, uint64_t length,
                      uint16_t flags, TcpFixture *fixture) {
  const int64_t listener = make_bound_socket(
      family, SOCK_STREAM, bytes, length, flags, &fixture->bound);
  if (listener < 0) return false;
  fixture->listener = listener;

  if (lang_net_listen_v1(static_cast<int32_t>(listener), 8) != 0) {
    close_tcp_fixture(fixture);
    return false;
  }

  const int64_t client = lang_net_socket_v1(family, SOCK_STREAM, 0);
  if (client < 0) {
    close_tcp_fixture(fixture);
    return false;
  }
  fixture->client = client;

  if (lang_net_connect_v1(static_cast<int32_t>(client), &fixture->bound) != 0) {
    close_tcp_fixture(fixture);
    return false;
  }

  const int64_t accepted =
      lang_net_accept_v1(static_cast<int32_t>(listener), &fixture->peer,
                         SOCK_CLOEXEC);
  if (accepted < 0) {
    close_tcp_fixture(fixture);
    return false;
  }
  fixture->accepted = accepted;
  return true;
}

void test_abi_layout() {
  CHECK_RESULT(sizeof(LangNetAddress), 24);
  CHECK_RESULT(__builtin_offsetof(LangNetAddress, family), 0);
  CHECK_RESULT(__builtin_offsetof(LangNetAddress, port), 4);
  CHECK_RESULT(__builtin_offsetof(LangNetAddress, flags), 6);
  CHECK_RESULT(__builtin_offsetof(LangNetAddress, scope), 8);
  CHECK_RESULT(__builtin_offsetof(LangNetAddress, reserved), 12);
  CHECK_RESULT(__builtin_offsetof(LangNetAddress, data), 16);
  CHECK_RESULT(sizeof(LangPollFd), 8);
  CHECK_RESULT(__builtin_offsetof(LangPollFd, fd), 0);
  CHECK_RESULT(__builtin_offsetof(LangPollFd, events), 4);
  CHECK_RESULT(__builtin_offsetof(LangPollFd, revents), 6);
}

void test_address_validation() {
  const int64_t fd = lang_net_socket_v1(AF_INET, SOCK_DGRAM, 0);
  CHECK_TRUE(fd >= 0);
  if (fd < 0) return;

  uint8_t bytes4[4] = {127, 0, 0, 1};
  LangVec vector = borrowed_vec(bytes4, 4);
  LangNetAddress address;
  set_address(&address, AF_INET, 0, 0, 0, &vector);

  CHECK_RESULT(lang_net_bind_v1(static_cast<int32_t>(fd), nullptr), -EFAULT);
  CHECK_RESULT(lang_net_connect_v1(static_cast<int32_t>(fd), nullptr),
               -EFAULT);
  CHECK_RESULT(lang_net_bind_v1(-1, &address), -EBADF);
  CHECK_RESULT(lang_net_connect_v1(-1, &address), -EBADF);
  CHECK_RESULT(lang_net_getsockname_v1(static_cast<int32_t>(fd), nullptr),
               -EFAULT);
  CHECK_RESULT(lang_net_getpeername_v1(static_cast<int32_t>(fd), nullptr),
               -EFAULT);

  address.reserved = 1;
  CHECK_RESULT(lang_net_bind_v1(static_cast<int32_t>(fd), &address), -EINVAL);
  address.reserved = 0;

  address.flags = 2;
  CHECK_RESULT(lang_net_bind_v1(static_cast<int32_t>(fd), &address), -EINVAL);
  address.flags = 1;
  CHECK_RESULT(lang_net_bind_v1(static_cast<int32_t>(fd), &address), -EINVAL);
  address.flags = 0;

  address.family = 3;
  CHECK_RESULT(lang_net_bind_v1(static_cast<int32_t>(fd), &address),
               -EAFNOSUPPORT);
  address.family = AF_INET;

  address.data = nullptr;
  CHECK_RESULT(lang_net_bind_v1(static_cast<int32_t>(fd), &address), -EINVAL);
  address.data = &vector;

  vector.stride = 2;
  CHECK_RESULT(lang_net_bind_v1(static_cast<int32_t>(fd), &address), -EINVAL);
  vector.stride = 1;

  vector.initialized = 3;
  CHECK_RESULT(lang_net_bind_v1(static_cast<int32_t>(fd), &address), -EINVAL);
  vector.initialized = 4;

  vector.copy = &dummy_value_op;
  CHECK_RESULT(lang_net_bind_v1(static_cast<int32_t>(fd), &address), -EINVAL);
  vector.copy = nullptr;

  vector.drop = &dummy_value_op;
  CHECK_RESULT(lang_net_bind_v1(static_cast<int32_t>(fd), &address), -EINVAL);
  vector.drop = nullptr;

  vector.length = 3;
  vector.initialized = 3;
  CHECK_RESULT(lang_net_bind_v1(static_cast<int32_t>(fd), &address), -EINVAL);
  vector.length = 4;
  vector.initialized = 4;

  uint8_t *saved_data = static_cast<uint8_t *>(vector.data);
  vector.data = nullptr;
  CHECK_RESULT(lang_net_bind_v1(static_cast<int32_t>(fd), &address), -EINVAL);
  vector.data = saved_data;

  address.family = AF_INET6;
  CHECK_RESULT(lang_net_bind_v1(static_cast<int32_t>(fd), &address), -EINVAL);
  address.family = AF_INET;

  CHECK_RESULT(lang_net_sendto_v1(static_cast<int32_t>(fd), nullptr, 0, 0,
                                  nullptr),
               -EFAULT);

  CHECK_RESULT(lang_net_setsockopt_bytes_v1(
                   static_cast<int32_t>(fd), SOL_SOCKET, SO_LINGER, nullptr, 1),
               -EFAULT);
  CHECK_RESULT(lang_net_setsockopt_bytes_v1(
                   static_cast<int32_t>(fd), SOL_SOCKET, SO_LINGER, bytes4,
                   UINT64_MAX),
               -EINVAL);

  uint64_t length = UINT64_MAX;
  CHECK_RESULT(lang_net_getsockopt_bytes_v1(static_cast<int32_t>(fd),
                                            SOL_SOCKET, SO_LINGER, bytes4,
                                            &length),
               -EINVAL);
  length = 4;
  CHECK_RESULT(lang_net_getsockopt_bytes_v1(static_cast<int32_t>(fd),
                                            SOL_SOCKET, SO_LINGER, nullptr,
                                            &length),
               -EFAULT);
  length = 4;
  CHECK_RESULT(lang_net_getsockopt_bytes_v1(static_cast<int32_t>(fd),
                                            SOL_SOCKET, SO_LINGER, bytes4,
                                            nullptr),
               -EFAULT);

  CHECK_RESULT(lang_net_getsockopt_int_v1(static_cast<int32_t>(fd), SOL_SOCKET,
                                          SO_REUSEADDR, nullptr),
               -EFAULT);

  CHECK_RESULT(lang_net_socketpair_v1(AF_UNIX, SOCK_STREAM, 0, nullptr),
               -EFAULT);
  int32_t sentinel[2] = {-7, -9};
  CHECK_TRUE(lang_net_socketpair_v1(999, SOCK_STREAM, 0, sentinel) < 0);
  CHECK_RESULT(sentinel[0], -7);
  CHECK_RESULT(sentinel[1], -9);

  CHECK_RESULT(lang_net_poll_v1(nullptr, 1, 0), -EFAULT);
  CHECK_RESULT(lang_net_poll_v1(nullptr, 0, 0), 0);
  LangPollFd row = {static_cast<int32_t>(fd), POLLIN, 0};
  CHECK_RESULT(lang_net_poll_v1(&row, UINT64_MAX, 0), -EINVAL);

  CHECK_RESULT(lang_net_inet_ntop_v1(nullptr, nullptr, 0), -EFAULT);
  CHECK_RESULT(lang_net_inet_ntop_v1(&address, nullptr, 16), -EFAULT);

  LangNetAddress untouched = {};
  untouched.family = 123;
  CHECK_RESULT(lang_net_getsockname_v1(-1, &untouched), -EBADF);
  CHECK_RESULT(untouched.family, 123);
  CHECK_TRUE(untouched.data == nullptr);

  const int64_t unix_fd = lang_net_socket_v1(AF_UNIX, SOCK_DGRAM, 0);
  CHECK_TRUE(unix_fd >= 0);
  if (unix_fd >= 0) {
    uint8_t unix_bytes[108];
    std::memset(unix_bytes, 'u', sizeof unix_bytes);
    LangVec unix_vector = borrowed_vec(unix_bytes, sizeof unix_bytes);
    LangNetAddress unix_address;
    set_address(&unix_address, AF_UNIX, 0, 0, 0, &unix_vector);
    CHECK_RESULT(lang_net_bind_v1(static_cast<int32_t>(unix_fd),
                                  &unix_address),
                 -ENAMETOOLONG);
    unix_address.flags = 1;
    CHECK_RESULT(lang_net_bind_v1(static_cast<int32_t>(unix_fd),
                                  &unix_address),
                 -ENAMETOOLONG);
    unix_address.flags = 0;

    unix_vector.length = 3;
    unix_vector.initialized = 3;
    unix_bytes[1] = 0;
    CHECK_RESULT(lang_net_bind_v1(static_cast<int32_t>(unix_fd),
                                  &unix_address),
                 -EINVAL);
    close_fd(unix_fd);
  }

  close_fd(fd);
}

void test_inet_conversion() {
  const char *valid_ipv4[] = {"127.0.0.1", "0.0.0.0", "255.255.255.255",
                              "192.168.1.10", "10.0.0.255"};
  for (const char *text : valid_ipv4) {
    uint8_t expected[4] = {};
    CHECK_TRUE(inet_pton(AF_INET, text, expected) == 1);

    LangNetAddress address = {};
    CHECK_RESULT(lang_net_inet_pton_v1(AF_INET, text,
                                       static_cast<uint64_t>(std::strlen(text)),
                                       &address),
                 0);
    check_address(address, AF_INET, 0, 0, 0, expected, 4);

    char expected_text[INET_ADDRSTRLEN] = {};
    CHECK_TRUE(inet_ntop(AF_INET, expected, expected_text,
                         sizeof expected_text) != nullptr);
    char output[64] = {};
    const int64_t count =
        lang_net_inet_ntop_v1(&address, output, sizeof output);
    CHECK_RESULT(count, static_cast<int64_t>(std::strlen(expected_text)));
    CHECK_TRUE(std::strcmp(output, expected_text) == 0);

    char exact[INET_ADDRSTRLEN] = {};
    CHECK_RESULT(lang_net_inet_ntop_v1(&address, exact,
                                       static_cast<uint64_t>(count) + 1),
                 count);
    CHECK_TRUE(std::strcmp(exact, expected_text) == 0);

    char sentinel[32];
    std::memset(sentinel, 0x5a, sizeof sentinel);
    CHECK_RESULT(lang_net_inet_ntop_v1(&address, sentinel,
                                       static_cast<uint64_t>(count)),
                 -ENOSPC);
    bool unchanged = true;
    for (unsigned char byte : sentinel)
      if (byte != 0x5a) unchanged = false;
    CHECK_TRUE(unchanged);

    char big[64] = {};
    CHECK_RESULT(lang_net_inet_ntop_v1(&address, big, UINT64_MAX), count);
    CHECK_TRUE(std::strcmp(big, expected_text) == 0);

    CHECK_RESULT(lang_net_inet_ntop_v1(&address, output, 0), -ENOSPC);
    release_address(&address);
  }

  const char *invalid_ipv4[] = {
      "",           "256.0.0.1", "1.2.3",     "1.2.3.4.5",
      "1.2.3.-1",   "1.2.3.4x",  " 1.2.3.4",  "1.2.3.4 ",
      "1..2.3",     "1.2.3.",    "1.2.3.256", "1.2.3.4\n",
      "0x1.2.3.4",  "1.2.3.4/24", "001.002.003.004",
      "01.2.3.4",   "1.2.3.04"};
  for (const char *text : invalid_ipv4) {
    LangNetAddress address = {};
    address.family = 77;
    CHECK_TRUE(lang_net_inet_pton_v1(
                   AF_INET, text, static_cast<uint64_t>(std::strlen(text)),
                   &address) < 0);
    CHECK_RESULT(address.family, 77);
    CHECK_TRUE(address.data == nullptr);
  }

  {
    const char embedded[] = {'1', '2', '7', '.', '0', '.', '0',
                             '.', '1', '\0', 'x'};
    LangNetAddress address = {};
    CHECK_RESULT(lang_net_inet_pton_v1(AF_INET, embedded, sizeof embedded,
                                       &address),
                 -EINVAL);
    const char bounded[] = {'1', '2', '7', '.', '0', '.', '0',
                            '.', '1', 'X', 'Y'};
    CHECK_RESULT(lang_net_inet_pton_v1(AF_INET, bounded, 9, &address), 0);
    const uint8_t bounded_expected[4] = {127, 0, 0, 1};
    check_address(address, AF_INET, 0, 0, 0, bounded_expected, 4);
    release_address(&address);
    CHECK_RESULT(lang_net_inet_pton_v1(AF_INET, bounded, 10, &address),
                 -EINVAL);
  }

  {
    LangNetAddress address = {};
    CHECK_RESULT(lang_net_inet_pton_v1(AF_INET, nullptr, 0, &address),
                 -EINVAL);
    CHECK_RESULT(lang_net_inet_pton_v1(AF_INET, nullptr, 1, &address),
                 -EFAULT);
    CHECK_RESULT(lang_net_inet_pton_v1(AF_INET, "1.2.3.4", 7, nullptr),
                 -EFAULT);
    CHECK_RESULT(lang_net_inet_pton_v1(AF_UNIX, "1.2.3.4", 7, &address),
                 -EAFNOSUPPORT);
  }

  struct Ipv6Case {
    const char *input;
    const char *output;
  };
  const Ipv6Case valid_ipv6[] = {
      {"::", "::"},
      {"::1", "::1"},
      {"1::", "1::"},
      {"2001:db8::1", "2001:db8::1"},
      {"2001:0db8:0000:0000:0000:ff00:0042:8329",
       "2001:db8::ff00:42:8329"},
      {"::ffff:192.0.2.1", "::ffff:192.0.2.1"},
      {"0:0:0:0:0:ffff:c000:201", "::ffff:192.0.2.1"},
      {"::192.0.2.1", "::c000:201"},
      {"1:0:0:2:0:0:3:4", "1::2:0:0:3:4"},
      {"1:0:0:2:0:0:0:3", "1:0:0:2::3"},
      {"0:0:1:0:0:0:0:0", "0:0:1::"},
      {"1:2:3:4:5:6:7:8", "1:2:3:4:5:6:7:8"},
      {"fe80::1", "fe80::1"},
      {"1:2:3:4:5:6:192.0.2.1", "1:2:3:4:5:6:c000:201"},
      {"1:2:3:4:5::192.0.2.1", "1:2:3:4:5:0:c000:201"},
  };
  for (const Ipv6Case &test : valid_ipv6) {
    uint8_t expected[16] = {};
    CHECK_TRUE(inet_pton(AF_INET6, test.input, expected) == 1);
    LangNetAddress address = {};
    CHECK_RESULT(lang_net_inet_pton_v1(
                     AF_INET6, test.input,
                     static_cast<uint64_t>(std::strlen(test.input)), &address),
                 0);
    check_address(address, AF_INET6, 0, 0, 0, expected, 16);

    char output[64] = {};
    const int64_t count =
        lang_net_inet_ntop_v1(&address, output, sizeof output);
    CHECK_RESULT(count, static_cast<int64_t>(std::strlen(test.output)));
    CHECK_TRUE(std::strcmp(output, test.output) == 0);
    release_address(&address);
  }

  const char *invalid_ipv6[] = {
      "",                     ":",                   ":::",
      "1:2:3:4:5:6:7",        "1:2:3:4:5:6:7:8:9",   "1:2:3:4:5:6:7:",
      "1:2:3:4:5:6:7:8::",    "::g",                 "12345::",
      "1:2:3:4:5:6:7:8g",     "[::1]",               "::1%eth0",
      "::1%1",                "::ffff:256.0.0.1",    "::ffff:1.2.3",
      "::ffff:1.2.3.4:5",     "1:2:3:4:5:6:192.0.2.1:7",
      "1:2:3:4:5:6:7:192.0.2.1", "::ffff:192.0.2.1x", "  ::1",
      "::1 "};
  for (const char *text : invalid_ipv6) {
    LangNetAddress address = {};
    address.family = 88;
    CHECK_TRUE(lang_net_inet_pton_v1(
                   AF_INET6, text, static_cast<uint64_t>(std::strlen(text)),
                   &address) < 0);
    CHECK_RESULT(address.family, 88);
    CHECK_TRUE(address.data == nullptr);
  }

  {
    LangNetAddress address = {};
    CHECK_RESULT(lang_net_inet_pton_v1(AF_INET6, "fe80::1", 7, &address), 0);
    address.scope = 5;
    char output[64] = {};
    const int64_t count =
        lang_net_inet_ntop_v1(&address, output, sizeof output);
    CHECK_RESULT(count, 7);
    CHECK_TRUE(std::strcmp(output, "fe80::1") == 0);
    release_address(&address);
  }

  {
    LangNetAddress address = {};
    address.family = AF_UNIX;
    CHECK_RESULT(lang_net_inet_ntop_v1(&address, nullptr, 16),
                 -EAFNOSUPPORT);
  }
}

void test_tcp_ipv4() {
  uint8_t loopback[4] = {127, 0, 0, 1};
  TcpFixture fixture;
  CHECK_TRUE(make_tcp_fixture(AF_INET, loopback, 4, 0, &fixture));
  if (fixture.listener < 0) return;

  CHECK_RESULT(fixture.bound.family, AF_INET);
  CHECK_TRUE(fixture.bound.port != 0);
  check_address(fixture.bound, AF_INET, fixture.bound.port, 0, 0, loopback, 4);

  LangNetAddress client_name = {};
  CHECK_RESULT(lang_net_getsockname_v1(static_cast<int32_t>(fixture.client),
                                       &client_name),
               0);
  CHECK_RESULT(client_name.family, AF_INET);
  CHECK_RESULT(fixture.peer.family, AF_INET);
  CHECK_RESULT(fixture.peer.port, client_name.port);
  check_vec(fixture.peer.data, static_cast<const uint8_t *>(client_name.data->data),
            4);

  LangNetAddress server_peer = {};
  CHECK_RESULT(lang_net_getpeername_v1(static_cast<int32_t>(fixture.client),
                                       &server_peer),
               0);
  CHECK_RESULT(server_peer.family, AF_INET);
  CHECK_RESULT(server_peer.port, fixture.bound.port);
  check_vec(server_peer.data, loopback, 4);
  release_address(&server_peer);

  const uint8_t payload[] = {0x10, 0x20, 0x30, 0x40, 0x50};
  CHECK_RESULT(lang_net_send_v1(static_cast<int32_t>(fixture.client), payload,
                                sizeof payload, 0),
               static_cast<int64_t>(sizeof payload));
  uint8_t receive_buffer[32] = {};
  CHECK_RESULT(lang_net_recv_v1(static_cast<int32_t>(fixture.accepted),
                                receive_buffer, sizeof receive_buffer, 0),
               static_cast<int64_t>(sizeof payload));
  CHECK_TRUE(std::memcmp(receive_buffer, payload, sizeof payload) == 0);

  const uint8_t full[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  CHECK_RESULT(lang_net_send_v1(static_cast<int32_t>(fixture.accepted), full,
                                sizeof full, 0),
               static_cast<int64_t>(sizeof full));
  uint8_t short_buffer[4] = {};
  CHECK_RESULT(lang_net_recv_v1(static_cast<int32_t>(fixture.client),
                                short_buffer, 4, 0),
               4);
  CHECK_TRUE(std::memcmp(short_buffer, full, 4) == 0);
  CHECK_RESULT(lang_net_recv_v1(static_cast<int32_t>(fixture.client),
                                short_buffer, 4, 0),
               4);
  CHECK_TRUE(std::memcmp(short_buffer, full + 4, 4) == 0);
  CHECK_RESULT(lang_net_recv_v1(static_cast<int32_t>(fixture.client),
                                short_buffer, 2, 0),
               2);
  CHECK_TRUE(std::memcmp(short_buffer, full + 8, 2) == 0);

  CHECK_RESULT(lang_net_send_v1(static_cast<int32_t>(fixture.client), nullptr,
                                0, 0),
               0);
  CHECK_RESULT(lang_net_send_v1(static_cast<int32_t>(fixture.client), nullptr,
                                1, 0),
               -EFAULT);
  CHECK_RESULT(lang_net_send_v1(static_cast<int32_t>(fixture.client), payload,
                                UINT64_MAX, 0),
               -EINVAL);
  CHECK_RESULT(lang_net_recv_v1(static_cast<int32_t>(fixture.accepted),
                                receive_buffer, UINT64_MAX, 0),
               -EINVAL);

  CHECK_RESULT(lang_net_shutdown_v1(static_cast<int32_t>(fixture.client),
                                    SHUT_WR),
               0);
  CHECK_RESULT(lang_net_recv_v1(static_cast<int32_t>(fixture.accepted),
                                receive_buffer, sizeof receive_buffer, 0),
               0);

  const int64_t second_client = lang_net_socket_v1(AF_INET, SOCK_STREAM, 0);
  CHECK_TRUE(second_client >= 0);
  if (second_client >= 0) {
    CHECK_RESULT(lang_net_connect_v1(static_cast<int32_t>(second_client),
                                     &fixture.bound),
                 0);
    const int64_t second_accepted = lang_net_accept_v1(
        static_cast<int32_t>(fixture.listener), nullptr, SOCK_CLOEXEC);
    CHECK_TRUE(second_accepted >= 0);
    if (second_accepted >= 0) {
      CHECK_TRUE((fcntl(static_cast<int>(second_accepted), F_GETFD) &
                  FD_CLOEXEC) != 0);
    }
    close_fd(second_accepted);
    close_fd(second_client);
  }

  const int64_t accepted_fd = fixture.accepted;
  CHECK_TRUE((fcntl(static_cast<int>(accepted_fd), F_GETFD) & FD_CLOEXEC) != 0);

  CHECK_RESULT(lang_net_accept_v1(-1, nullptr, 0), -EBADF);
  CHECK_RESULT(lang_net_listen_v1(-1, 1), -EBADF);
  CHECK_RESULT(lang_net_getsockname_v1(-1, &client_name), -EBADF);
  CHECK_RESULT(lang_net_getpeername_v1(-1, &client_name), -EBADF);
  CHECK_RESULT(lang_net_shutdown_v1(-1, SHUT_RDWR), -EBADF);
  CHECK_RESULT(lang_net_send_v1(-1, payload, sizeof payload, 0), -EBADF);
  CHECK_RESULT(lang_net_recv_v1(-1, receive_buffer, 1, 0), -EBADF);

  release_address(&client_name);
  close_tcp_fixture(&fixture);
}

void test_tcp_ipv6(bool ipv6_available) {
  if (!ipv6_available) {
    std::fprintf(stderr, "note: IPv6 unavailable; IPv6 TCP checks skipped\n");
    return;
  }
  uint8_t loopback[16] = {};
  CHECK_TRUE(inet_pton(AF_INET6, "::1", loopback) == 1);
  TcpFixture fixture;
  CHECK_TRUE(make_tcp_fixture(AF_INET6, loopback, 16, 0, &fixture));
  if (fixture.listener < 0) return;

  CHECK_RESULT(fixture.bound.family, AF_INET6);
  CHECK_TRUE(fixture.bound.port != 0);
  CHECK_RESULT(fixture.bound.scope, 0);
  check_address(fixture.bound, AF_INET6, fixture.bound.port, 0, 0, loopback,
                16);

  const uint8_t payload[] = "ipv6-tcp";
  CHECK_RESULT(lang_net_send_v1(static_cast<int32_t>(fixture.client), payload,
                                sizeof payload - 1, 0),
               static_cast<int64_t>(sizeof payload - 1));
  uint8_t receive_buffer[32] = {};
  CHECK_RESULT(lang_net_recv_v1(static_cast<int32_t>(fixture.accepted),
                                receive_buffer, sizeof receive_buffer, 0),
               static_cast<int64_t>(sizeof payload - 1));
  CHECK_TRUE(std::memcmp(receive_buffer, payload, sizeof payload - 1) == 0);

  CHECK_RESULT(lang_net_shutdown_v1(static_cast<int32_t>(fixture.client),
                                    SHUT_WR),
               0);
  CHECK_RESULT(lang_net_recv_v1(static_cast<int32_t>(fixture.accepted),
                                receive_buffer, sizeof receive_buffer, 0),
               0);
  close_tcp_fixture(&fixture);
}

void test_udp_ipv4() {
  uint8_t loopback[4] = {127, 0, 0, 1};
  LangNetAddress receiver_address = {};
  const int64_t receiver = make_bound_socket(AF_INET, SOCK_DGRAM, loopback, 4, 0,
                                             &receiver_address);
  CHECK_TRUE(receiver >= 0);
  if (receiver < 0) return;

  const int64_t sender = lang_net_socket_v1(AF_INET, SOCK_DGRAM, 0);
  CHECK_TRUE(sender >= 0);
  if (sender >= 0) {
    const uint8_t payload[] = {0xde, 0xad, 0xbe, 0xef};
    CHECK_RESULT(lang_net_sendto_v1(static_cast<int32_t>(sender), payload,
                                    sizeof payload, 0, &receiver_address),
                 static_cast<int64_t>(sizeof payload));

    LangNetAddress source = {};
    uint8_t receive_buffer[32] = {};
    CHECK_RESULT(lang_net_recvfrom_v1(static_cast<int32_t>(receiver),
                                      receive_buffer, sizeof receive_buffer,
                                      0, &source),
                 static_cast<int64_t>(sizeof payload));
    CHECK_TRUE(std::memcmp(receive_buffer, payload, sizeof payload) == 0);
    CHECK_RESULT(source.family, AF_INET);

    LangNetAddress sender_name = {};
    CHECK_RESULT(lang_net_getsockname_v1(static_cast<int32_t>(sender),
                                         &sender_name),
                 0);
    CHECK_RESULT(source.port, sender_name.port);
    check_vec(source.data, loopback, 4);
    release_address(&sender_name);
    release_address(&source);

    CHECK_RESULT(lang_net_sendto_v1(static_cast<int32_t>(sender), nullptr, 0, 0,
                                    &receiver_address),
                 0);
    CHECK_RESULT(lang_net_sendto_v1(static_cast<int32_t>(sender), "Z", 1, 0,
                                    &receiver_address),
                 1);
    LangNetAddress zero_source = {};
    CHECK_RESULT(lang_net_recvfrom_v1(static_cast<int32_t>(receiver), nullptr,
                                      0, 0, &zero_source),
                 0);
    release_address(&zero_source);
    CHECK_RESULT(lang_net_recvfrom_v1(static_cast<int32_t>(receiver),
                                      receive_buffer, 1, 0, nullptr),
                 1);
    CHECK_RESULT(receive_buffer[0], 'Z');

    CHECK_RESULT(lang_net_connect_v1(static_cast<int32_t>(sender),
                                     &receiver_address),
                 0);
    CHECK_RESULT(lang_net_send_v1(static_cast<int32_t>(sender), nullptr, 0, 0),
                 0);
    CHECK_RESULT(lang_net_send_v1(static_cast<int32_t>(sender), "Q", 1, 0), 1);
    CHECK_RESULT(lang_net_recv_v1(static_cast<int32_t>(receiver), nullptr, 0, 0),
                 0);
    CHECK_RESULT(lang_net_recv_v1(static_cast<int32_t>(receiver),
                                  receive_buffer, 1, 0),
                 1);
    CHECK_RESULT(receive_buffer[0], 'Q');

    CHECK_RESULT(lang_net_sendto_v1(static_cast<int32_t>(sender), payload,
                                    sizeof payload, 0, nullptr),
                 -EFAULT);
    CHECK_RESULT(lang_net_sendto_v1(static_cast<int32_t>(sender), nullptr, 1, 0,
                                    &receiver_address),
                 -EFAULT);
    CHECK_RESULT(lang_net_sendto_v1(static_cast<int32_t>(sender), payload,
                                    UINT64_MAX, 0, &receiver_address),
                 -EINVAL);
    CHECK_RESULT(lang_net_recvfrom_v1(static_cast<int32_t>(receiver), nullptr,
                                      1, 0, nullptr),
                 -EFAULT);
    CHECK_RESULT(lang_net_recvfrom_v1(static_cast<int32_t>(receiver),
                                      receive_buffer, UINT64_MAX, 0, nullptr),
                 -EINVAL);
    close_fd(sender);
  }

  release_address(&receiver_address);
  close_fd(receiver);
}

void test_udp_ipv6(bool ipv6_available) {
  if (!ipv6_available) {
    std::fprintf(stderr, "note: IPv6 unavailable; IPv6 UDP checks skipped\n");
    return;
  }
  uint8_t loopback[16] = {};
  CHECK_TRUE(inet_pton(AF_INET6, "::1", loopback) == 1);
  LangNetAddress receiver_address = {};
  const int64_t receiver = make_bound_socket(AF_INET6, SOCK_DGRAM, loopback, 16,
                                             0, &receiver_address);
  CHECK_TRUE(receiver >= 0);
  if (receiver < 0) return;

  const int64_t sender = lang_net_socket_v1(AF_INET6, SOCK_DGRAM, 0);
  CHECK_TRUE(sender >= 0);
  if (sender >= 0) {
    const uint8_t payload[] = "ipv6-udp";
    CHECK_RESULT(lang_net_sendto_v1(static_cast<int32_t>(sender), payload,
                                    sizeof payload - 1, 0,
                                    &receiver_address),
                 static_cast<int64_t>(sizeof payload - 1));
    LangNetAddress source = {};
    uint8_t receive_buffer[32] = {};
    CHECK_RESULT(lang_net_recvfrom_v1(static_cast<int32_t>(receiver),
                                      receive_buffer, sizeof receive_buffer,
                                      0, &source),
                 static_cast<int64_t>(sizeof payload - 1));
    CHECK_TRUE(std::memcmp(receive_buffer, payload, sizeof payload - 1) == 0);
    CHECK_RESULT(source.family, AF_INET6);
    CHECK_RESULT(source.scope, 0);
    check_vec(source.data, loopback, 16);
    release_address(&source);
    close_fd(sender);
  }

  release_address(&receiver_address);
  close_fd(receiver);
}

void test_unix_pathname(const char *directory) {
  char path[512];
  CHECK_TRUE(make_path(path, sizeof path, directory, "unix-stream"));
  const uint64_t path_length = std::strlen(path);
  CHECK_TRUE(path_length < 108);

  TcpFixture fixture;
  CHECK_TRUE(make_tcp_fixture(AF_UNIX,
                              reinterpret_cast<const uint8_t *>(path),
                              path_length, 0, &fixture));
  if (fixture.listener < 0) return;

  CHECK_RESULT(fixture.bound.family, AF_UNIX);
  CHECK_RESULT(fixture.bound.flags, 0);
  check_vec(fixture.bound.data, reinterpret_cast<const uint8_t *>(path),
            path_length);
  CHECK_RESULT(fixture.peer.family, AF_UNIX);
  CHECK_TRUE(fixture.peer.data != nullptr);
  if (fixture.peer.data != nullptr)
    CHECK_RESULT(fixture.peer.data->length, 0);

  LangNetAddress server_peer = {};
  CHECK_RESULT(lang_net_getpeername_v1(static_cast<int32_t>(fixture.client),
                                       &server_peer),
               0);
  CHECK_RESULT(server_peer.family, AF_UNIX);
  CHECK_RESULT(server_peer.flags, 0);
  check_vec(server_peer.data, reinterpret_cast<const uint8_t *>(path),
            path_length);
  release_address(&server_peer);

  const uint8_t payload[] = "unix-path";
  CHECK_RESULT(lang_net_send_v1(static_cast<int32_t>(fixture.client), payload,
                                sizeof payload - 1, 0),
               static_cast<int64_t>(sizeof payload - 1));
  uint8_t receive_buffer[32] = {};
  CHECK_RESULT(lang_net_recv_v1(static_cast<int32_t>(fixture.accepted),
                                receive_buffer, sizeof receive_buffer, 0),
               static_cast<int64_t>(sizeof payload - 1));
  CHECK_TRUE(std::memcmp(receive_buffer, payload, sizeof payload - 1) == 0);

  CHECK_RESULT(lang_net_shutdown_v1(static_cast<int32_t>(fixture.client),
                                    SHUT_WR),
               0);
  CHECK_RESULT(lang_net_recv_v1(static_cast<int32_t>(fixture.accepted),
                                receive_buffer, sizeof receive_buffer, 0),
               0);

  close_tcp_fixture(&fixture);
  ::unlink(path);
}

void test_unix_abstract() {
  char name[64];
  const int name_length =
      std::snprintf(name, sizeof name, "woozy-abstract-%d", getpid());
  CHECK_TRUE(name_length > 0);
  if (name_length <= 0) return;

  TcpFixture fixture;
  CHECK_TRUE(make_tcp_fixture(AF_UNIX,
                              reinterpret_cast<const uint8_t *>(name),
                              static_cast<uint64_t>(name_length), 1, &fixture));
  if (fixture.listener < 0) return;

  CHECK_RESULT(fixture.bound.family, AF_UNIX);
  CHECK_RESULT(fixture.bound.flags, 1);
  check_vec(fixture.bound.data, reinterpret_cast<const uint8_t *>(name),
            static_cast<uint64_t>(name_length));

  const uint8_t payload[] = "unix-abstract";
  CHECK_RESULT(lang_net_send_v1(static_cast<int32_t>(fixture.client), payload,
                                sizeof payload - 1, 0),
               static_cast<int64_t>(sizeof payload - 1));
  uint8_t receive_buffer[32] = {};
  CHECK_RESULT(lang_net_recv_v1(static_cast<int32_t>(fixture.accepted),
                                receive_buffer, sizeof receive_buffer, 0),
               static_cast<int64_t>(sizeof payload - 1));
  CHECK_TRUE(std::memcmp(receive_buffer, payload, sizeof payload - 1) == 0);

  close_tcp_fixture(&fixture);

  // Abstract names may contain NUL bytes; only pathname data rejects them.
  uint8_t nul_name[64];
  std::memcpy(nul_name, name, static_cast<size_t>(name_length));
  nul_name[static_cast<size_t>(name_length) / 2] = 0;
  LangNetAddress nul_address = {};
  const int64_t nul_fd = make_bound_socket(
      AF_UNIX, SOCK_STREAM, nul_name, static_cast<uint64_t>(name_length), 1,
      &nul_address);
  CHECK_TRUE(nul_fd >= 0);
  if (nul_fd >= 0) {
    CHECK_RESULT(nul_address.family, AF_UNIX);
    CHECK_RESULT(nul_address.flags, 1);
    check_vec(nul_address.data, nul_name,
              static_cast<uint64_t>(name_length));
    release_address(&nul_address);
    close_fd(nul_fd);
  }

  // Exactly 107 abstract-name bytes is the maximum that fits sun_path[108]
  // after the leading abstract NUL.
  uint8_t max_name[107];
  for (uint8_t &byte : max_name) byte = 'x';
  const int max_prefix =
      std::snprintf(reinterpret_cast<char *>(max_name), sizeof max_name,
                    "woozy-max-%d", getpid());
  CHECK_TRUE(max_prefix > 0 && max_prefix < 107);
  if (max_prefix > 0 && max_prefix < 107) {
    for (int index = max_prefix; index < 107; ++index) max_name[index] = 'y';
    LangNetAddress max_address = {};
    const int64_t max_fd = make_bound_socket(
        AF_UNIX, SOCK_STREAM, max_name, sizeof max_name, 1, &max_address);
    CHECK_TRUE(max_fd >= 0);
    if (max_fd >= 0) {
      CHECK_RESULT(max_address.family, AF_UNIX);
      CHECK_RESULT(max_address.flags, 1);
      check_vec(max_address.data, max_name, sizeof max_name);
      release_address(&max_address);
      close_fd(max_fd);
    }
  }
}

void test_unix_datagram(const char *directory) {
  char path[512];
  CHECK_TRUE(make_path(path, sizeof path, directory, "unix-dgram"));
  const uint64_t path_length = std::strlen(path);

  char sender_path[512];
  CHECK_TRUE(make_path(sender_path, sizeof sender_path, directory,
                       "unix-dgram-sender"));
  const uint64_t sender_path_length = std::strlen(sender_path);

  LangNetAddress receiver_address = {};
  const int64_t receiver =
      make_bound_socket(AF_UNIX, SOCK_DGRAM,
                        reinterpret_cast<const uint8_t *>(path), path_length,
                        0, &receiver_address);
  CHECK_TRUE(receiver >= 0);
  if (receiver >= 0) {
    LangNetAddress sender_address = {};
    const int64_t sender = make_bound_socket(
        AF_UNIX, SOCK_DGRAM,
        reinterpret_cast<const uint8_t *>(sender_path), sender_path_length, 0,
        &sender_address);
    CHECK_TRUE(sender >= 0);
    if (sender >= 0) {
      const uint8_t payload[] = "unix-dgram-payload";
      CHECK_RESULT(lang_net_sendto_v1(static_cast<int32_t>(sender), payload,
                                      sizeof payload - 1, 0,
                                      &receiver_address),
                   static_cast<int64_t>(sizeof payload - 1));
      LangNetAddress source = {};
      uint8_t receive_buffer[64] = {};
      CHECK_RESULT(lang_net_recvfrom_v1(static_cast<int32_t>(receiver),
                                        receive_buffer,
                                        sizeof receive_buffer, 0, &source),
                   static_cast<int64_t>(sizeof payload - 1));
      CHECK_TRUE(std::memcmp(receive_buffer, payload, sizeof payload - 1) == 0);
      CHECK_RESULT(source.family, AF_UNIX);
      CHECK_RESULT(source.flags, 0);
      check_vec(source.data,
                reinterpret_cast<const uint8_t *>(sender_path),
                sender_path_length);
      release_address(&source);

      CHECK_RESULT(lang_net_sendto_v1(static_cast<int32_t>(sender), nullptr, 0,
                                      0, &receiver_address),
                   0);
      CHECK_RESULT(lang_net_recvfrom_v1(static_cast<int32_t>(receiver), nullptr,
                                        0, 0, nullptr),
                   0);
      close_fd(sender);
    }
    release_address(&sender_address);
    release_address(&receiver_address);
    close_fd(receiver);
  }
  ::unlink(path);
  ::unlink(sender_path);

  char receiver_name[64];
  char sender_name[64];
  const int receiver_length =
      std::snprintf(receiver_name, sizeof receiver_name, "woozy-dgram-r-%d",
                    getpid());
  const int sender_length =
      std::snprintf(sender_name, sizeof sender_name, "woozy-dgram-s-%d",
                    getpid());
  CHECK_TRUE(receiver_length > 0 && sender_length > 0);
  if (receiver_length <= 0 || sender_length <= 0) return;

  LangNetAddress abstract_receiver_address = {};
  const int64_t abstract_receiver = make_bound_socket(
      AF_UNIX, SOCK_DGRAM,
      reinterpret_cast<const uint8_t *>(receiver_name),
      static_cast<uint64_t>(receiver_length), 1, &abstract_receiver_address);
  CHECK_TRUE(abstract_receiver >= 0);
  if (abstract_receiver < 0) return;

  LangNetAddress abstract_sender_address = {};
  const int64_t abstract_sender = make_bound_socket(
      AF_UNIX, SOCK_DGRAM, reinterpret_cast<const uint8_t *>(sender_name),
      static_cast<uint64_t>(sender_length), 1, &abstract_sender_address);
  CHECK_TRUE(abstract_sender >= 0);
  if (abstract_sender >= 0) {
    CHECK_RESULT(lang_net_sendto_v1(static_cast<int32_t>(abstract_sender), "A",
                                    1, 0, &abstract_receiver_address),
                 1);
    LangNetAddress source = {};
    uint8_t byte = 0;
    CHECK_RESULT(lang_net_recvfrom_v1(static_cast<int32_t>(abstract_receiver),
                                      &byte, 1, 0, &source),
                 1);
    CHECK_RESULT(byte, 'A');
    CHECK_RESULT(source.family, AF_UNIX);
    CHECK_RESULT(source.flags, 1);
    check_vec(source.data,
              reinterpret_cast<const uint8_t *>(sender_name),
              static_cast<uint64_t>(sender_length));
    release_address(&source);
    close_fd(abstract_sender);
  }
  release_address(&abstract_sender_address);
  release_address(&abstract_receiver_address);
  close_fd(abstract_receiver);
}

void test_socketpair() {
  int32_t stream_pair[2] = {-1, -1};
  CHECK_RESULT(lang_net_socketpair_v1(AF_UNIX, SOCK_STREAM, 0, stream_pair), 0);
  CHECK_TRUE(stream_pair[0] >= 0 && stream_pair[1] >= 0);
  if (stream_pair[0] >= 0 && stream_pair[1] >= 0) {
    const uint8_t payload[] = "pair-stream";
    CHECK_RESULT(lang_net_send_v1(stream_pair[0], payload, sizeof payload - 1,
                                  0),
                 static_cast<int64_t>(sizeof payload - 1));
    uint8_t receive_buffer[32] = {};
    CHECK_RESULT(lang_net_recv_v1(stream_pair[1], receive_buffer,
                                  sizeof receive_buffer, 0),
                 static_cast<int64_t>(sizeof payload - 1));
    CHECK_TRUE(std::memcmp(receive_buffer, payload, sizeof payload - 1) == 0);

    CHECK_RESULT(lang_net_send_v1(stream_pair[0], nullptr, 0, 0), 0);

    LangNetAddress name = {};
    CHECK_RESULT(lang_net_getsockname_v1(stream_pair[0], &name), 0);
    CHECK_RESULT(name.family, AF_UNIX);
    CHECK_RESULT(name.data->length, 0);
    release_address(&name);

    close_fd(stream_pair[0]);
    close_fd(stream_pair[1]);
  }

  int32_t dgram_pair[2] = {-1, -1};
  CHECK_RESULT(lang_net_socketpair_v1(AF_UNIX, SOCK_DGRAM, 0, dgram_pair), 0);
  CHECK_TRUE(dgram_pair[0] >= 0 && dgram_pair[1] >= 0);
  if (dgram_pair[0] >= 0 && dgram_pair[1] >= 0) {
    CHECK_RESULT(lang_net_send_v1(dgram_pair[0], nullptr, 0, 0), 0);
    CHECK_RESULT(lang_net_send_v1(dgram_pair[0], "D", 1, 0), 1);
    CHECK_RESULT(lang_net_recv_v1(dgram_pair[1], nullptr, 0, 0), 0);
    uint8_t byte = 0;
    CHECK_RESULT(lang_net_recv_v1(dgram_pair[1], &byte, 1, 0), 1);
    CHECK_RESULT(byte, 'D');
    close_fd(dgram_pair[0]);
    close_fd(dgram_pair[1]);
  }

  int32_t sentinel[2] = {-7, -9};
  CHECK_TRUE(lang_net_socketpair_v1(999, SOCK_STREAM, 0, sentinel) < 0);
  CHECK_RESULT(sentinel[0], -7);
  CHECK_RESULT(sentinel[1], -9);
}

void test_socket_options() {
  const int64_t fd = lang_net_socket_v1(AF_INET, SOCK_STREAM, 0);
  CHECK_TRUE(fd >= 0);
  if (fd < 0) return;

  CHECK_RESULT(lang_net_setsockopt_int_v1(static_cast<int32_t>(fd), SOL_SOCKET,
                                          SO_REUSEADDR, 1),
               0);
  int32_t value = -1;
  CHECK_RESULT(lang_net_getsockopt_int_v1(static_cast<int32_t>(fd), SOL_SOCKET,
                                          SO_REUSEADDR, &value),
               0);
  CHECK_RESULT(value, 1);

  CHECK_RESULT(lang_net_setsockopt_int_v1(static_cast<int32_t>(fd),
                                          IPPROTO_TCP, TCP_NODELAY, 1),
               0);
  value = -1;
  CHECK_RESULT(lang_net_getsockopt_int_v1(static_cast<int32_t>(fd),
                                          IPPROTO_TCP, TCP_NODELAY, &value),
               0);
  CHECK_RESULT(value, 1);

  struct linger linger_value = {1, 3};
  CHECK_RESULT(lang_net_setsockopt_bytes_v1(static_cast<int32_t>(fd),
                                            SOL_SOCKET, SO_LINGER,
                                            &linger_value,
                                            sizeof linger_value),
               0);
  struct linger readback = {};
  uint64_t length = sizeof readback;
  CHECK_RESULT(lang_net_getsockopt_bytes_v1(static_cast<int32_t>(fd),
                                            SOL_SOCKET, SO_LINGER, &readback,
                                            &length),
               0);
  CHECK_RESULT(length, sizeof readback);
  CHECK_RESULT(readback.l_onoff, 1);
  CHECK_RESULT(readback.l_linger, 3);

  CHECK_RESULT(lang_net_setsockopt_bytes_v1(static_cast<int32_t>(fd), SOL_SOCKET,
                                            SO_LINGER, nullptr, 1),
               -EFAULT);
  CHECK_RESULT(lang_net_setsockopt_bytes_v1(static_cast<int32_t>(fd), SOL_SOCKET,
                                            SO_LINGER, &linger_value, UINT64_MAX),
               -EINVAL);
  length = sizeof readback;
  CHECK_RESULT(lang_net_getsockopt_bytes_v1(static_cast<int32_t>(fd), SOL_SOCKET,
                                            SO_LINGER, nullptr, &length),
               -EFAULT);
  length = sizeof readback;
  CHECK_RESULT(lang_net_getsockopt_bytes_v1(static_cast<int32_t>(fd), SOL_SOCKET,
                                            SO_LINGER, &readback, nullptr),
               -EFAULT);
  CHECK_RESULT(lang_net_getsockopt_int_v1(static_cast<int32_t>(fd), SOL_SOCKET,
                                          SO_REUSEADDR, nullptr),
               -EFAULT);
  CHECK_RESULT(lang_net_setsockopt_int_v1(-1, SOL_SOCKET, SO_REUSEADDR, 1),
               -EBADF);
  CHECK_RESULT(lang_net_getsockopt_int_v1(-1, SOL_SOCKET, SO_REUSEADDR, &value),
               -EBADF);

  close_fd(fd);
}

void test_poll() {
  int32_t pair[2] = {-1, -1};
  CHECK_RESULT(lang_net_socketpair_v1(AF_UNIX, SOCK_STREAM, 0, pair), 0);
  CHECK_TRUE(pair[0] >= 0 && pair[1] >= 0);
  if (pair[0] < 0 || pair[1] < 0) return;

  LangPollFd rows[2] = {{pair[0], POLLIN, 0}, {pair[1], POLLIN, 0}};
  CHECK_RESULT(lang_net_poll_v1(rows, 2, 0), 0);

  CHECK_RESULT(lang_net_send_v1(pair[1], "P", 1, 0), 1);
  CHECK_RESULT(lang_net_poll_v1(rows, 1, 100), 1);
  CHECK_TRUE((rows[0].revents & POLLIN) != 0);

  uint8_t byte = 0;
  CHECK_RESULT(lang_net_recv_v1(pair[0], &byte, 1, 0), 1);
  CHECK_RESULT(byte, 'P');

  CHECK_RESULT(lang_net_poll_v1(nullptr, 0, 0), 0);
  CHECK_RESULT(lang_net_poll_v1(nullptr, 1, 0), -EFAULT);

  const int closed_fd = open("/dev/null", O_RDONLY);
  CHECK_TRUE(closed_fd >= 0);
  if (closed_fd >= 0) {
    close(closed_fd);
    LangPollFd invalid = {closed_fd, POLLIN, 0};
    CHECK_RESULT(lang_net_poll_v1(&invalid, 1, 0), 1);
    CHECK_TRUE((invalid.revents & POLLNVAL) != 0);
  }

  close_fd(pair[0]);
  close_fd(pair[1]);
}

volatile sig_atomic_t sigpipe_count = 0;

void on_sigpipe(int) { sigpipe_count = static_cast<sig_atomic_t>(sigpipe_count + 1); }

void test_epipe_without_sigpipe() {
  uint8_t loopback[4] = {127, 0, 0, 1};
  TcpFixture fixture;
  CHECK_TRUE(make_tcp_fixture(AF_INET, loopback, 4, 0, &fixture));
  if (fixture.listener < 0) return;

  struct sigaction action = {};
  struct sigaction previous = {};
  action.sa_handler = on_sigpipe;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  CHECK_TRUE(sigaction(SIGPIPE, &action, &previous) == 0);

  close_fd(fixture.client);
  fixture.client = -1;

  struct timespec delay = {0, 20000000};
  nanosleep(&delay, nullptr);

  const uint8_t payload[] = "Q";
  int64_t result = 0;
  for (int attempt = 0; attempt < 20; ++attempt) {
    result = lang_net_send_v1(static_cast<int32_t>(fixture.accepted), payload, 1,
                              0);
    if (result < 0) break;
    nanosleep(&delay, nullptr);
  }
  CHECK_RESULT(result, -EPIPE);
  CHECK_RESULT(sigpipe_count, 0);

  sigaction(SIGPIPE, &previous, nullptr);
  close_tcp_fixture(&fixture);
}

bool probe_ipv6() {
  const int64_t fd = lang_net_socket_v1(AF_INET6, SOCK_STREAM, 0);
  if (fd < 0) {
    if (fd == -EAFNOSUPPORT || fd == -EPROTONOSUPPORT) return false;
    CHECK_TRUE(false);
    return false;
  }
  close_fd(fd);
  return true;
}

} // namespace

int main() {
  char directory_template[] = "/tmp/woozy-network-runtime-XXXXXX";
  char *directory = mkdtemp(directory_template);
  if (directory == nullptr) {
    std::fprintf(stderr, "FAIL: mkdtemp: %s\n", std::strerror(errno));
    return 1;
  }

  const bool ipv6_available = probe_ipv6();

  test_abi_layout();
  test_address_validation();
  test_inet_conversion();
  test_tcp_ipv4();
  test_tcp_ipv6(ipv6_available);
  test_udp_ipv4();
  test_udp_ipv6(ipv6_available);
  test_unix_pathname(directory);
  test_unix_abstract();
  test_unix_datagram(directory);
  test_socketpair();
  test_socket_options();
  test_poll();
  test_epipe_without_sigpipe();

  char leftover[512];
  if (make_path(leftover, sizeof leftover, directory, "unix-stream"))
    ::unlink(leftover);
  if (make_path(leftover, sizeof leftover, directory, "unix-dgram"))
    ::unlink(leftover);
  if (make_path(leftover, sizeof leftover, directory, "unix-dgram-sender"))
    ::unlink(leftover);
  if (rmdir(directory) != 0)
    std::fprintf(stderr, "note: could not remove %s: %s\n", directory,
                 std::strerror(errno));

  CHECK_RESULT(allocation_count, free_count);

  if (failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("network runtime checks passed\n");
  return 0;
}
