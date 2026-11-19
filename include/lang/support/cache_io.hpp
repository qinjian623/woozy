#pragma once
#include "lang/support/digest.hpp"
#include "lang/support/publish.hpp"
#include <charconv>
#include <fstream>
#include <limits>
#include <optional>
#include <system_error>

namespace lang::cache {
// Cache files are bounded input. Every record is versioned by its caller and
// covered by a digest before any arena indices are read.
inline constexpr size_t byte_limit = 64 * 1024 * 1024;
struct Writer {
  std::string bytes;
  void number(uint64_t value) { bytes += std::to_string(value) + ':'; }
  void string(std::string_view value) { number(value.size()); bytes += value; }
};
struct Reader {
  std::string_view bytes;
  size_t cursor = 0;
  uint64_t number() {
    auto end = bytes.find(':', cursor);
    if (end == bytes.npos || end - cursor > 20 || end == cursor)
      throw std::invalid_argument("invalid cache number");
    uint64_t value;
    const auto parsed = std::from_chars(bytes.data() + cursor, bytes.data() + end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != bytes.data() + end)
      throw std::invalid_argument("invalid cache number");
    cursor = end + 1;
    return value;
  }
  size_t count() {
    const auto value = number();
    if (value > bytes.size() - cursor || value > 2000000)
      throw std::invalid_argument("cache count exceeds input");
    return static_cast<size_t>(value);
  }
  bool flag() {
    auto value = number();
    if (value > 1) throw std::invalid_argument("invalid cache flag");
    return value;
  }
  std::string string() {
    auto length = count();
    std::string value(bytes.substr(cursor, length)); cursor += length;
    return value;
  }
  void done() const {
    if (cursor != bytes.size()) throw std::invalid_argument("trailing cache bytes");
  }
};
inline std::optional<std::string> load(const std::filesystem::path &path) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path, error)) || error)
    return {};
  auto size = std::filesystem::file_size(path, error);
  if (error || size < 65 || size > byte_limit) return {};
  std::ifstream input(path, std::ios::binary);
  std::string bytes(size, '\0');
  if (!input.read(bytes.data(), static_cast<std::streamsize>(size))) return {};
  if (bytes[64] != '\n' || bytes.substr(0, 64) != sha256(std::string_view(bytes).substr(65))) return {};
  return bytes.substr(65);
}
inline void store(const std::filesystem::path &directory, const std::string &name,
                  std::string_view bytes) {
  if (bytes.size() > byte_limit) return;
  // Cache publication is optional. Only I/O failures are ignored here;
  // callers' source checks, verification and user-output publication stay fatal.
  try {
    StagingDir stage(directory, ".woozyc-cache-XXXXXX");
    std::ofstream output(stage.path() / name, std::ios::binary);
    output << sha256(bytes) << '\n'; output.write(bytes.data(), bytes.size()); output.close();
    if (!output) throw std::ios_base::failure("cannot write compiler cache");
    publish_staged_files(stage, directory, {name});
  } catch (const std::system_error &) {}

}
}
