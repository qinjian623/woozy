#include "lang/support/publish.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/file.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
const std::vector<std::string> names{"build.log", "records.json", "statistics.json", "program.o", "program"};
void write(const fs::path &path, const std::string &bytes) {
  std::ofstream output(path, std::ios::binary); output << bytes; output.close();
  if (!output) throw std::runtime_error("write failed");
}
std::string read(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("read failed");
  return std::string((std::istreambuf_iterator<char>(input)), {});
}
void writer(const fs::path &directory, char tag) {
  for (unsigned index = 0; index < 180; ++index) {
    lang::StagingDir stage(directory);
    const auto bytes = std::string(8192, tag) + std::to_string(index);
    for (const auto &name : names) write(stage.path() / name, bytes);
    lang::publish_staged_files(stage, directory, names);
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
    auto first = ::fork();
    if (first == 0) { try { writer(directory, 'A'); _exit(0); } catch (...) { _exit(1); } }
    if (first < 0) throw std::runtime_error("fork failed");
    auto second = ::fork();
    if (second == 0) { try { writer(alias, 'B'); _exit(0); } catch (...) { _exit(1); } }
    if (second < 0) throw std::runtime_error("fork failed");
    int lock = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (lock < 0) throw std::runtime_error("open directory failed");
    bool first_done = false, second_done = false;
    size_t observations = 0;
    while (!first_done || !second_done) {
      if (::flock(lock, LOCK_SH)) throw std::runtime_error("observer lock failed");
      auto expected = read(directory / names.front());
      for (const auto &name : names)
        if (read(directory / name) != expected) throw std::runtime_error("mixed publication");
      if (::flock(lock, LOCK_UN)) throw std::runtime_error("observer unlock failed");
      ++observations;
      int status;
      for (auto process : {first, second}) {
        auto done = ::waitpid(process, &status, WNOHANG);
        if (done == process) {
          if (!WIFEXITED(status) || WEXITSTATUS(status)) throw std::runtime_error("writer failed");
          if (process == first) first_done = true; else second_done = true;
        }
      }
    }
    ::close(lock);
    std::cout << "writers=2 publications=360 locked_observations=" << observations << '\n';
    return 0;
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
