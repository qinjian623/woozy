#include "lang/support/resource.hpp"
#include <cstdlib>
#include <system_error>
#include <unistd.h>
namespace lang {
namespace {
namespace fs = std::filesystem;
fs::path executable_directory() {
  std::error_code error;
  auto executable = fs::read_symlink("/proc/self/exe", error);
  return error ? fs::path{} : executable.parent_path();
}
bool build_root(const fs::path &directory) {
  std::error_code error;
  return fs::is_regular_file(directory / "CMakeCache.txt", error) && !error;
}
bool nested_program(const fs::path &directory) {
  auto name = directory.filename().string();
  return name == "native" || name == "bin" || name == "libexec";
}
}
bool is_executable_file(const fs::path &path) {
  std::error_code error;
  return fs::is_regular_file(path, error) && !error &&
         ::access(path.c_str(), X_OK) == 0;
}
std::optional<fs::path> resource_root() {
  auto directory = executable_directory();
  if (directory.empty() || !directory.is_absolute()) return std::nullopt;
  // A CTest executable under tests/native is not a packaged Native helper.
  for (auto ancestor = directory; ancestor.has_relative_path();
       ancestor = ancestor.parent_path()) {
    if (ancestor.filename() == "tests" && build_root(ancestor.parent_path()))
      return ancestor.parent_path();
  }
  if (nested_program(directory))
    return directory.parent_path();
  return directory;
}
std::vector<fs::path> resource_roots() {
  std::vector<fs::path> roots;
  auto directory = executable_directory();
  if (!directory.empty() && directory.is_absolute()) roots.push_back(directory);
  if (auto root = resource_root(); root && *root != directory)
    roots.push_back(*root);
  return roots;
}
std::optional<fs::path> find_resource_at(const fs::path &root,
                                         std::string_view relative) {
  if (relative.empty()) return std::nullopt;
  for (const auto &base : {root, root / "resources"}) {
    std::error_code error;
    auto candidate = base / fs::path(relative);
    if (fs::exists(candidate, error) && !error) return candidate;
  }
  return std::nullopt;
}
std::optional<fs::path> find_resource(std::string_view relative) {
  for (const auto &root : resource_roots())
    if (auto found = find_resource_at(root, relative)) return found;
  return std::nullopt;
}
std::vector<fs::path> path_directories() {
  std::vector<fs::path> directories;
  const char *path = std::getenv("PATH");
  if (!path) return directories;
  std::string_view entries(path);
  for (size_t begin = 0; begin <= entries.size();) {
    auto end = entries.find(':', begin);
    auto entry = entries.substr(begin, end == std::string_view::npos
                                           ? std::string_view::npos
                                           : end - begin);
    if (!entry.empty() && fs::path(entry).is_absolute())
      directories.emplace_back(entry);
    if (end == std::string_view::npos) break;
    begin = end + 1;
  }
  return directories;
}
std::optional<fs::path> find_program(std::string_view name) {
  fs::path requested(name);
  if (requested.has_parent_path())
    return is_executable_file(requested) ? std::optional<fs::path>(requested)
                                         : std::nullopt;
  for (const auto &directory : path_directories()) {
    auto candidate = directory / requested;
    if (is_executable_file(candidate)) return candidate;
  }
  return std::nullopt;
}
}
