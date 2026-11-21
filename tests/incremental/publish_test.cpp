#include "lang/support/publish.hpp"
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/file.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
const std::vector<std::string> names{"build.log", "records.json", "statistics.json", "program.o", "program"};
constexpr unsigned publications_per_writer = 180;
void write(const fs::path &path, const std::string &bytes) {
  std::ofstream output(path, std::ios::binary); output << bytes; output.close();
  if (!output) throw std::runtime_error("write failed");
}
std::string read(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("read failed");
  return std::string((std::istreambuf_iterator<char>(input)), {});
}
void writer(const fs::path &directory, char tag, int progress) {
  for (unsigned index = 0; index < publications_per_writer; ++index) {
    lang::StagingDir stage(directory);
    const auto bytes = std::string(8192, tag) + std::to_string(index);
    for (const auto &name : names) write(stage.path() / name, bytes);
    lang::publish_staged_files(stage, directory, names);
    ssize_t notified;
    do { notified = ::write(progress, &tag, 1); }
    while (notified < 0 && errno == EINTR);
    if (notified != 1) throw std::runtime_error("notify observer failed");
  }
}
int main(int argc, char **argv) {
  if (argc != 2) return 2;
  try {
    fs::create_directories(argv[1]);
    std::string pattern = (fs::path(argv[1]) / "run-XXXXXX").string();
    if (!::mkdtemp(pattern.data())) throw std::runtime_error("create test directory failed");
    const fs::path directory = fs::path(pattern) / "publish-output";
    fs::create_directories(directory);
    const auto alias = fs::path(pattern) / "publish-alias";
    fs::create_directory_symlink(directory, alias);
    for (const auto &name : names) write(directory / name, "initial");
    // A failure after earlier renames restores every prior output.
    {
      lang::StagingDir stage(directory);
      for (size_t i = 0; i + 1 < names.size(); ++i) write(stage.path() / names[i], "incomplete");
      bool failed = false;
      try { lang::publish_staged_files(stage, directory, names); }
      catch (const fs::filesystem_error &) { failed = true; }
      if (!failed) throw std::runtime_error("missing final input was accepted");
      for (const auto &name : names)
        if (read(directory / name) != "initial") throw std::runtime_error("rollback mismatch");
    }
    int progress[2];
    if (::pipe(progress)) throw std::runtime_error("create progress pipe failed");
    auto first = ::fork();
    if (first == 0) {
      ::close(progress[0]);
      try { writer(directory, 'A', progress[1]); _exit(0); } catch (...) { _exit(1); }
    }
    if (first < 0) throw std::runtime_error("fork failed");
    auto second = ::fork();
    if (second == 0) {
      ::close(progress[0]);
      try { writer(alias, 'B', progress[1]); _exit(0); } catch (...) { _exit(1); }
    }
    if (second < 0) throw std::runtime_error("fork failed");
    ::close(progress[1]);
    int lock = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (lock < 0) throw std::runtime_error("open directory failed");
    size_t observations = 0;
    ssize_t notified;
    do {
      if (::flock(lock, LOCK_SH)) throw std::runtime_error("observer lock failed");
      auto expected = read(directory / names.front());
      for (const auto &name : names)
        if (read(directory / name) != expected) throw std::runtime_error("mixed publication");
      if (::flock(lock, LOCK_UN)) throw std::runtime_error("observer unlock failed");
      ++observations;
      // Wait for a publication before taking another shared lock, which can
      // otherwise starve both writers waiting for an exclusive lock.
      char tag;
      do { notified = ::read(progress[0], &tag, 1); }
      while (notified < 0 && errno == EINTR);
      if (notified < 0) throw std::runtime_error("read publication progress failed");
    } while (notified != 0);
    ::close(progress[0]);
    for (auto process : {first, second}) {
      int status;
      pid_t done;
      do { done = ::waitpid(process, &status, 0); }
      while (done < 0 && errno == EINTR);
      if (done != process || !WIFEXITED(status) || WEXITSTATUS(status))
        throw std::runtime_error("writer failed");
    }
    ::close(lock);
    if (observations != 2 * publications_per_writer + 1)
      throw std::runtime_error("publication count mismatch");
    std::cout << "writers=2 publications=" << observations - 1
              << " locked_observations=" << observations << '\n';
    return 0;
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
