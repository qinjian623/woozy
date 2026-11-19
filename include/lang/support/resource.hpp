#pragma once
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>
namespace lang {
// Bounded executable-relative resource roots: the executable directory and
// one package root (a parent for native/bin layouts or the nearest CMake build
// root). The current working directory is never consulted.
std::vector<std::filesystem::path> resource_roots();
std::optional<std::filesystem::path> resource_root();
std::optional<std::filesystem::path> find_resource_at(
    const std::filesystem::path &root, std::string_view relative);
std::optional<std::filesystem::path> find_resource(std::string_view relative);
// Absolute entries of PATH, in order. Empty and relative entries are ignored.
std::vector<std::filesystem::path> path_directories();
std::optional<std::filesystem::path> find_program(std::string_view name);
bool is_executable_file(const std::filesystem::path &path);
}
