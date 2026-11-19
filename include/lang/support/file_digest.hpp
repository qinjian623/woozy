#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
namespace lang {
// Content SHA memo for stable regular files. Metadata never replaces the SHA
// in compiler identities; changed/unstable files are read again or rejected.
struct FileDigests {
  std::filesystem::path directory;
  uint64_t hits = 0, misses = 0, bytes_read = 0;
  std::string get(const std::filesystem::path &);
};
}
