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
  std::vector<ForeignBindingId> properties, indexers, slices;
};
struct BindingValue {
  std::string type;
};
// A declared host property. `getter` is a nonempty attribute path relative to
// the owner value. An empty setter path means the property is read-only.
struct BindingProperty {
  std::string owner_type, name, type;
  std::vector<std::string> getter, setter;
  std::string error_type;
};
// A declared single-index operation. The fixed result type is both the read
// type and the write value type; there is no runtime dtype inference.
struct BindingIndexer {
  std::string owner_type, index_type, result_type, error_type;
  bool readable = false, writable = false;
};
// A declared Python slice. A null endpoint type means that endpoint cannot be
// supplied in source; an omitted endpoint is passed as None.
struct BindingSlice {
  std::string owner_type, result_type, error_type;
  std::optional<std::string> start_type, end_type;
  bool readable = false, writable = false;
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
  std::variant<BindingType, BindingValue, BindingCallable, BindingProperty,
               BindingIndexer, BindingSlice>
      detail;
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

// ---------------------------------------------------------------------------
// C ABI bindings (schema v2). These records are intentionally independent of
// the Python schema-v1 records above. A C type expression is structural:
// scalars, Ptr[T]/Buffer[T], or one named record/resource/callback/opaque
// declaration. `writable` is carried for pointers and buffers only.
struct CTypeRef {
  enum class Kind { scalar, pointer, buffer, named } kind = Kind::scalar;
  std::string scalar;      // canonical scalar spelling
  bool writable = false;   // pointer/buffer write permission
  std::vector<CTypeRef> args; // exactly one pointee/element for pointer/buffer
  uint32_t named = 0;      // 1-based index in Project::c_types
  bool operator==(const CTypeRef &) const = default;
};
enum class CDeclKind { record, resource, callback, opaque };
struct CFieldDecl {
  std::string name;
  CTypeRef type;
  uint64_t offset = 0;
  SourceSpan source;
};
struct CParameterDecl {
  std::string name;
  CTypeRef type;
  SourceSpan source;
};
struct CRegistrationDecl {
  uint32_t callback_type = 0; // Project::c_types, rebased with the bundle
  std::string unregister_symbol;
};
struct CRegistrationCall {
  size_t callback_parameter = 0; // indexes into the original C parameter list
  size_t userdata_parameter = 0;
  size_t callback_userdata_parameter = 0;
};
struct CTypeDecl {
  uint32_t id = 0; // 1-based index in Project::c_types, source declaration ID
  size_t bundle = 0;
  SourceSpan source;
  std::string identity, module, name, c_name;
  CDeclKind kind = CDeclKind::opaque;
  std::vector<CFieldDecl> fields; // record only, declaration order
  uint64_t size = 0, align = 0;
  // Resource lifetime. Retain may be empty (such a value cannot be copied).
  std::string release_symbol, retain_symbol;
  // Callback signature; result is a C type expression.
  CTypeRef result;
  std::vector<CParameterDecl> parameters;
  std::optional<CRegistrationDecl> registration; // managed resource only
};
struct CReturnDecl {
  CTypeRef type;
  std::string ownership; // value | owned | borrowed | unit
  std::string release_symbol, retain_symbol;
  SourceSpan source;
};
struct CCallableDecl {
  uint32_t id = 0; // 1-based index in Project::c_callables
  size_t bundle = 0;
  SourceSpan source;
  std::string identity, module, name, symbol;
  std::vector<CParameterDecl> parameters; // fixed positional order
  CReturnDecl result;
  std::string stable_identity;
  std::optional<CRegistrationCall> registers;
};
inline std::vector<size_t>
c_source_parameter_indices(const CCallableDecl &callable) {
  std::vector<size_t> result;
  for (size_t i = 0; i < callable.parameters.size(); ++i)
    if (!callable.registers || i != callable.registers->userdata_parameter)
      result.push_back(i);
  return result;
}
struct CModule {
  std::string logical_path, provider, library;
  SourceSpan source;
};
struct CBindingBundle {
  std::filesystem::path path;
  std::string json, sha256;
  SourceSpan source;
  int schema_version = 2;
  std::string contract, abi, target;
  bool requires_hosted = false;
  std::vector<CModule> modules;
  std::vector<uint32_t> types, callables;
};
struct ForeignExport {
  std::string name, identity;
  ForeignExportKind kind;
  size_t bundle;
  std::optional<ForeignBindingId> declaration; // type or value
  // Ordinary function overloads, or constructors attached to a type.
  std::vector<ForeignBindingId> candidates;
  // C ABI v2 exports. Exactly one C identity is set for a C export; the
  // Python fields above remain untouched.
  std::optional<uint32_t> c_type, c_callable;
  std::optional<CTypeRef> c_type_ref; // resolved structural alias or named type
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
  std::optional<size_t> c_binding; // Project::c_bindings index
};
struct ParseCache;
struct ProjectOptions {
  std::filesystem::path manifest;
  std::optional<std::filesystem::path> std_source_root;
  std::optional<std::filesystem::path> std_binding_bundle;
  std::shared_ptr<ParseCache> parse_cache;
};
struct Project {
  std::filesystem::path root;
  std::string package_name, backend;
  unsigned word_bits = 64;
  bool hosted = false; // explicit [build.native] runtime = "hosted"
  ModuleId entry;
  std::vector<Module> modules;
  std::vector<BindingBundle> bindings;
  std::vector<ForeignDeclaration> foreign_bindings;
  std::vector<CBindingBundle> c_bindings;
  std::vector<CTypeDecl> c_types;       // 1-based IDs through module->c_type
  std::vector<CCallableDecl> c_callables; // 1-based IDs
  // Link inputs collected from native dependencies, stable first-use order.
  std::vector<std::string> c_libraries;
  std::vector<std::filesystem::path> c_library_dirs;
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
  const CTypeDecl &c_type(uint32_t id) const {
    return c_types.at(id - 1);
  }
  const CCallableDecl &c_callable(uint32_t id) const {
    return c_callables.at(id - 1);
  }
};
// IDs index this Project. SourceManager remains the owner used to print spans.
PhaseResult<Project> load_project(const ProjectOptions &, SourceManager &,
                                  std::stop_token = {});
} // namespace lang::modules
