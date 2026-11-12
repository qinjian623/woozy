#include "tls_source.hpp"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

int main() {
  auto *error = wts_error_new();
  auto *count = wts_count_new();
  assert(error && count);
  for (uint64_t id : {uint64_t{0}, uint64_t{1}, ~uint64_t{0}}) {
    assert(wts_transport_fd(id) == -1);
    wts_count_set(count, 91);
    assert(wts_transport_read(id, nullptr, 0, count, error) == LANG_TLS_STATUS_ERROR);
    assert(wts_count_value(count) == 0);
    assert(wts_error_category(error) == LANG_TLS_ERROR_INVALID_ARGUMENT);
    assert(wts_transport_write(id, nullptr, 0, count, error) == LANG_TLS_STATUS_ERROR);
  }

  int sockets[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
  auto *context = wts_context_client_default(error);
  assert(context);
  auto *name = wts_bytes_new(9);
  assert(name && !wts_bytes_failed(name));
  std::memcpy(wts_bytes_data_mut(name), "localhost", 9);
  wts_bytes_set_length(name, 9);
  const auto make_stream = [&] {
    auto *stream = wts_stream_client(context, sockets[0], LANG_TLS_NAME_DNS,
                                     name, error);
    assert(stream);
    return stream;
  };

  uint64_t previous = 0;
  for (int i = 0; i < 256; ++i) {
    auto *stream = make_stream();
    const auto id = wts_stream_token(stream);
    assert(id > previous);
    wts_stream_retain(stream);
    wts_stream_release(stream);
    assert(wts_transport_fd(id) == sockets[0]);
    wts_stream_release(stream);
    assert(wts_transport_fd(id) == -1);
    assert(wts_transport_read(id, nullptr, 0, count, error) == LANG_TLS_STATUS_ERROR);
    previous = id;
  }

  auto *stream = make_stream();
  auto id = wts_stream_token(stream);
  std::atomic<bool> stop{false};
  std::atomic<unsigned> lookups{0};
  std::vector<std::thread> readers;
  for (int i = 0; i < 4; ++i)
    readers.emplace_back([&] {
      while (!stop.load()) {
        assert(wts_transport_fd(id) == sockets[0]);
        ++lookups;
      }
    });
  while (lookups.load() < 256) std::this_thread::yield();
  // Fork must inherit an unlocked token table while other readers are active.
  const auto child = fork();
  assert(child >= 0);
  if (child == 0) _exit(wts_transport_fd(id) == sockets[0] ? 0 : 1);
  int status = 0;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  stop.store(true);
  for (auto &thread : readers) thread.join();
  wts_stream_release(stream);
  assert(wts_transport_fd(id) == -1);

  stream = make_stream();
  id = wts_stream_token(stream);
  std::atomic<unsigned> ready{0};
  lookups.store(0);
  readers.clear();
  for (int i = 0; i < 4; ++i)
    readers.emplace_back([&] {
      ++ready;
      while (ready.load() != 4) std::this_thread::yield();
      for (int j = 0; j < 4096; ++j) {
        const auto fd = wts_transport_fd(id);
        assert(fd == sockets[0] || fd == -1);
        ++lookups;
      }
    });
  while (lookups.load() < 128) std::this_thread::yield();
  wts_stream_release(stream);
  for (auto &thread : readers) thread.join();
  assert(wts_transport_fd(id) == -1);
  assert(wts_transport_read(id, nullptr, 0, count, error) == LANG_TLS_STATUS_ERROR);
  assert(wts_transport_write(id, nullptr, 0, count, error) == LANG_TLS_STATUS_ERROR);

  wts_bytes_release(name);
  wts_context_release(context);
  wts_count_release(count);
  wts_error_release(error);
  close(sockets[0]);
  close(sockets[1]);
  std::puts("TLS_TOKEN_CHECKS_OK invalid expired nonreuse concurrent fork");
}
