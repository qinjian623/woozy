#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

// lang-native-scalars/1. This is a terminal Panic, not a typed language error.
// The record is also the runner's evidence; exit 101 alone is never sufficient.
extern "C" [[noreturn]] void lang_native_panic_v1(uint32_t cause,
                                                  uint64_t snapshot,
                                                  uint64_t begin,
                                                  uint64_t end) noexcept {
  char record[192];
  const char *name = cause == 1   ? "division-by-zero"
                     : cause == 2 ? "invalid-shift"
                     : cause == 3 ? "integer-conversion"
                                  : "invalid-cause";
  int count = std::snprintf(record, sizeof(record),
                            "LANG_NATIVE_PANIC:v1:%s:%llu:%llu:%llu\n", name,
                            static_cast<unsigned long long>(snapshot),
                            static_cast<unsigned long long>(begin),
                            static_cast<unsigned long long>(end));
  if (count > 0 && static_cast<size_t>(count) < sizeof(record)) {
    size_t offset = 0;
    while (offset < static_cast<size_t>(count)) {
      auto n = ::write(STDERR_FILENO, record + offset,
                       static_cast<size_t>(count) - offset);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        break;
      offset += static_cast<size_t>(n);
    }
  }
  std::_Exit(101);
}

// Separate ordinary typed-error entry diagnostic; no panic or unwinding.
extern "C" void lang_native_typed_error_v1(const char *key,
                                           uint64_t size) noexcept {
  auto write_all = [](const char *bytes, size_t count) {
    while (count) {
      auto n = ::write(STDERR_FILENO, bytes, count);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        break;
      bytes += n;
      count -= static_cast<size_t>(n);
    }
  };
  constexpr char prefix[] = "LANG_TYPED_ERROR: ";
  write_all(prefix, sizeof(prefix) - 1);
  write_all(key, static_cast<size_t>(size));
  write_all("\n", 1);
}
