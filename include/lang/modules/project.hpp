#pragma once
#include "lang/syntax/parser.hpp"
#include <filesystem>
#include <variant>

namespace lang::modules {
// Parsed declarations only. Review status is a claim in the input, not proof
// that a package was imported or checked against a dependency lock.
struct BindingParameter {
  std::string name, type, kind;
  bool optional;
  std::string mode, escapes;
  SourceSpan source;
};
struct BindingType {
  std::string kind, copy;
  std::vector<ForeignBindingId> members; // method/callable candidates, by owner
};
struct BindingValue {
  std::string type;
};
struct BindingCallable {
  std::string call_kind;
  std::optional<std::string> owner_type;
  std::vector<BindingParameter> parameters;
  std::string return_type;
  std::optional<std::string> error_type;
  std::string return_ownership, storage, receiver_escapes;
  std::vector<std::string> effects;
};
struct ForeignDeclaration {
  ForeignBindingId id; // Project-local index, never a persistent identity.
  size_t bundle;
  SourceSpan source;
  std::string json_pointer;
  // Original id for types/callables; module/name for values (which have no id).
  std::string identity, module, name;
  std::vector<std::string> host_attribute;
  std::variant<BindingType, BindingValue, BindingCallable> detail;
  // Length-delimited contract, bundle SHA-256, module, category and identity.
  std::string stable_identity;
};
struct BindingModule {
  std::string logical_path, host_module, provider;
  SourceSpan source;
};
struct PackageRequirement {
  std::string package, version_policy;
  SourceSpan source;
};
struct BindingModuleRef {
  size_t bundle, index;
};
struct ForeignExportRef {
  ModuleId module;
  size_t index;
};
enum class ForeignExportKind { type, value, function };
struct ForeignExport {
  std::string name, identity;
  ForeignExportKind kind;
  size_t bundle;
  std::optional<ForeignBindingId> declaration; // type or value
  // Ordinary function overloads, or constructors attached to a type.
  std::vector<ForeignBindingId> candidates;
};
struct BindingBundle {
  std::filesystem::path path;
  std::string json;
  std::string sha256;
  SourceSpan source;
  int schema_version;
  std::string contract, review_status;
  std::vector<std::string> allowed_backends;
  std::vector<PackageRequirement> package_requirements;
  std::vector<BindingModule> modules;
  std::vector<ForeignBindingId> declarations;
  std::vector<std::string> conformance_required;
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
  std::optional<BindingModuleRef> binding;
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
  std::vector<ForeignDeclaration> foreign_bindings;
  const Module &module(ModuleId id) const { return modules.at(id.value); }
  const ForeignDeclaration &foreign_binding(ForeignBindingId id) const {
    return foreign_bindings.at(id.value);
  }
  const ForeignExport &foreign_export(ForeignExportRef ref) const {
    return module(ref.module).foreign_exports.at(ref.index);
  }
  const BindingModule *binding_module(ModuleId id) const {
    const auto &ref = module(id).binding;
    return ref ? &bindings.at(ref->bundle).modules.at(ref->index) : nullptr;
  }
};
// IDs index this Project. SourceManager remains the owner used to print spans.
PhaseResult<Project> load_project(const ProjectOptions &, SourceManager &,
                                  std::stop_token = {});
} // namespace lang::modules
