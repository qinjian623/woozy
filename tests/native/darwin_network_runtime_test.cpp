#include "../../runtime/native/dns.hpp"
#include "../../runtime/native/network.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dns_sd.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" int64_t lang_net_send_vec_v1(int32_t fd, LangVec *data,
    uint64_t offset, uint64_t count, int32_t flags) noexcept;

namespace {

constexpr int32_t kUnix = 1;
constexpr int32_t kInet = 2;
constexpr int32_t kInet6 = 10;
constexpr int32_t kStream = 1;
constexpr int32_t kDatagram = 2;
constexpr int32_t kNonblock = 2048;
constexpr int32_t kCloexec = 524288;
constexpr int16_t kRead = 1;
constexpr int32_t kPeek = 2;
constexpr int32_t kTrunc = 32;
constexpr int32_t kDontWait = 64;
constexpr int32_t kWaitAll = 256;

LangVec *make_bytes(const char *data, size_t count) {
  LangVec *bytes = lang_vec_new_v1(count, 1, nullptr, nullptr);
  for (size_t i = 0; i < count; ++i) lang_vec_init_v1(bytes, data + i);
  return bytes;
}

void check_descriptor(int fd, bool nonblocking) {
  const int descriptor = fcntl(fd, F_GETFD);
  const int mode = fcntl(fd, F_GETFL);
  assert(descriptor >= 0 && (descriptor & FD_CLOEXEC));
  assert(mode >= 0 && ((mode & O_NONBLOCK) != 0) == nonblocking);
}

void check_name(LangNetAddress endpoint, const std::string &expected) {
  assert(endpoint.family == kUnix && endpoint.port == 0 && endpoint.flags == 0);
  assert(endpoint.data->length == expected.size());
  assert(expected.empty() ||
         std::memcmp(endpoint.data->data, expected.data(), expected.size()) == 0);
  lang_vec_release_v1(endpoint.data);
}

void unix_stream() {
  char directory[] = "/tmp/woozy-darwin-socket.XXXXXXXX";
  assert(mkdtemp(directory));
  std::string path(directory);
  path += '/';
  path.append(103 - path.size(), 's');
  LangNetAddress address{kUnix, 0, 0, 0, 0, make_bytes(path.data(), path.size())};
  const auto listener = static_cast<int32_t>(lang_net_socket_v1(kUnix, kStream | kNonblock | kCloexec, 0));
  assert(listener >= 0);
  check_descriptor(listener, true);

  const std::string too_long = path + "x";
  LangNetAddress oversized{kUnix, 0, 0, 0, 0, make_bytes(too_long.data(), too_long.size())};
  assert(lang_net_bind_v1(listener, &oversized) == -36);
  lang_vec_release_v1(oversized.data);
  LangNetAddress abstract = address;
  abstract.flags = 1;
  assert(lang_net_bind_v1(listener, &abstract) == -95);
  assert(lang_net_bind_v1(listener, &address) == 0);
  assert(lang_net_listen_v1(listener, 2) == 0);

  LangNetAddress endpoint{};
  assert(lang_net_accept_v1(listener, &endpoint, kCloexec) == -11 && !endpoint.data);
  assert(lang_net_getsockname_v1(listener, &endpoint) == 0);
  check_name(endpoint, path);

  const auto client = static_cast<int32_t>(lang_net_socket_v1(kUnix, kStream | kCloexec, 0));
  assert(client >= 0);
  check_descriptor(client, false);
  assert(lang_net_connect_v1(client, &address) == 0);
  assert(lang_net_getpeername_v1(client, &endpoint) == 0);
  check_name(endpoint, path);
  LangPollFd ready{listener, kRead, 0};
  assert(lang_net_poll_v1(&ready, 1, 1000) == 1 && (ready.revents & kRead));
  const auto peer = static_cast<int32_t>(lang_net_accept_v1(listener, &endpoint, kCloexec));
  assert(peer >= 0);
  check_name(endpoint, "");
  check_descriptor(peer, false);

  char request[4]{};
  assert(lang_net_recv_v1(peer, request, sizeof(request), kDontWait) == -11);
  LangVec *payload = make_bytes("xWZ!!", 5);
  assert(lang_net_send_vec_v1(client, payload, 1, 4, 0) == 4);
  lang_vec_release_v1(payload);
  ready = {peer, kRead, 0};
  assert(lang_net_poll_v1(&ready, 1, 1000) == 1 && (ready.revents & kRead));
  assert(lang_net_recv_v1(peer, request, 1, kPeek) == 1 && request[0] == 'W');
  assert(lang_net_recv_v1(peer, request, sizeof(request), kWaitAll) == 4);
  assert(std::memcmp(request, "WZ!!", sizeof(request)) == 0);
  assert(lang_net_shutdown_v1(client, 1) == 0);
  assert(lang_net_recv_v1(peer, request, sizeof(request), 0) == 0);

  assert(close(peer) == 0 && close(client) == 0 && close(listener) == 0);
  assert(unlink(path.c_str()) == 0 && rmdir(directory) == 0);
  lang_vec_release_v1(address.data);
}

void socket_pairs() {
  int32_t pair[2]{-1, -1};
  assert(lang_net_socketpair_v1(2, kStream, 0, pair) == -97);
  assert(pair[0] == -1 && pair[1] == -1);
  assert(lang_net_socketpair_v1(kUnix, kStream | kCloexec, 0, pair) == 0);
  check_descriptor(pair[0], false);
  assert(lang_net_accept_v1(pair[0], nullptr, kCloexec) == -95);
  assert(close(pair[1]) == 0);
  assert(signal(SIGPIPE, SIG_DFL) != SIG_ERR);
  const char value = 'z';
  assert(lang_net_send_v1(pair[0], &value, 1, 0) == -32);
  assert(close(pair[0]) == 0);

  assert(lang_net_socketpair_v1(kUnix, kDatagram | kNonblock | kCloexec, 0, pair) == 0);
  check_descriptor(pair[0], true);
  char data{};
  assert(lang_net_send_v1(pair[0], nullptr, 0, 0) == 0);
  LangPollFd row{pair[1], kRead, 0};
  assert(lang_net_poll_v1(&row, 1, 1000) == 1 && (row.revents & kRead));
  assert(lang_net_recv_v1(pair[1], &data, 1, 0) == 0);
  assert(lang_net_recv_v1(pair[1], &data, 1, 0) == -11);
  assert(lang_net_send_v1(pair[0], "woo", 3, 0) == 3);
  assert(lang_net_recv_v1(pair[1], nullptr, 0, kTrunc) == 3);
  assert(lang_net_recv_v1(pair[1], &data, 1, 0) == -11);
  assert(close(pair[0]) == 0 && close(pair[1]) == 0);
}

LangNetAddress loopback(int32_t family) {
  const char *text = family == kInet ? "127.0.0.1" : "::1";
  LangNetAddress result{};
  assert(lang_net_inet_pton_v1(family, text, std::strlen(text), &result) == 0);
  assert(result.family == family && result.port == 0 && result.data);
  return result;
}

void check_ip(LangNetAddress value, int32_t family, uint16_t port) {
  assert(value.family == family && value.port == port && value.flags == 0);
  char text[64];
  assert(lang_net_inet_ntop_v1(&value, text, sizeof(text)) > 0);
  assert(std::strcmp(text, family == kInet ? "127.0.0.1" : "::1") == 0);
  lang_vec_release_v1(value.data);
}

void ip_text() {
  LangNetAddress value{};
  assert(lang_net_inet_pton_v1(kInet, "127.000.0.1", 11, &value) == -22 &&
         !value.data);
  assert(lang_net_inet_pton_v1(kInet6, "::192.0.2.1", 11, &value) == 0);
  char text[64] = "untouched";
  assert(lang_net_inet_ntop_v1(&value, text, 5) == -28);
  assert(std::strcmp(text, "untouched") == 0);
  assert(lang_net_inet_ntop_v1(&value, text, sizeof(text)) == 10);
  assert(std::strcmp(text, "::c000:201") == 0);
  lang_vec_release_v1(value.data);
}

void ip_stream(int32_t family) {
  const auto server = static_cast<int32_t>(
      lang_net_socket_v1(family, kStream | kNonblock | kCloexec, 0));
  assert(server >= 0);
  assert(lang_net_setsockopt_int_v1(server, 1, 2, 1) == 0);
  if (family == kInet6) {
    assert(lang_net_setsockopt_int_v1(server, 41, 26, 1) == 0);
    int32_t enabled = -1;
    assert(lang_net_getsockopt_int_v1(server, 41, 26, &enabled) == 0 && enabled == 1);
  }
  auto address = loopback(family);
  assert(lang_net_bind_v1(server, &address) == 0);
  lang_vec_release_v1(address.data);
  assert(lang_net_getsockname_v1(server, &address) == 0 && address.port != 0);
  assert(lang_net_listen_v1(server, 2) == 0);

  const auto client = static_cast<int32_t>(lang_net_socket_v1(family, kStream, 0));
  assert(client >= 0 && lang_net_connect_v1(client, &address) == 0);
  assert(lang_net_setsockopt_int_v1(client, 6, 1, 1) == 0);
  int32_t option = -1;
  assert(lang_net_getsockopt_int_v1(client, 6, 1, &option) == 0 && option == 1);
  assert(lang_net_getsockopt_int_v1(client, 1, -1, &option) == -92 && option == 1);
  const int64_t timeout[]{1, 0};
  assert(lang_net_setsockopt_bytes_v1(client, 1, 20, timeout, sizeof(timeout)) == 0);
  int64_t readback[]{-1, -1};
  uint64_t length = sizeof(readback);
  assert(lang_net_getsockopt_bytes_v1(client, 1, 20, readback, &length) == 0);
  assert(length == sizeof(readback) && readback[0] == timeout[0] && readback[1] == timeout[1]);
  const int32_t linger[]{1, 3};
  assert(lang_net_setsockopt_bytes_v1(client, 1, 13, linger, sizeof(linger)) == 0);
  int32_t linger_out[]{-1, -1};
  length = sizeof(linger_out);
  assert(lang_net_getsockopt_bytes_v1(client, 1, 13, linger_out, &length) == 0);
  assert(length == sizeof(linger_out) && linger_out[0] == linger[0] && linger_out[1] == linger[1]);

  LangNetAddress client_source{};
  assert(lang_net_getsockname_v1(client, &client_source) == 0);
  const uint16_t source_port = client_source.port;
  check_ip(client_source, family, source_port);
  LangNetAddress peer_source{};
  const auto peer = static_cast<int32_t>(lang_net_accept_v1(server, &peer_source, kCloexec));
  assert(peer >= 0);
  check_descriptor(peer, false);
  check_ip(peer_source, family, source_port);
  LangNetAddress destination{};
  assert(lang_net_getpeername_v1(client, &destination) == 0);
  check_ip(destination, family, address.port);

  assert(lang_net_send_v1(client, "hello", 5, 0) == 5);
  char read[3] = {'!', '!', '!'};
  assert(lang_net_recv_v1(peer, read, 2, kTrunc | kWaitAll) == 2 &&
         std::memcmp(read, "!!!", sizeof(read)) == 0);
  assert(lang_net_recv_v1(peer, read, sizeof(read), kWaitAll) == 3 &&
         std::memcmp(read, "llo", sizeof(read)) == 0);

  // An abortive close queues ECONNRESET in the peer's SO_ERROR.
  const int32_t abortive[]{1, 0};
  assert(lang_net_setsockopt_bytes_v1(peer, 1, 13, abortive, sizeof(abortive)) == 0);
  assert(close(peer) == 0);
  LangPollFd ready{client, kRead, 0};
  assert(lang_net_poll_v1(&ready, 1, 1000) == 1);
  assert(lang_net_getsockopt_int_v1(client, 1, 4, &option) == 0 && option == 104);
  assert(lang_net_getsockopt_int_v1(client, 1, 4, &option) == 0 && option == 0);
  assert(close(client) == 0 && close(server) == 0);
  ready = {server, kRead, 0};
  assert(lang_net_poll_v1(&ready, 1, 0) == 1 && (ready.revents & 32));
  option = 7;
  assert(lang_net_getsockopt_int_v1(server, 1, 4, &option) == -9 && option == 7);
  lang_vec_release_v1(address.data);
}

void ip_datagram(int32_t family) {
  const auto receiver = static_cast<int32_t>(lang_net_socket_v1(family, kDatagram | kNonblock, 0));
  const auto sender = static_cast<int32_t>(lang_net_socket_v1(family, kDatagram, 0));
  auto address = loopback(family);
  assert(receiver >= 0 && sender >= 0);
  assert(lang_net_bind_v1(receiver, &address) == 0 &&
         lang_net_bind_v1(sender, &address) == 0);
  lang_vec_release_v1(address.data);
  assert(lang_net_getsockname_v1(receiver, &address) == 0);
  LangNetAddress sender_address{};
  assert(lang_net_getsockname_v1(sender, &sender_address) == 0);
  assert(lang_net_sendto_v1(sender, "hello", 5, 0, &address) == 5);
  // Loopback delivery may finish after sendto returns.
  LangPollFd ready{receiver, kRead, 0};
  assert(lang_net_poll_v1(&ready, 1, 1000) == 1 && (ready.revents & kRead));
  LangNetAddress from{};
  char data[2]{};
  assert(lang_net_recvfrom_v1(receiver, data, sizeof(data), kPeek | kTrunc, &from) == 5);
  assert(std::memcmp(data, "he", sizeof(data)) == 0);
  check_ip(from, family, sender_address.port);
  assert(lang_net_recvfrom_v1(receiver, data, sizeof(data), kTrunc, &from) == 5);
  check_ip(from, family, sender_address.port);
  assert(lang_net_recv_v1(receiver, data, sizeof(data), 0) == -11);
  assert(close(receiver) == 0 && close(sender) == 0);
  lang_vec_release_v1(address.data);
  lang_vec_release_v1(sender_address.data);
}

LangText make_text(const char *value, size_t size) {
  return {reinterpret_cast<const uint8_t *>(value), size, size, nullptr};
}

void dns_lookup() {
  const auto local = make_text("localhost", 9);
  LangVec *result = nullptr;
  assert(lang_net_lookup_v1(&local, 443, 0, &result) == 0 && result->length == 2);
  auto *data = static_cast<LangNetAddress *>(result->data);
  assert(data[0].family == kInet && data[1].family == kInet6);
  lang_vec_retain_v1(data[0].data);
  lang_vec_retain_v1(data[1].data);
  check_ip(data[0], kInet, 443);
  check_ip(data[1], kInet6, 443);
  lang_vec_release_v1(result);

  const auto literal = make_text("::1", 3);
  assert(lang_net_lookup_v1(&literal, 123, kInet6, &result) == 0 && result->length == 1);
  auto *item = static_cast<LangNetAddress *>(result->data);
  lang_vec_retain_v1(item->data);
  check_ip(*item, kInet6, 123);
  lang_vec_release_v1(result);
  const auto embedded = make_text("bad\0host", 8);
  result = nullptr;
  assert(lang_net_lookup_v1(&embedded, 0, 0, &result) == -22 && !result);
}

void record_registered(DNSServiceRef, DNSRecordRef, DNSServiceFlags,
                       DNSServiceErrorType error, void *context) {
  *static_cast<DNSServiceErrorType *>(context) = error;
}

void dns_system_lookup() {
  // This record only exists in the local system resolver, with no DNS settings changed.
  char name[80];
  const int length = std::snprintf(name, sizeof(name), "woozy-test-resolver-%d.local.", getpid());
  assert(length > 0 && static_cast<size_t>(length) < sizeof(name));
  constexpr uint8_t expected[]{192, 0, 2, 47};
  DNSServiceRef connection{};
  DNSRecordRef record{};
  DNSServiceErrorType registration = kDNSServiceErr_Unknown;
  assert(DNSServiceCreateConnection(&connection) == kDNSServiceErr_NoError);
  assert(DNSServiceRegisterRecord(connection, &record, kDNSServiceFlagsUnique,
      kDNSServiceInterfaceIndexLocalOnly, name, kDNSServiceType_A, kDNSServiceClass_IN,
      sizeof(expected), expected, 15, record_registered, &registration) == kDNSServiceErr_NoError);
  pollfd ready{DNSServiceRefSockFD(connection), POLLIN, 0};
  assert(poll(&ready, 1, 3000) == 1);
  assert(DNSServiceProcessResult(connection) == kDNSServiceErr_NoError);
  assert(registration == kDNSServiceErr_NoError);

  const auto host = make_text(name, static_cast<size_t>(length));
  LangVec *result = nullptr;
  assert(lang_net_lookup_v1(&host, 8443, kInet, &result) == 0);
  assert(result && result->length == 1);
  const auto &address = *static_cast<const LangNetAddress *>(result->data);
  assert(address.family == kInet && address.port == 8443 && address.scope == 0);
  assert(address.data->length == sizeof(expected));
  assert(std::memcmp(address.data->data, expected, sizeof(expected)) == 0);
  lang_vec_release_v1(result);
  DNSServiceRefDeallocate(connection);
}

void serve_dns_once(int server) {
  pollfd ready{server, POLLIN, 0};
  if (poll(&ready, 1, 2000) != 1) _exit(2);
  uint8_t wire[512];
  sockaddr_in from{};
  socklen_t length = sizeof(from);
  const auto received = recvfrom(server, wire, sizeof(wire), 0,
                                 reinterpret_cast<sockaddr *>(&from), &length);
  if (received < 17) _exit(3);

  // One A record for the question at offset 12.
  constexpr uint8_t answer[]{0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 0, 2, 42};
  if (static_cast<size_t>(received) > sizeof(wire) - sizeof(answer)) _exit(4);
  wire[2] = 0x81;
  wire[3] = 0x80;
  wire[7] = 1;
  std::memcpy(wire + received, answer, sizeof(answer));
  const auto total = received + static_cast<ssize_t>(sizeof(answer));
  const auto sent = sendto(server, wire, total, 0, reinterpret_cast<sockaddr *>(&from), length);
  _exit(sent == total ? 0 : 5);
}

void dns_offline() {
  const int server = socket(AF_INET, SOCK_DGRAM, 0);
  assert(server >= 0);
  sockaddr_in host{};
  host.sin_len = sizeof(host);
  host.sin_family = AF_INET;
  host.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(server, reinterpret_cast<sockaddr *>(&host), sizeof(host)) == 0);
  socklen_t length = sizeof(host);
  assert(getsockname(server, reinterpret_cast<sockaddr *>(&host), &length) == 0);
  const pid_t child = fork();
  assert(child >= 0);
  if (child == 0) serve_dns_once(server);
  assert(close(server) == 0);

  auto address = loopback(kInet);
  address.port = ntohs(host.sin_port);
  const auto question = make_text("my-test.example", 15);
  LangVec *result = nullptr;
  const auto query = lang_net_dns_query_v1(&question, kInet, &address, 2000, &result);
  lang_vec_release_v1(address.data);
  int status = 0;
  assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  assert(query == 0 && result->length == 1 && result->copy && result->drop);
  const auto &item = *static_cast<const LangNetAddress *>(result->data);
  const uint8_t expected[]{192, 0, 2, 42};
  assert(item.family == kInet && item.port == 0 && item.data->length == sizeof(expected));
  assert(std::memcmp(item.data->data, expected, sizeof(expected)) == 0);
  lang_vec_release_v1(result);
}

} // namespace

int main() {
  assert(lang_net_poll_v1(nullptr, 0, 0) == 0);
  assert(lang_net_poll_v1(nullptr, 1, 0) == -14);
  unix_stream();
  socket_pairs();
  ip_text();
  for (const auto family : {kInet, kInet6}) {
    ip_stream(family);
    ip_datagram(family);
  }
  dns_lookup();
  dns_system_lookup();
  dns_offline();
  std::puts("Darwin network runtime passed");
}
