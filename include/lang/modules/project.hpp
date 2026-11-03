#pragma once
#include "lang/syntax/parser.hpp"
#include <filesystem>

namespace lang::modules {
enum class ForeignExportKind { type, value, function };
struct ForeignExport {
  std::string name, identity;
  ForeignExportKind kind;
  size_t bundle;
};
struct BindingBundle {
  std::filesystem::path path;
  std::string json;
};
struct Import {
  size_t file;
  AstNodeId node;
  ModuleId target;
  std::string alias;
  SourceSpan span;
};
struct Module {
  ModuleId id;
  std::string logical_path, identity, package_name;
  std::vector<std::shared_ptr<const AstFile>> files;
  std::vector<Import> imports;
  std::vector<ForeignExport> foreign_exports;
};
struct ProjectOptions {
  std::filesystem::path manifest;
  std::optional<std::filesystem::path> std_source_root;
  std::optional<std::filesystem::path> std_binding_bundle;
};
struct Project {
  std::filesystem::path root;
  std::string package_name, backend;
  unsigned word_bits = 64;
  ModuleId entry;
  std::vector<Module> modules;
  std::vector<BindingBundle> bindings;
  const Module &module(ModuleId id) const { return modules.at(id.value); }
};
// IDs index this Project. SourceManager remains the owner used to print spans.
PhaseResult<Project> load_project(const ProjectOptions &, SourceManager &,
                                  std::stop_token = {});
} // namespace lang::modules
