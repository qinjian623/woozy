#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace lang {

/// RAII staging directory created inside the destination directory so renames
/// stay on the same filesystem. Removed on destruction unless `preserve()` is
/// called after a partial rollback failure.
class StagingDir {
  std::filesystem::path path_;
  bool preserve_ = false;

public:
  explicit StagingDir(const std::filesystem::path &destination,
                      std::string_view prefix = ".woozyc-XXXXXX");
  ~StagingDir();

  StagingDir(const StagingDir &) = delete;
  StagingDir &operator=(const StagingDir &) = delete;
  StagingDir(StagingDir &&) = delete;
  StagingDir &operator=(StagingDir &&) = delete;

  const std::filesystem::path &path() const { return path_; }
  void preserve() { preserve_ = true; }
};

/// Validates existing destination paths are regular files and moves `names`
/// from `stage.path()` to `destination` with transactional backup/rollback.
/// Publishers to the same directory serialize the entire transaction.
void publish_staged_files(StagingDir &stage,
                          const std::filesystem::path &destination,
                          const std::vector<std::string> &names);

} // namespace lang
