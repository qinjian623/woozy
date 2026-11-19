#include "lang/support/publish.hpp"

#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <system_error>
#include <unistd.h>

namespace lang {
namespace {
namespace fs = std::filesystem;
class PublicationLock {
  int descriptor_;
public:
  explicit PublicationLock(const fs::path &directory) {
    descriptor_ = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor_ < 0)
      throw fs::filesystem_error("open publication directory", directory,
                                 std::error_code(errno, std::generic_category()));
    while (::flock(descriptor_, LOCK_EX) != 0) {
      if (errno == EINTR) continue;
      const auto error = std::error_code(errno, std::generic_category());
      ::close(descriptor_);
      throw fs::filesystem_error("lock publication directory", directory, error);
    }
  }
  ~PublicationLock() { ::close(descriptor_); }
  PublicationLock(const PublicationLock &) = delete;
  PublicationLock &operator=(const PublicationLock &) = delete;
};
} // namespace

StagingDir::StagingDir(const fs::path &destination, std::string_view prefix) {
  fs::create_directories(destination);
  std::string pattern = (destination / prefix).string();
  if (!::mkdtemp(pattern.data())) {
    throw fs::filesystem_error("create build staging directory", destination,
                               std::error_code(errno, std::generic_category()));
  }
  path_ = std::move(pattern);
}

StagingDir::~StagingDir() {
  if (!preserve_ && !path_.empty()) {
    std::error_code ignored;
    fs::remove_all(path_, ignored);
  }
}

void publish_staged_files(StagingDir &stage, const fs::path &destination,
                          const std::vector<std::string> &names) {
  // Lock the actual directory inode, including symlink aliases, for the
  // complete backup/publish/rollback transaction. No lock file survives.
  PublicationLock lock(destination);
  for (const auto &name : names) {
    const auto target = destination / name;
    const auto status = fs::symlink_status(target);
    if (status.type() != fs::file_type::not_found &&
        !fs::is_regular_file(status)) {
      throw fs::filesystem_error(
          "output must be a regular file", target,
          std::make_error_code(std::errc::invalid_argument));
    }
  }
  if (names.empty())
    return;

  struct Backup {
    fs::path path;
    bool existed = false;
  };
  const size_t backup_count = names.size() - 1;
  std::vector<Backup> backups(backup_count);
  for (size_t i = 0; i < backup_count; ++i) {
    const auto target = destination / names[i];
    if (fs::exists(target)) {
      backups[i].existed = true;
      backups[i].path =
          stage.path() / (i == 0 ? std::string("previous-first")
                                 : "previous-" + std::to_string(i));
      fs::copy_file(target, backups[i].path);
    }
  }

  size_t published = 0;
  try {
    for (; published < names.size(); ++published) {
      fs::rename(stage.path() / names[published],
                 destination / names[published]);
    }
  } catch (...) {
    for (size_t i = published; i > 0; --i) {
      const size_t index = i - 1;
      const auto target = destination / names[index];
      std::error_code error;
      if (backups[index].existed)
        fs::rename(backups[index].path, target, error);
      else
        fs::remove(target, error);
      if (error) {
        stage.preserve();
        throw fs::filesystem_error(
            backups[index].existed
                ? "restore output failed; previous output retained at " +
                      backups[index].path.string()
                : "remove new output failed; retained at " + target.string(),
            target, error);
      }
    }
    throw;
  }

  for (const auto &backup : backups) {
    if (backup.existed)
      fs::remove(backup.path);
  }
}

} // namespace lang
