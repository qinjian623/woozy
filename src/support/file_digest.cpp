#include "lang/support/file_digest.hpp"
#include "lang/support/cache_io.hpp"
#include <array>
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace lang {
namespace {
struct File { int fd; ~File() { if (fd >= 0) ::close(fd); } };
struct stat info(int fd) {
  struct stat value{};
  if (::fstat(fd, &value)) throw std::system_error(errno, std::generic_category(), "fstat fingerprint input");
  return value;
}
std::string metadata(const std::filesystem::path &canonical, const struct stat &value) {
  cache::Writer out; out.string("wz-file-metadata/1"); out.string(canonical.string());
  out.number(value.st_dev); out.number(value.st_ino); out.number(value.st_nlink);
  out.number(value.st_mode); out.number(value.st_uid); out.number(value.st_gid); out.number(value.st_rdev);
  out.number(value.st_size); out.string(std::to_string(value.st_blksize)); out.string(std::to_string(value.st_blocks));
  out.string(std::to_string(value.st_mtim.tv_sec)); out.number(value.st_mtim.tv_nsec);
  out.string(std::to_string(value.st_ctim.tv_sec)); out.number(value.st_ctim.tv_nsec);
  return out.bytes;
}
bool current(const std::filesystem::path &path, const std::filesystem::path &canonical,
             const std::string &expected, int fd) {
  std::error_code error;
  if (std::filesystem::canonical(path, error) != canonical || error) return false;
  struct stat at_path{};
  if (::stat(path.c_str(), &at_path)) return false;
  return metadata(canonical, at_path) == expected && metadata(canonical, info(fd)) == expected;
}
bool digest_shape(const std::string &value) {
  if (value.size() != 64) return false;
  for (char c : value) if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f')) return false;
  return true;
}
ssize_t read_retry(int fd, void *bytes, size_t count) {
  for (;;) {
    auto result = ::read(fd, bytes, count);
    if (result >= 0) return result;
    if (errno != EINTR) throw std::system_error(errno, std::generic_category(), "read fingerprint input");
  }
}
}
std::string FileDigests::get(const std::filesystem::path &path) {
  for (unsigned attempt = 0; attempt != 3; ++attempt) {
    const auto canonical = std::filesystem::canonical(path);
    File file{::open(path.c_str(), O_RDONLY | O_CLOEXEC)};
    if (file.fd < 0) throw std::system_error(errno, std::generic_category(), "open fingerprint input");
    const auto before = info(file.fd);
    if (!S_ISREG(before.st_mode) || before.st_size < 0)
      throw std::system_error(EINVAL, std::generic_category(), "fingerprint input is not a regular file");
    const auto expected = metadata(canonical, before);
    if (!current(path, canonical, expected, file.fd)) continue;
    const auto key = sha256(expected) + ".sha";
    if (!directory.empty()) {
      if (auto bytes = cache::load(directory / key)) {
        try {
          cache::Reader record{*bytes};
          if (record.string() == "wz-file-digest/1" && record.string() == key) {
            const auto digest = record.string(); record.done();
            if (digest_shape(digest) && current(path, canonical, expected, file.fd)) { ++hits; return digest; }
          }
        } catch (const std::invalid_argument &) {}
      }
    }
    ++misses;
    std::string bytes;
    std::array<char, 65536> buffer;
    auto remaining = static_cast<uint64_t>(before.st_size);
    while (remaining) {
      const auto count = read_retry(file.fd, buffer.data(), std::min<uint64_t>(buffer.size(), remaining));
      if (!count) break;
      bytes.append(buffer.data(), count); bytes_read += count; remaining -= count;
    }
    char extra;
    const auto additional = read_retry(file.fd, &extra, 1); bytes_read += additional;
    if (remaining || additional || !current(path, canonical, expected, file.fd)) continue;
    const auto digest = sha256(bytes);
    // Check after hashing too: hashing can outlast a concurrent replacement.
    if (!current(path, canonical, expected, file.fd)) continue;
    if (!directory.empty()) {
      cache::Writer record; record.string("wz-file-digest/1"); record.string(key); record.string(digest);
      cache::store(directory, key, record.bytes);
    }
    return digest;
  }
  throw std::system_error(EAGAIN, std::generic_category(), "fingerprint input changed while reading " + path.string());
}
}
