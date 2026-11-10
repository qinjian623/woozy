// Test-only auxiliary observer for the large sieve preparation.
//
// Source: overrides the runtime's weak lang_vec_observe_v1 hook.  At the final
// release of a Vec whose length is SIEVE_OBSERVE_N+1 and stride is 1, it writes
// the complete element bytes to SIEVE_OBSERVER_OUT as a sequence of
// 8-byte-little-endian-length frames.  It never recomputes the sieve.
//
// C++: the preparation script compiles a separate auxiliary copy of the formal
// reference with exactly one call to sieve_observe_cpp(composite.data(),
// composite.size()) before the count check.  This function writes the same
// frame format.  The formal C++ source and normal program are unchanged.
//
// This file depends on the host libc, uses raw write() calls, and is not a
// formal CLI or performance program.  The Source hook filters the composite
// Vec by the frozen length/stride, so the small bounds Vec is not observed.
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

#include "vec.hpp"

#ifndef SIEVE_OBSERVE_N
#error "SIEVE_OBSERVE_N must be defined"
#endif

namespace {
constexpr uint64_t kExpectedLength = static_cast<uint64_t>(SIEVE_OBSERVE_N) + 1;
int output_fd = -1;

void write_all(int fd, const void *buffer, std::size_t count) noexcept {
  const auto *bytes = static_cast<const unsigned char *>(buffer);
  while (count) {
    const ssize_t written = ::write(fd, bytes, count);
    if (written < 0) {
      if (errno == EINTR) continue;
      std::_Exit(98);
    }
    if (written == 0) std::_Exit(98);
    bytes += static_cast<std::size_t>(written);
    count -= static_cast<std::size_t>(written);
  }
}

void open_output() noexcept {
  if (output_fd >= 0) return;
  const char *path = std::getenv("SIEVE_OBSERVER_OUT");
  if (path == nullptr || *path == '\0') {
    std::fputs("large_observer: SIEVE_OBSERVER_OUT is not set\n", stderr);
    std::_Exit(97);
  }
  output_fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (output_fd < 0) {
    std::fputs("large_observer: cannot open output\n", stderr);
    std::_Exit(97);
  }
}

void dump(const unsigned char *data, uint64_t length) noexcept {
  if (length != kExpectedLength || data == nullptr) return;
  open_output();
  const uint64_t encoded = length;
  write_all(output_fd, &encoded, sizeof encoded);
  write_all(output_fd, data, static_cast<std::size_t>(length));
}
}

extern "C" void lang_vec_observe_v1(uint32_t event, const void *value,
                                     uint64_t detail) noexcept {
  if (event != 3 || detail != 0 || value == nullptr) return;
  const auto *vec = static_cast<const LangVec *>(value);
  if (vec->length != kExpectedLength || vec->stride != 1
      || vec->initialized != kExpectedLength || vec->data == nullptr) {
    return;
  }
  dump(static_cast<const unsigned char *>(vec->data), vec->length);
}

extern "C" void sieve_observe_cpp(const uint8_t *data,
                                  std::size_t length) noexcept {
  dump(data, static_cast<uint64_t>(length));
}
