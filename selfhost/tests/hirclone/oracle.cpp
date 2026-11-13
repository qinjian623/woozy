// C++ reference oracle for the pure-Wz hirclone tests.
//
// This is a test-only translation unit linked against the existing stage0
// frontend/hir libraries.  It mirrors the Woozy fixture closely enough to run
// the same field probe table (same ids, names and order) and prints a report in
// the same format as selfhost/tests/hirclone/src/main.wz.
//
// Reference copy semantics: lang/hir/hir.cpp freeze() (around line 7916)
// copies HirModule by value, then deep-copies ResolvedProject, Project and
// every AstFile arena.  C++ std::vector copies are already deep, so this TU
// performs exactly that copy and checks it under the same mutation probes.
// A C++ SourceSnapshot is an immutable support object, so byte/position table
// probes have no mutable C++ counterpart; ParseResult diagnostics are not part
// of the C++ module either.  Those probe ids print "cpp=none".  The four narrow
// Wz entry points (clone_node/clone_function/clone_pattern/clone_constant) and
// the Wz-only depth/cycle SupportError branch have no C++ API either, so they
// also print cpp=none; the oracle compares the normal trajectory only.
#include "lang/hir/hir.hpp"
#include "lang/modules/project.hpp"
#include "lang/support/source.hpp"
#include "lang/syntax/parser.hpp"

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace lang;
using namespace lang::hir;

namespace {

const char *kCodeOld =
    "\xEF\xBB\xBFpackage app;\r\n// \xE7\xAC\xAC\xE4\xB8\x80\xE7\x89\x88 "
    "\xF0\x9F\x98\x80\r\nlet answer i32 = 42;\r\n"
    "func add(a i32, b i32) i32 { return a + b; }\r\n";
const char *kCodeNew =
    "package app;\n// \xE7\xAC\xAC\xE4\xBA\x8C\xE7\x89\x88\n"
    "let answer i32 = 43;\n"
    "func add(a i32, b i32) i32 { return a + b; }\n";

struct TypeIds {
  TypeId unit, boolean, i32, u32, i64, f64, string, usize_id;
  TypeId vec_i32, slice_i32, weak_i32;
  TypeId signature, throwing, nominal, c_pointer, c_record;
};

std::vector<TypeId> types(std::initializer_list<TypeId> values) {
  return std::vector<TypeId>(values);
}

Source ast_source(const AstFile &file, size_t file_index,
                  size_t ast_index) {
  const size_t at = ast_index % file.nodes.size();
  return Source{modules::NodeRef{ModuleId{0}, file_index, AstNodeId{at}},
                file.nodes[at].span};
}

void span_text(std::ostringstream &s, SourceSpan span) {
  s << span.snapshot.value << ':' << span.begin_byte << ':' << span.end_byte;
}

void ctype_ref_text(std::ostringstream &s, const modules::CTypeRef &ref) {
  s << '[' << int(ref.kind) << '/' << std::quoted(ref.scalar) << '/'
    << ref.writable << '/' << ref.named;
  for (const auto &arg : ref.args)
    ctype_ref_text(s, arg);
  s << ']';
}

void source_text(std::ostringstream &s, const SourceSnapshot &source) {
  s << "src=" << source.file.value << ':' << source.revision.value << ':'
    << source.id.value << ':' << std::quoted(source.path) << ':'
    << source.valid() << ':' << source.original.size() << ':'
    << std::quoted(source.original) << ':'
    << std::quoted(source.normalized()) << '\n';
}

void ast_text(std::ostringstream &s, const AstFile &file) {
  source_text(s, *file.source);
  s << "tokens=" << file.tokens.size() << '\n';
  for (const auto &token : file.tokens) {
    s << "tok " << int(token.kind) << ' ';
    span_text(s, token.span);
    s << ' ' << std::quoted(std::string(token.spelling)) << " trivia="
      << token.leading_trivia.size();
    for (const auto &trivia : token.leading_trivia) {
      s << ' ' << int(trivia.kind) << ' ';
      span_text(s, trivia.span);
      s << ' ' << std::quoted(std::string(trivia.spelling));
    }
    s << " str=";
    if (token.string_value)
      s << std::quoted(*token.string_value);
    else
      s << '-';
    s << '\n';
  }
  s << "nodes=" << file.nodes.size() << '\n';
  for (const auto &node : file.nodes) {
    s << "ast " << int(node.kind) << ' ';
    span_text(s, node.span);
    s << ' ' << node.token_begin << ':' << node.token_end << " kids=";
    for (auto child : node.children)
      s << child.value << ',';
    s << " rec=" << node.recovered << '\n';
  }
  s << "root=" << file.root.value << " rec=" << file.recovered << '\n';
}

void binding_type_text(std::ostringstream &s,
                       const modules::BindingBundle &bundle) {
  s << "bundle " << std::quoted(bundle.path.string()) << ' '
    << std::quoted(bundle.json) << ' ' << std::quoted(bundle.sha256) << ' '
    << bundle.schema_version << ' ' << std::quoted(bundle.contract) << ' '
    << std::quoted(bundle.review_status) << '\n';
  for (const auto &value : bundle.allowed_backends)
    s << "backend " << std::quoted(value) << '\n';
  for (const auto &value : bundle.package_requirements)
    s << "require " << std::quoted(value.package) << ' '
      << std::quoted(value.version_policy) << '\n';
  for (const auto &value : bundle.modules)
    s << "bmod " << std::quoted(value.logical_path) << ' '
      << std::quoted(value.host_module) << ' '
      << std::quoted(value.provider) << '\n';
  for (auto value : bundle.declarations)
    s << "bdecl " << value.value << '\n';
  for (const auto &value : bundle.conformance_required)
    s << "confirm " << std::quoted(value) << '\n';
}

void foreign_text(std::ostringstream &s,
                  const modules::ForeignDeclaration &value) {
  s << "foreign " << value.id.value << ' ' << value.bundle << ' '
    << std::quoted(value.json_pointer) << ' ' << std::quoted(value.identity)
    << ' ' << std::quoted(value.module) << ' ' << std::quoted(value.name)
    << ' ' << std::quoted(value.stable_identity) << '\n';
  for (const auto &item : value.host_attribute)
    s << "attr " << std::quoted(item) << '\n';
  std::visit(
      [&](const auto &payload) {
        using T = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<T, modules::BindingType>) {
          s << "dtype " << std::quoted(payload.kind) << ' '
            << std::quoted(payload.copy) << '\n';
          for (auto member : payload.members)
            s << "member " << member.value << '\n';
          for (auto member : payload.properties)
            s << "property " << member.value << '\n';
          for (auto member : payload.indexers)
            s << "indexer " << member.value << '\n';
          for (auto member : payload.slices)
            s << "slice " << member.value << '\n';
        } else if constexpr (std::is_same_v<T, modules::BindingValue>) {
          s << "dvalue " << std::quoted(payload.type) << '\n';
        } else if constexpr (std::is_same_v<T, modules::BindingCallable>) {
          s << "dcall " << std::quoted(payload.call_kind) << ' '
            << std::quoted(payload.return_type) << ' '
            << std::quoted(payload.return_ownership) << ' '
            << std::quoted(payload.storage) << ' '
            << std::quoted(payload.receiver_escapes) << '\n';
          if (payload.owner_type)
            s << "owner " << std::quoted(*payload.owner_type) << '\n';
          if (payload.error_type)
            s << "error " << std::quoted(*payload.error_type) << '\n';
          for (const auto &parameter : payload.parameters)
            s << "cparam " << std::quoted(parameter.name) << ' '
              << std::quoted(parameter.type) << ' '
              << std::quoted(parameter.kind) << ' ' << parameter.optional
              << ' ' << std::quoted(parameter.mode) << ' '
              << std::quoted(parameter.escapes) << '\n';
          for (const auto &effect : payload.effects)
            s << "effect " << std::quoted(effect) << '\n';
        } else if constexpr (std::is_same_v<T, modules::BindingProperty>) {
          s << "dprop " << std::quoted(payload.owner_type) << ' '
            << std::quoted(payload.name) << ' ' << std::quoted(payload.type)
            << ' ' << std::quoted(payload.error_type) << '\n';
          for (const auto &item : payload.getter)
            s << "getter " << std::quoted(item) << '\n';
          for (const auto &item : payload.setter)
            s << "setter " << std::quoted(item) << '\n';
        } else if constexpr (std::is_same_v<T, modules::BindingIndexer>) {
          s << "dindex " << std::quoted(payload.owner_type) << ' '
            << std::quoted(payload.index_type) << ' '
            << std::quoted(payload.result_type) << ' '
            << std::quoted(payload.error_type) << ' ' << payload.readable
            << ' ' << payload.writable << '\n';
        } else {
          s << "dslice " << std::quoted(payload.owner_type) << ' '
            << std::quoted(payload.result_type) << ' '
            << std::quoted(payload.error_type) << ' ' << payload.readable
            << ' ' << payload.writable << '\n';
          if (payload.start_type)
            s << "start " << std::quoted(*payload.start_type) << '\n';
          if (payload.end_type)
            s << "end " << std::quoted(*payload.end_type) << '\n';
        }
      },
      value.detail);
}

void c_type_decl_text(std::ostringstream &s, const modules::CTypeDecl &value) {
  s << "ctype " << value.id << ' ' << value.bundle << ' '
    << std::quoted(value.identity) << ' ' << std::quoted(value.module) << ' '
    << std::quoted(value.name) << ' ' << std::quoted(value.c_name) << ' '
    << int(value.kind) << ' ' << value.size << ':' << value.align << ' '
    << std::quoted(value.release_symbol) << ' '
    << std::quoted(value.retain_symbol) << '\n';
  for (const auto &field : value.fields) {
    s << "cfield " << std::quoted(field.name) << ' ' << field.offset;
    ctype_ref_text(s, field.type);
    s << '\n';
  }
  s << "cresult";
  ctype_ref_text(s, value.result);
  s << '\n';
  for (const auto &parameter : value.parameters) {
    s << "ctypeparam " << std::quoted(parameter.name);
    ctype_ref_text(s, parameter.type);
    s << '\n';
  }
  if (value.registration)
    s << "reg " << value.registration->callback_type << ' '
      << std::quoted(value.registration->unregister_symbol) << '\n';
}

void c_callable_text(std::ostringstream &s,
                     const modules::CCallableDecl &value) {
  s << "ccall " << value.id << ' ' << value.bundle << ' '
    << std::quoted(value.identity) << ' ' << std::quoted(value.module) << ' '
    << std::quoted(value.name) << ' ' << std::quoted(value.symbol) << ' '
    << std::quoted(value.stable_identity) << '\n';
  for (const auto &parameter : value.parameters) {
    s << "ccparam " << std::quoted(parameter.name);
    ctype_ref_text(s, parameter.type);
    s << '\n';
  }
  s << "cret " << std::quoted(value.result.ownership) << ' '
    << std::quoted(value.result.release_symbol) << ' '
    << std::quoted(value.result.retain_symbol);
  ctype_ref_text(s, value.result.type);
  s << '\n';
  if (value.registers)
    s << "creg " << value.registers->callback_parameter << ' '
      << value.registers->userdata_parameter << ' '
      << value.registers->callback_userdata_parameter << '\n';
}

void c_binding_text(std::ostringstream &s,
                    const modules::CBindingBundle &value) {
  s << "cbundle " << std::quoted(value.path.string()) << ' '
    << std::quoted(value.json) << ' ' << std::quoted(value.sha256) << ' '
    << value.schema_version << ' ' << std::quoted(value.contract) << ' '
    << std::quoted(value.abi) << ' ' << std::quoted(value.target) << ' '
    << value.requires_hosted << '\n';
  for (const auto &module : value.modules)
    s << "cmodule " << std::quoted(module.logical_path) << ' '
      << std::quoted(module.provider) << ' '
      << std::quoted(module.library) << '\n';
  for (auto type : value.types)
    s << "cbtype " << type << '\n';
  for (auto callable : value.callables)
    s << "cbcall " << callable << '\n';
}

void module_text(std::ostringstream &s, const modules::Module &value) {
  s << "module " << value.id.value << ' ' << std::quoted(value.logical_path)
    << ' ' << std::quoted(value.identity) << ' '
    << std::quoted(value.package_name) << '\n';
  for (const auto &file : value.files) {
    s << "file\n";
    ast_text(s, *file);
  }
  for (const auto &import_value : value.imports) {
    s << "import " << import_value.file << ' ' << import_value.node.value
      << ' ' << import_value.target.value << ' '
      << std::quoted(import_value.alias) << '\n';
  }
  for (const auto &export_value : value.foreign_exports) {
    s << "export " << std::quoted(export_value.name) << ' '
      << std::quoted(export_value.identity) << ' ' << int(export_value.kind)
      << ' ' << export_value.bundle << '\n';
    if (export_value.declaration)
      s << "decl " << export_value.declaration->value << '\n';
    for (auto candidate : export_value.candidates)
      s << "candidate " << candidate.value << '\n';
    if (export_value.c_type)
      s << "ctypeid " << *export_value.c_type << '\n';
    if (export_value.c_callable)
      s << "ccallid " << *export_value.c_callable << '\n';
    if (export_value.c_type_ref) {
      s << "ctypealias";
      ctype_ref_text(s, *export_value.c_type_ref);
      s << '\n';
    }
  }
  if (value.binding)
    s << "binding " << value.binding->bundle << ':' << value.binding->index
      << '\n';
  if (value.c_binding)
    s << "cbinding " << *value.c_binding << '\n';
}

std::string dump_project(const modules::ResolvedProject &resolved) {
  std::ostringstream s;
  const auto &project = *resolved.project;
  s << "project " << std::quoted(project.root.string()) << ' '
    << std::quoted(project.package_name) << ' ' << std::quoted(project.backend)
    << ' ' << project.word_bits << ' ' << project.hosted << ' '
    << project.entry.value << '\n';
  for (const auto &module : project.modules)
    module_text(s, module);
  for (const auto &bundle : project.bindings)
    binding_type_text(s, bundle);
  for (const auto &declaration : project.foreign_bindings)
    foreign_text(s, declaration);
  for (const auto &bundle : project.c_bindings)
    c_binding_text(s, bundle);
  for (const auto &declaration : project.c_types)
    c_type_decl_text(s, declaration);
  for (const auto &callable : project.c_callables)
    c_callable_text(s, callable);
  for (const auto &library : project.c_libraries)
    s << "lib " << std::quoted(library) << '\n';
  for (const auto &directory : project.c_library_dirs)
    s << "libdir " << std::quoted(directory.string()) << '\n';
  for (const auto &symbol : resolved.symbols) {
    s << "symbol " << symbol.id.value << ' ' << int(symbol.kind) << ' '
      << std::quoted(symbol.name) << ' ' << symbol.module.value << ' '
      << symbol.exported << '\n';
    if (symbol.definition)
      s << "def " << symbol.definition->module.value << ':'
        << symbol.definition->file << ':' << symbol.definition->node.value
        << '\n';
    if (symbol.target)
      s << "target " << symbol.target->value << '\n';
    if (symbol.foreign)
      s << "forexport " << symbol.foreign->module.value << ':'
        << symbol.foreign->index << '\n';
  }
  for (const auto &occurrence : resolved.occurrences)
    s << "occurrence " << occurrence.symbol.value << ' '
      << occurrence.source.module.value << ':' << occurrence.source.file << ':'
      << occurrence.source.node.value << '\n';
  for (const auto &member : resolved.deferred_members)
    s << "deferred " << member.module.value << ':' << member.file << ':'
      << member.node.value << '\n';
  for (const auto &parameter : resolved.self_parameters)
    s << "self " << parameter.second.value << ' '
      << parameter.first.module.value << ':' << parameter.first.file << ':'
      << parameter.first.node.value << '\n';
  return s.str();
}

struct Fixture {
  SourceManager sources;
  std::shared_ptr<modules::Project> project;
  std::shared_ptr<modules::ResolvedProject> resolved;
  HirModule module;
  TypeIds ids;
};

struct Working {
  std::shared_ptr<modules::Project> project;
  std::shared_ptr<modules::ResolvedProject> resolved;
  HirModule module;
};

Working clone_working(const Fixture &f) {
  Working w;
  w.project = std::make_shared<modules::Project>(*f.project);
  // freeze() deep-copies every AstFile arena.  Keep the test copy mutable so
  // mutation probes can observe whether the original arena was isolated.
  for (auto &module : w.project->modules)
    for (auto &file : module.files)
      file = std::make_shared<AstFile>(*file);
  w.resolved = std::make_shared<modules::ResolvedProject>(*f.resolved);
  w.resolved->project = w.project;
  w.module = f.module;
  w.module.resolved = w.resolved;
  return w;
}

// dump(HirModule) prints TraitObjectInit.method_args only up to methods.size().
// The Wz fingerprint covers every stored method argument, so the oracle
// appends the missing part explicitly.
std::string dump_extra(const HirModule &module) {
  std::ostringstream s;
  for (const auto &node : module.nodes) {
    if (const auto *payload = std::get_if<TraitObjectInit>(&node.data)) {
      s << "method_args=" << payload->method_args.size();
      for (const auto &args : payload->method_args) {
        s << ' ';
        for (auto arg : args)
          s << arg.value << ',';
      }
      s << '\n';
    }
  }
  return s.str();
}

std::string fp(const Working &w) {
  return dump(w.module) + dump_project(*w.resolved) + dump_extra(w.module);
}

std::string fp(const Fixture &f) {
  return dump(f.module) + dump_project(*f.resolved) + dump_extra(f.module);
}

template <class T> void grow(std::vector<T> &values) {
  T copy = values.at(0);
  values.push_back(std::move(copy));
}

void add_node(HirModule &m, TypeId type, Source source, ScopeId scope,
              std::vector<HirNodeId> children, NodeData data,
              std::vector<TypeId> errors, std::vector<ExitPlan> exits) {
  m.nodes.push_back(
      HirNode{type, source, scope, std::move(children), std::move(data),
              std::move(errors), std::move(exits)});
}

Fixture make_fixture() {
  Fixture f;
  const FileId file_id = f.sources.add_file("app/main.wz");
  auto old_source = f.sources.update(file_id, kCodeOld);
  auto new_source = f.sources.update(file_id, kCodeNew);
  auto old_lex = lex(old_source);
  auto new_lex = lex(new_source);
  if (!old_lex.success() || !new_lex.success())
    throw std::runtime_error("fixture lex failed");
  auto old_parse = parse(*old_lex.value);
  auto new_parse = parse(*new_lex.value);
  if (!old_parse.success() || !new_parse.success())
    throw std::runtime_error("fixture parse failed");
  const auto &old_file = *old_parse.value;
  const auto &new_file = *new_parse.value;
  const SourceSpan zero{SourceSnapshotId{0}, 0, 0};

  f.project = std::make_shared<modules::Project>();
  f.project->root = "/fixture";
  f.project->package_name = "fixture";
  f.project->backend = "native";
  f.project->word_bits = 64;
  f.project->hosted = true;
  f.project->entry = ModuleId{0};
  f.project->c_libraries = {"m", "c"};
  f.project->c_library_dirs = {"/usr/lib", "/lib"};

  modules::Module module;
  module.id = ModuleId{0};
  module.logical_path = "app";
  module.identity = "app:main";
  module.package_name = "fixture";
  module.files.push_back(std::make_shared<AstFile>(old_file));
  module.files.push_back(std::make_shared<AstFile>(new_file));
  module.imports.push_back(
      modules::Import{0, AstNodeId{0}, ModuleId{0}, "m", zero});
  modules::ForeignExport export_value;
  export_value.name = "Vec";
  export_value.identity = "py:Vec";
  export_value.kind = modules::ForeignExportKind::type;
  export_value.bundle = 0;
  export_value.declaration = ForeignBindingId{0};
  export_value.candidates = {ForeignBindingId{0}, ForeignBindingId{1}};
  export_value.c_type = uint32_t{1};
  export_value.c_callable = uint32_t{2};
  modules::CTypeRef alias;
  alias.kind = modules::CTypeRef::Kind::pointer;
  alias.scalar = "u8";
  alias.writable = true;
  modules::CTypeRef alias_arg;
  alias_arg.kind = modules::CTypeRef::Kind::scalar;
  alias_arg.scalar = "u8";
  alias.args.push_back(alias_arg);
  export_value.c_type_ref = alias;
  module.foreign_exports.push_back(export_value);
  module.binding = modules::BindingModuleRef{0, 0};
  module.c_binding = size_t{0};
  f.project->modules.push_back(std::move(module));

  modules::BindingBundle bundle;
  bundle.path = "py/numpy.json";
  bundle.json = "{}";
  bundle.sha256 = "abc";
  bundle.schema_version = 1;
  bundle.contract = "v1";
  bundle.review_status = "reviewed";
  bundle.allowed_backends = {"native", "python"};
  bundle.package_requirements.push_back(
      modules::PackageRequirement{"numpy", "^1.0", zero});
  bundle.modules.push_back(
      modules::BindingModule{"py/numpy", "numpy", "python", zero});
  bundle.declarations = {ForeignBindingId{0}, ForeignBindingId{1}};
  bundle.conformance_required = {"shape"};
  f.project->bindings.push_back(std::move(bundle));

  auto add_foreign = [&](modules::ForeignDeclaration declaration) {
    f.project->foreign_bindings.push_back(std::move(declaration));
  };
  modules::ForeignDeclaration type_decl;
  type_decl.id = ForeignBindingId{0};
  type_decl.bundle = 0;
  type_decl.json_pointer = "/types/0";
  type_decl.identity = "py:Vec";
  type_decl.module = "numpy";
  type_decl.name = "Vec";
  type_decl.host_attribute = {"numpy.ndarray"};
  modules::BindingType binding_type;
  binding_type.kind = "class";
  binding_type.copy = "share";
  binding_type.members = {ForeignBindingId{0}, ForeignBindingId{2}};
  binding_type.properties = {ForeignBindingId{1}};
  binding_type.indexers = {ForeignBindingId{0}};
  binding_type.slices = {ForeignBindingId{0}};
  type_decl.detail = binding_type;
  type_decl.stable_identity = "stable:type";
  add_foreign(type_decl);

  modules::ForeignDeclaration value_decl;
  value_decl.id = ForeignBindingId{1};
  value_decl.bundle = 0;
  value_decl.json_pointer = "/values/1";
  value_decl.identity = "py:pi";
  value_decl.module = "numpy";
  value_decl.name = "pi";
  value_decl.detail = modules::BindingValue{"f64"};
  value_decl.stable_identity = "stable:value";
  add_foreign(value_decl);

  modules::ForeignDeclaration call_decl;
  call_decl.id = ForeignBindingId{2};
  call_decl.bundle = 0;
  call_decl.json_pointer = "/callables/2";
  call_decl.identity = "py:array";
  call_decl.module = "numpy";
  call_decl.name = "array";
  modules::BindingCallable callable;
  callable.call_kind = "function";
  callable.parameters.push_back(modules::BindingParameter{
      "value", "i32", "value", false, "ordinary", "none", zero});
  callable.parameters.push_back(modules::BindingParameter{
      "where", "string", "value", true, "inout", "borrow", zero});
  callable.return_type = "Vec";
  callable.error_type = "py.Error";
  callable.return_ownership = "owned";
  callable.storage = "runtime";
  callable.receiver_escapes = "none";
  callable.effects = {"gpu"};
  call_decl.detail = callable;
  call_decl.stable_identity = "stable:callable";
  add_foreign(call_decl);

  modules::ForeignDeclaration property_decl;
  property_decl.id = ForeignBindingId{3};
  property_decl.bundle = 0;
  property_decl.json_pointer = "/properties/3";
  property_decl.identity = "py:shape";
  property_decl.module = "numpy";
  property_decl.name = "shape";
  modules::BindingProperty property;
  property.owner_type = "Vec";
  property.name = "shape";
  property.type = "Vec[i32]";
  property.getter = {"shape"};
  property.setter = {"set_shape"};
  property.error_type = "py.Error";
  property_decl.detail = property;
  property_decl.stable_identity = "stable:property";
  add_foreign(property_decl);

  modules::ForeignDeclaration index_decl;
  index_decl.id = ForeignBindingId{4};
  index_decl.bundle = 0;
  index_decl.json_pointer = "/indexers/4";
  index_decl.identity = "py:index";
  index_decl.module = "numpy";
  index_decl.name = "index";
  index_decl.detail = modules::BindingIndexer{
      "Vec", "i32", "i32", "py.Error", true, true};
  index_decl.stable_identity = "stable:indexer";
  add_foreign(index_decl);

  modules::ForeignDeclaration slice_decl;
  slice_decl.id = ForeignBindingId{5};
  slice_decl.bundle = 0;
  slice_decl.json_pointer = "/slices/5";
  slice_decl.identity = "py:slice";
  slice_decl.module = "numpy";
  slice_decl.name = "slice";
  slice_decl.detail = modules::BindingSlice{
      "Vec", "Vec[i32]", "py.Error", std::string("i32"), std::nullopt, true,
      false};
  slice_decl.stable_identity = "stable:slice";
  add_foreign(slice_decl);

  modules::CBindingBundle c_bundle;
  c_bundle.path = "c/math.json";
  c_bundle.json = "{}";
  c_bundle.sha256 = "def";
  c_bundle.schema_version = 2;
  c_bundle.contract = "v2";
  c_bundle.abi = "c";
  c_bundle.target = "x86_64-linux";
  c_bundle.modules.push_back(modules::CModule{"c/math", "c", "m", zero});
  c_bundle.types = {1};
  c_bundle.callables = {1};
  f.project->c_bindings.push_back(std::move(c_bundle));

  modules::CTypeDecl c_type;
  c_type.id = 1;
  c_type.bundle = 0;
  c_type.identity = "c:Point";
  c_type.module = "math";
  c_type.name = "Point";
  c_type.c_name = "point";
  c_type.kind = modules::CDeclKind::record;
  c_type.size = 16;
  c_type.align = 8;
  c_type.release_symbol = "point_release";
  c_type.retain_symbol = "point_retain";
  modules::CFieldDecl field;
  field.name = "x";
  field.type.kind = modules::CTypeRef::Kind::scalar;
  field.type.scalar = "i32";
  c_type.fields.push_back(field);
  field.name = "data";
  field.offset = 8;
  field.type.kind = modules::CTypeRef::Kind::pointer;
  field.type.writable = true;
  modules::CTypeRef named;
  named.kind = modules::CTypeRef::Kind::named;
  named.named = 1;
  field.type.args.push_back(named);
  c_type.fields.push_back(field);
  c_type.result.kind = modules::CTypeRef::Kind::pointer;
  c_type.result.args.push_back(named);
  c_type.parameters.push_back(modules::CParameterDecl{"value", named, zero});
  c_type.registration = modules::CRegistrationDecl{1, "point_unregister"};
  f.project->c_types.push_back(std::move(c_type));

  modules::CCallableDecl c_callable;
  c_callable.id = 1;
  c_callable.bundle = 0;
  c_callable.identity = "c:point_new";
  c_callable.module = "math";
  c_callable.name = "point_new";
  c_callable.symbol = "point_new";
  c_callable.parameters.push_back(modules::CParameterDecl{"value", named, zero});
  c_callable.result.type.kind = modules::CTypeRef::Kind::pointer;
  c_callable.result.type.args.push_back(named);
  c_callable.result.ownership = "owned";
  c_callable.result.release_symbol = "point_release";
  c_callable.result.retain_symbol = "point_retain";
  c_callable.stable_identity = "stable:c:point_new";
  c_callable.registers = modules::CRegistrationCall{0, 1, 2};
  f.project->c_callables.push_back(std::move(c_callable));

  f.resolved = std::make_shared<modules::ResolvedProject>();
  f.resolved->project = f.project;
  modules::Symbol main_symbol;
  main_symbol.id = SymbolId{0};
  main_symbol.name = "main";
  main_symbol.kind = modules::SymbolKind::function;
  main_symbol.module = ModuleId{0};
  main_symbol.definition = modules::NodeRef{ModuleId{0}, 0, AstNodeId{0}};
  main_symbol.declaration = zero;
  main_symbol.exported = true;
  f.resolved->symbols.push_back(main_symbol);
  modules::Symbol vec_symbol;
  vec_symbol.id = SymbolId{1};
  vec_symbol.name = "Vec";
  vec_symbol.kind = modules::SymbolKind::type;
  vec_symbol.module = ModuleId{0};
  vec_symbol.exported = true;
  vec_symbol.target = ModuleId{0};
  vec_symbol.foreign =
      modules::ForeignExportRef{ModuleId{0}, 0};
  f.resolved->symbols.push_back(vec_symbol);
  f.resolved->occurrences.push_back(modules::Occurrence{
      modules::NodeRef{ModuleId{0}, 0, AstNodeId{1}}, SymbolId{0}, zero});
  f.resolved->occurrences.push_back(modules::Occurrence{
      modules::NodeRef{ModuleId{0}, 1, AstNodeId{2}}, SymbolId{1}, zero});
  f.resolved->deferred_members.push_back(
      modules::NodeRef{ModuleId{0}, 0, AstNodeId{3}});
  f.resolved->self_parameters.push_back(
      {modules::NodeRef{ModuleId{0}, 0, AstNodeId{4}}, SymbolId{1}});

  auto &m = f.module;
  m.resolved = f.resolved;
  m.identity = ModuleId{3};
  m.config = Config{Backend::native, 64};
  Type t;
  t.kind = TypeKind::Unit;
  f.ids.unit = m.types.intern(t);
  t.kind = TypeKind::Bool;
  f.ids.boolean = m.types.intern(t);
  t.kind = TypeKind::SignedInt;
  t.bits = 32;
  f.ids.i32 = m.types.intern(t);
  t.kind = TypeKind::UnsignedInt;
  t.bits = 32;
  f.ids.u32 = m.types.intern(t);
  t.kind = TypeKind::SignedInt;
  t.bits = 64;
  f.ids.i64 = m.types.intern(t);
  t.kind = TypeKind::Float;
  t.bits = 64;
  f.ids.f64 = m.types.intern(t);
  t = Type{};
  t.kind = TypeKind::String;
  f.ids.string = m.types.intern(t);
  t = Type{};
  t.kind = TypeKind::Usize;
  f.ids.usize_id = m.types.intern(t);
  t = Type{};
  t.kind = TypeKind::Vec;
  t.type_args = types({f.ids.i32});
  f.ids.vec_i32 = m.types.intern(t);
  t = Type{};
  t.kind = TypeKind::Slice;
  t.type_args = types({f.ids.i32});
  f.ids.slice_i32 = m.types.intern(t);
  t = Type{};
  t.kind = TypeKind::Weak;
  t.type_args = types({f.ids.i32});
  f.ids.weak_i32 = m.types.intern(t);
  t = Type{};
  t.kind = TypeKind::Function;
  t.result = f.ids.i32;
  t.parameters = {{ParamMode::ordinary, f.ids.i32},
                  {ParamMode::inout, f.ids.i32}};
  f.ids.signature = m.types.intern(t);
  t = Type{};
  t.kind = TypeKind::Function;
  t.result = f.ids.i64;
  t.parameters = {{ParamMode::move, f.ids.i32}};
  t.throws_type = f.ids.boolean;
  f.ids.throwing = m.types.intern(t);
  t = Type{};
  t.kind = TypeKind::NominalStruct;
  t.declaration = SymbolId{5};
  t.type_args = types({f.ids.i32, f.ids.boolean});
  f.ids.nominal = m.types.intern(t);
  t = Type{};
  t.kind = TypeKind::CForeignPointer;
  t.type_args = types({f.ids.i32});
  t.bits = 1;
  f.ids.c_pointer = m.types.intern(t);
  t = Type{};
  t.kind = TypeKind::CForeignRecord;
  t.c_type = 1;
  f.ids.c_record = m.types.intern(t);
  const auto &ids = f.ids;

  auto src0 = ast_source(old_file, 0, 0);
  auto src1 = ast_source(old_file, 0, 1);
  auto src2 = ast_source(new_file, 1, 2);
  auto scope = [&](SymbolId function, std::optional<ScopeId> parent,
                   Source source) {
    ScopeId id{m.scopes.size()};
    m.scopes.push_back({function, parent, source});
    return id;
  };
  const ScopeId scope0 = scope(SymbolId{0}, std::nullopt, src0);
  const ScopeId scope1 =
      scope(SymbolId{1}, std::optional<ScopeId>{scope0}, src1);
  const ScopeId scope2 =
      scope(SymbolId{1}, std::optional<ScopeId>{scope1}, src2);
  auto cleanup = [](CleanupKind kind, OwnerKind owner, uint64_t identity,
                    TypeId type) {
    return CleanupStep{kind, OwnerRef{owner, identity}, type};
  };
  std::vector<CleanupStep> actions_normal;
  actions_normal.push_back(
      cleanup(CleanupKind::save_output, OwnerKind::return_output, 11,
              ids.i32));
  actions_normal.push_back(
      cleanup(CleanupKind::end_borrow, OwnerKind::binding, 12, ids.vec_i32));
  std::vector<ExitPlan> exit_normal;
  ExitPlan normal_plan;
  normal_plan.kind = ExitKind::normal;
  normal_plan.target = ScopeExit{std::optional<ScopeId>{scope0}};
  normal_plan.exited_scopes = {scope0, scope1};
  normal_plan.defers = {HirNodeId{2}};
  normal_plan.actions = actions_normal;
  exit_normal.push_back(normal_plan);
  std::vector<ExitPlan> exit_return;
  ExitPlan return_plan;
  return_plan.kind = ExitKind::return_;
  return_plan.target = FunctionExit{SymbolId{1}};
  return_plan.exited_scopes = {scope1};
  return_plan.saves_value = true;
  return_plan.actions.push_back(
      cleanup(CleanupKind::destroy_if_live, OwnerKind::temporary, 13,
              ids.i64));
  exit_return.push_back(return_plan);
  std::vector<ExitPlan> exit_break;
  ExitPlan break_plan;
  break_plan.kind = ExitKind::break_;
  break_plan.target = JumpExit{HirNodeId{21}};
  break_plan.exited_scopes = {scope2};
  break_plan.defers = {HirNodeId{3}};
  break_plan.actions.push_back(
      cleanup(CleanupKind::defer_, OwnerKind::iteration, 14, ids.i32));
  exit_break.push_back(break_plan);
  std::vector<ExitPlan> exit_error;
  ExitPlan error_plan;
  error_plan.kind = ExitKind::error;
  error_plan.error_type = ids.i32;
  error_plan.target = CatchTarget{HirNodeId{30}, 0};
  error_plan.exited_scopes = {scope0};
  error_plan.actions.push_back(cleanup(CleanupKind::end_iteration,
                                       OwnerKind::error_output, 15,
                                       ids.boolean));
  exit_error.push_back(error_plan);

  auto pattern = [](TypeId type, Source source,
                    std::optional<SymbolId> variant,
                    std::optional<SymbolId> binding,
                    std::vector<Pattern> payload,
                    std::optional<PatternLiteral> literal) {
    return Pattern{type, source, variant, binding, std::move(payload),
                   std::move(literal)};
  };
  auto constant = [](TypeId type, auto data) {
    ConstantValue value;
    value.type = type;
    value.data = data;
    return value;
  };
  auto call_arg = [](uint32_t source_index, uint32_t parameter_index,
                     std::optional<std::string> label, ParamMode mode,
                     HirNodeId expression) {
    return CallArg{source_index, parameter_index, std::move(label), mode,
                   expression};
  };

  std::vector<CaptureSource> captures;
  captures.push_back(CaptureSource{SymbolId{21}, SymbolId{22}, ids.i32,
                                   false});
  captures.push_back(CaptureSource{SymbolId{23}, SymbolId{24}, ids.vec_i32,
                                   true});
  std::vector<TypeId> type_args = types({ids.i32, ids.boolean});

  add_node(m, ids.unit, src0, scope0, {}, UnitLiteral{}, {}, {});
  add_node(m, ids.boolean, src0, scope0, {}, BoolLiteral{true}, {}, {});
  add_node(m, ids.i32, src0, scope0, {},
           IntegerLiteral{"18446744073709551615", UINT64_MAX}, {}, {});
  add_node(m, ids.f64, src0, scope0, {}, FloatLiteral{4607182418800017408},
           {}, {});
  add_node(m, ids.string, src0, scope0, {},
           StringLiteral{"h\xC3\xA9llo\xF0\x9F\x98\x80"}, {}, {});
  add_node(m, ids.i32, src0, scope0, {}, Read{SymbolId{7}}, {}, {});
  add_node(m, ids.i32, src0, scope0, {}, Place{SymbolId{8}}, {}, exit_normal);
  FunctionValue function_value;
  function_value.symbol = SymbolId{9};
  function_value.type_args = type_args;
  function_value.captures = captures;
  function_value.move_capture = true;
  function_value.bound =
      BoundReceiver{ids.i32, ParamMode::inout};
  add_node(m, ids.signature, src0, scope0, {HirNodeId{1}, HirNodeId{4}},
           function_value, {}, {});
  add_node(m, ids.unit, src0, scope0, {HirNodeId{2}},
           Binding{SymbolId{9}, true}, {}, {});
  add_node(m, ids.unit, src0, scope0, {HirNodeId{2}, HirNodeId{3}},
           Assign{std::optional<BinaryOp>{BinaryOp::add}}, {}, {});
  add_node(m, ids.i32, src0, scope0, {HirNodeId{2}},
           Unary{UnaryOp::negate}, {}, {});
  add_node(m, ids.i32, src0, scope0, {HirNodeId{2}, HirNodeId{3}},
           Binary{BinaryOp::multiply}, {}, {});
  add_node(m, ids.boolean, src0, scope0, {HirNodeId{1}, HirNodeId{12}},
           ShortCircuit{LogicalOp::and_}, {}, {});
  add_node(m, ids.i32, src0, scope0, {HirNodeId{2}}, IntegerConvert{}, {},
           {});
  std::vector<CallArg> call_args;
  call_args.push_back(call_arg(0, 1, "b", ParamMode::inout,
                               HirNodeId{2}));
  call_args.push_back(
      call_arg(1, 0, std::nullopt, ParamMode::move, HirNodeId{3}));
  Call call;
  call.kind = CalleeKind::c_foreign;
  call.signature = ids.signature;
  call.direct_symbol = SymbolId{0};
  call.callee = HirNodeId{1};
  call.args = call_args;
  call.type_args = type_args;
  call.binding = ForeignBindingId{0};
  call.c_callable = uint32_t{0};
  call.omitted = {0, 2};
  call.system = static_cast<SystemOperation>(0);
  add_node(m, ids.i32, src0, scope0,
           {HirNodeId{1}, HirNodeId{2}, HirNodeId{3}}, call,
           {ids.boolean, ids.i32}, {});
  add_node(m, ids.unit, src0, scope0, {HirNodeId{0}, HirNodeId{1}}, Block{},
           {}, {});
  add_node(m, ids.unit, src0, scope0, {HirNodeId{14}},
           ExpressionStatement{}, {}, {});
  add_node(m, ids.unit, src0, scope0, {HirNodeId{1}, HirNodeId{15}}, If{},
           {}, {});
  add_node(m, ids.unit, src0, scope0,
           {HirNodeId{8}, HirNodeId{1}, HirNodeId{9}, HirNodeId{15}}, For{},
           {}, {});
  add_node(m, ids.unit, src0, scope0, {}, Noop{}, {}, {});
  add_node(m, ids.unit, src0, scope0, {HirNodeId{2}},
           Return{SymbolId{1}}, {}, exit_return);
  add_node(m, ids.unit, src0, scope0, {}, Break{HirNodeId{45}}, {},
           exit_break);
  add_node(m, ids.unit, src0, scope0, {}, Continue{HirNodeId{46}}, {}, {});
  add_node(m, ids.unit, src0, scope0, {},
           Unsupported{"async cancellation"}, {}, {});
  std::vector<CallArg> struct_fields;
  struct_fields.push_back(call_arg(0, 0, "x", ParamMode::ordinary,
                                   HirNodeId{2}));
  struct_fields.push_back(
      call_arg(1, 1, std::nullopt, ParamMode::inout, HirNodeId{3}));
  add_node(m, ids.nominal, src0, scope0, {HirNodeId{2}, HirNodeId{3}},
           StructInit{struct_fields}, {}, {});
  add_node(m, ids.nominal, src0, scope0, {HirNodeId{2}, HirNodeId{3}},
           EnumInit{SymbolId{0}, struct_fields}, {}, {});
  add_node(m, ids.i32, src0, scope0, {HirNodeId{2}},
           FieldRead{SymbolId{0}}, {}, {});
  add_node(m, ids.i32, src0, scope0, {HirNodeId{2}},
           FieldPlace{SymbolId{1}}, {}, {});
  std::vector<Pattern> inner_payload;
  inner_payload.push_back(pattern(
      ids.i32, src1, std::optional<SymbolId>{SymbolId{2}}, std::nullopt, {},
      PatternLiteral{IntegerLiteral{"0", 0}}));
  std::vector<Pattern> outer_payload;
  outer_payload.push_back(pattern(
      ids.vec_i32, src1, std::optional<SymbolId>{SymbolId{1}},
      std::optional<SymbolId>{SymbolId{0}}, inner_payload, std::nullopt));
  Switch switch_node;
  switch_node.arms.push_back(
      Arm{pattern(ids.vec_i32, src1, std::optional<SymbolId>{SymbolId{0}},
                  std::optional<SymbolId>{SymbolId{0}}, outer_payload,
                  std::nullopt),
          scope1, HirNodeId{15}, src1});
  switch_node.arms.push_back(
      Arm{pattern(ids.boolean, src2, std::nullopt,
                  std::optional<SymbolId>{SymbolId{0}}, {},
                  PatternLiteral{BoolLiteral{false}}),
          scope2, HirNodeId{16}, src2});
  switch_node.exhaustive = true;
  add_node(m, ids.unit, src1, scope1, {HirNodeId{1}}, switch_node, {}, {});
  TraitRef trait_ref;
  trait_ref.declaration = SymbolId{9};
  trait_ref.args = type_args;
  add_node(m, ids.i32, src1, scope1, {HirNodeId{14}},
           ConstraintCall{trait_ref, SymbolId{10}, ids.i32, ids.signature,
                          call_args},
           {}, {});
  Try try_node;
  try_node.catches.push_back(Catch{ids.i32, SymbolId{12}, scope1,
                                   HirNodeId{15}, src1});
  try_node.catches.push_back(Catch{ids.string, SymbolId{13}, scope2,
                                   HirNodeId{16}, src2});
  add_node(m, ids.unit, src1, scope1, {HirNodeId{14}}, try_node, {},
           exit_error);
  add_node(m, ids.unit, src1, scope1, {HirNodeId{2}},
           Defer{scope2, {SymbolId{3}, SymbolId{4}, SymbolId{5}}}, {}, {});
  add_node(m, ids.unit, src1, scope1, {HirNodeId{2}}, Throw{}, {}, {});
  add_node(m, ids.i32, src1, scope1, {HirNodeId{8}},
           Move{std::optional<SymbolId>{SymbolId{0}}}, {}, {});
  add_node(m, ids.vec_i32, src1, scope1, {HirNodeId{2}, HirNodeId{3}},
           VecInit{}, {}, {});
  add_node(m, ids.i32, src1, scope1, {HirNodeId{2}, HirNodeId{3}},
           IndexRead{}, {}, {});
  add_node(m, ids.i32, src1, scope1, {HirNodeId{2}, HirNodeId{3}},
           IndexPlace{}, {}, {});
  add_node(m, ids.slice_i32, src1, scope1, {HirNodeId{2}},
           VecSlice{std::optional<HirNodeId>{HirNodeId{0}}, std::nullopt}, {},
           {});
  IteratorFor iterator;
  iterator.cursor = SymbolId{20};
  iterator.option = SymbolId{21};
  iterator.next = HirNodeId{22};
  iterator.iterator = ids.vec_i32;
  iterator.element = ids.i32;
  iterator.some = SymbolId{25};
  iterator.none = SymbolId{26};
  iterator.some_field = SymbolId{27};
  add_node(m, ids.unit, src1, scope1, {HirNodeId{2}, HirNodeId{15}},
           ForIn{SymbolId{12}, scope2, HirNodeId{15}, iterator}, {}, {});
  add_node(m, ids.i32, src1, scope1, {HirNodeId{2}, HirNodeId{3}},
           ForeignRead{ForeignBindingId{3},
                       ForeignAccessKind::slice_place,
                       std::optional<HirNodeId>{HirNodeId{0}},
                       std::optional<HirNodeId>{HirNodeId{2}}},
           {}, {});
  std::vector<ConstantValue> aggregate_fields;
  aggregate_fields.push_back(
      constant(ids.i32, IntegerLiteral{"-0", 0}));
  std::vector<ConstantValue> inner_fields;
  inner_fields.push_back(constant(ids.boolean, BoolLiteral{false}));
  inner_fields.push_back(
      constant(ids.string, StringLiteral{std::string("nested\0value", 12)}));
  ConstantAggregate inner_aggregate;
  inner_aggregate.variant = SymbolId{0};
  inner_aggregate.fields = inner_fields;
  aggregate_fields.push_back(
      constant(ids.nominal, inner_aggregate));
  FunctionValue constant_function;
  constant_function.symbol = SymbolId{33};
  constant_function.type_args = types({ids.i32});
  constant_function.captures.push_back(
      CaptureSource{SymbolId{31}, SymbolId{32}, ids.i32, true});
  constant_function.bound = BoundReceiver{ids.i64, ParamMode::move};
  aggregate_fields.push_back(
      constant(ids.signature, constant_function));
  ConstantAggregate aggregate;
  aggregate.variant = SymbolId{0};
  aggregate.fields = aggregate_fields;
  add_node(m, ids.i32, src2, scope2, {},
           ConstantLiteral{constant(ids.nominal, aggregate)}, {}, {});
  add_node(m, ids.i32, src2, scope2, {HirNodeId{2}},
           CFieldRead{0, 0}, {}, {});
  add_node(m, ids.i32, src2, scope2, {HirNodeId{2}}, Await{}, {}, {});
  std::vector<std::vector<TypeId>> method_args;
  method_args.push_back(types({ids.i32}));
  method_args.push_back(types({ids.i32, ids.boolean}));
  method_args.push_back({});
  add_node(m, ids.nominal, src2, scope2, {HirNodeId{2}},
           TraitObjectInit{trait_ref, ids.nominal,
                           {SymbolId{21}, SymbolId{22}}, method_args},
           {}, {});
  add_node(m, ids.i32, src2, scope2, {HirNodeId{2}, HirNodeId{3}},
           TraitDispatch{trait_ref, SymbolId{14}, ids.signature, 0,
                         call_args},
           {}, {});

  std::vector<Parameter> parameters{{SymbolId{0}, "a"}, {SymbolId{1}, "b"}};
  HirFunction add_function;
  add_function.symbol = SymbolId{0};
  add_function.signature = ids.signature;
  add_function.parameters = parameters;
  add_function.body = HirNodeId{15};
  add_function.scope = scope0;
  add_function.source = ast_source(old_file, 0, 5);
  add_function.captures = {CaptureBinding{SymbolId{20}, SymbolId{21},
                                           ids.i32},
                           CaptureBinding{SymbolId{22}, SymbolId{23},
                                           ids.vec_i32}};
  add_function.async = true;
  m.functions.push_back(add_function);
  HirFunction main_function;
  main_function.symbol = SymbolId{1};
  main_function.signature = ids.throwing;
  main_function.body = HirNodeId{20};
  main_function.scope = scope2;
  main_function.source = ast_source(new_file, 1, 6);
  m.functions.push_back(main_function);

  auto generic_param = [&](SymbolId symbol, std::optional<TraitRef> trait,
                           Source source) {
    return GenericParam{symbol, std::move(trait), source};
  };
  std::vector<GenericParam> generic_params;
  generic_params.push_back(
      generic_param(SymbolId{2}, trait_ref, ast_source(old_file, 0, 10)));
  generic_params.push_back(
      generic_param(SymbolId{3}, std::nullopt, ast_source(old_file, 0, 11)));
  std::vector<FieldDecl> fields;
  fields.push_back(FieldDecl{SymbolId{4}, ids.i32, true,
                             ast_source(old_file, 0, 12)});
  fields.push_back(FieldDecl{SymbolId{5}, ids.string, false,
                             ast_source(old_file, 0, 13)});
  NominalDecl nominal;
  nominal.symbol = SymbolId{8};
  nominal.kind = TypeKind::NominalEnum;
  nominal.params = generic_params;
  nominal.fields = fields;
  nominal.variants.push_back(
      VariantDecl{SymbolId{6}, fields, ast_source(old_file, 0, 14)});
  nominal.variants.push_back(
      VariantDecl{SymbolId{7}, {}, ast_source(old_file, 0, 15)});
  nominal.source = ast_source(old_file, 0, 16);
  m.nominals.push_back(nominal);
  TraitDecl trait_decl;
  trait_decl.symbol = SymbolId{11};
  trait_decl.self = SymbolId{0};
  trait_decl.params = generic_params;
  trait_decl.methods.push_back(
      MethodPrototype{SymbolId{9}, ids.signature, parameters,
                      ast_source(old_file, 0, 17)});
  trait_decl.methods.push_back(
      MethodPrototype{SymbolId{10}, ids.throwing, {},
                      ast_source(old_file, 0, 18)});
  trait_decl.source = ast_source(old_file, 0, 19);
  m.traits.push_back(trait_decl);
  ImplDecl impl;
  impl.params = generic_params;
  impl.trait = trait_ref;
  impl.self_parameter = SymbolId{12};
  impl.self_type = ids.nominal;
  impl.methods = {SymbolId{9}, SymbolId{10}};
  impl.source = ast_source(old_file, 0, 20);
  m.impls.push_back(impl);
  FunctionTemplate function_template;
  function_template.function = add_function;
  function_template.params = generic_params;
  m.templates.push_back(function_template);
  FunctionInstance instance;
  instance.symbol = SymbolId{13};
  instance.origin = SymbolId{0};
  instance.type_args = type_args;
  instance.renamed = {{SymbolId{0}, SymbolId{100}},
                      {SymbolId{1}, SymbolId{101}}};
  instance.target = Config{Backend::python, 64};
  m.instances.push_back(instance);
  m.ownership.uses.push_back(
      ValueUse{HirNodeId{2}, 0, UseKind::borrow, SymbolId{0}, ids.i32,
               SymbolId{0}});
  m.ownership.uses.push_back(ValueUse{HirNodeId{3}, 1, UseKind::copy,
                                      std::nullopt, ids.vec_i32,
                                      std::nullopt});
  m.ownership.uses.push_back(ValueUse{HirNodeId{4}, 2, UseKind::take,
                                      SymbolId{2}, ids.i32, SymbolId{1}});
  m.ownership.uses.push_back(ValueUse{HirNodeId{5}, 3, UseKind::inout,
                                      SymbolId{3}, ids.i64, std::nullopt});
  m.ownership.temporaries.push_back(
      Temporary{HirNodeId{2}, HirNodeId{8}, ids.i32});
  m.ownership.temporaries.push_back(
      Temporary{HirNodeId{3}, HirNodeId{9}, ids.vec_i32});
  m.ownership.locals.push_back(LocalLifetime{
      SymbolId{0}, scope0, ids.i32, 0, ParamMode::ordinary, false});
  m.ownership.locals.push_back(LocalLifetime{
      SymbolId{1}, scope1, ids.vec_i32, 1, ParamMode::inout, true});
  m.ownership.full_expressions.push_back(FullExpression{
      HirNodeId{14}, {cleanup(CleanupKind::save_output, OwnerKind::binding,
                              40, ids.i32),
                      cleanup(CleanupKind::end_borrow, OwnerKind::temporary,
                              41, ids.string)}});
  m.ownership.full_expressions.push_back(FullExpression{
      HirNodeId{20}, {cleanup(CleanupKind::save_output, OwnerKind::binding,
                              40, ids.i32)}});
  m.ownership.calls.push_back(CallLifetime{
      HirNodeId{14}, {cleanup(CleanupKind::destroy_if_live,
                              OwnerKind::error_output, 42, ids.i64),
                      cleanup(CleanupKind::end_iteration,
                              OwnerKind::iteration, 43, ids.boolean)}});
  m.foreign_bindings.push_back(ForeignBinding{
      ForeignBindingId{0}, ids.c_pointer, std::optional<TypeId>{ids.signature},
      std::optional<TypeId>{TypeId{1}}, ids.i32});
  m.foreign_bindings.push_back(ForeignBinding{
      ForeignBindingId{1}, ids.c_record, std::nullopt, std::nullopt,
      ids.i32});
  return f;
}

AstFile &mutable_file(Working &w, size_t index) {
  return *std::const_pointer_cast<AstFile>(
      w.project->modules.at(0).files.at(index));
}

bool is_wz_only(int id) {
  switch (id) {
  case 0:
  case 9:
  case 10:
  case 11:
  case 12:
  case 15:
  case 16:
  case 17:
  case 18:
  case 21:
  case 22:
  case 119:
  case 120:
  case 121:
  case 122:
    return true;
  default:
    return false;
  }
}

const char *const kProbeNames[] = {
    "project.files",
    "project.modules",
    "project.bindings",
    "project.foreign_bindings",
    "project.c_bindings",
    "project.c_types",
    "project.c_callables",
    "project.c_libraries",
    "project.c_library_dirs",
    "source.original",
    "source.normalized",
    "source.offsets",
    "source.boundaries",
    "lexed.tokens",
    "lexed.tokens.trivia",
    "lexed.diagnostics",
    "lexed.diagnostics.related",
    "lexed.diagnostics.fixits",
    "lexed.diagnostics.notes",
    "parse.nodes",
    "parse.nodes.children",
    "parse.diagnostics",
    "parse.diagnostics.related",
    "module.files",
    "module.imports",
    "module.foreign_exports",
    "module.foreign_exports.candidates",
    "module.foreign_exports.c_type_ref.args",
    "binding.allowed_backends",
    "binding.package_requirements",
    "binding.modules",
    "binding.declarations",
    "binding.conformance_required",
    "foreign.host_attribute",
    "foreign.type.members",
    "foreign.type.properties",
    "foreign.type.indexers",
    "foreign.type.slices",
    "foreign.callable.parameters",
    "foreign.callable.effects",
    "foreign.property.getter",
    "foreign.property.setter",
    "c_binding.modules",
    "c_binding.types",
    "c_binding.callables",
    "c_type.fields",
    "c_type.field.type.args",
    "c_type.result.args",
    "c_type.parameters",
    "c_callable.parameters",
    "c_callable.result.type.args",
    "resolved.symbols",
    "resolved.occurrences",
    "resolved.deferred_members",
    "resolved.self_parameters",
    "types.entries",
    "functions",
    "functions.parameters",
    "functions.captures",
    "nodes",
    "nodes.children",
    "nodes.errors",
    "nodes.exits",
    "nodes.exits.exited_scopes",
    "nodes.exits.defers",
    "nodes.exits.actions",
    "function_value.captures",
    "function_value.type_args",
    "call.args",
    "call.type_args",
    "call.omitted",
    "struct_init.fields",
    "enum_init.payload",
    "switch.arms",
    "pattern.payload",
    "pattern.payload.nested",
    "constraint_call.trait_ref.args",
    "constraint_call.args",
    "try.catches",
    "defer.references",
    "constant.aggregate.fields",
    "constant.aggregate.nested_fields",
    "trait_object.methods",
    "trait_object.method_args",
    "trait_object.method_args.nested",
    "trait_dispatch.trait_ref.args",
    "trait_dispatch.args",
    "scopes",
    "nominals",
    "nominals.params",
    "nominals.params.trait_ref.args",
    "nominals.fields",
    "nominals.variants",
    "nominals.variants.payload",
    "traits",
    "traits.params",
    "traits.methods",
    "traits.methods.parameters",
    "impls",
    "impls.params",
    "impls.trait_ref.args",
    "impls.methods",
    "templates",
    "templates.function.parameters",
    "templates.function.captures",
    "templates.params",
    "instances",
    "instances.type_args",
    "instances.renamed",
    "ownership.uses",
    "ownership.temporaries",
    "ownership.locals",
    "ownership.full_expressions",
    "ownership.full_expressions.actions",
    "ownership.calls",
    "ownership.calls.on_return",
    "hir.foreign_bindings",
    "constant.function.captures",
    "constant.function.type_args",
    "entry.clone_node",
    "entry.clone_function",
    "entry.clone_pattern",
    "entry.clone_constant",
};

void mutate(Working &w, int id) {
  auto &m = w.module;
  auto &project = *w.project;
  switch (id) {
  case 1:
    grow(project.modules);
    break;
  case 2:
    grow(project.bindings);
    break;
  case 3:
    grow(project.foreign_bindings);
    break;
  case 4:
    grow(project.c_bindings);
    break;
  case 5:
    grow(project.c_types);
    break;
  case 6:
    grow(project.c_callables);
    break;
  case 7:
    project.c_libraries.push_back("mut");
    break;
  case 8:
    project.c_library_dirs.push_back("/mut");
    break;
  case 13: {
    auto &file = mutable_file(w, 0);
    auto token = file.tokens.at(0);
    file.tokens.push_back(token);
    break;
  }
  case 14: {
    auto &file = mutable_file(w, 0);
    SourceSpan span = file.tokens.at(0).span;
    file.tokens.at(0).leading_trivia.push_back(
        Trivia{TriviaKind::line_comment, span, "//mut"});
    break;
  }
  case 19: {
    auto &file = mutable_file(w, 0);
    auto node = file.nodes.at(0);
    file.nodes.push_back(node);
    break;
  }
  case 20: {
    auto &file = mutable_file(w, 0);
    file.nodes.at(0).children.push_back(AstNodeId{0});
    break;
  }
  case 23:
    grow(project.modules.at(0).files);
    break;
  case 24:
    grow(project.modules.at(0).imports);
    break;
  case 25:
    grow(project.modules.at(0).foreign_exports);
    break;
  case 26:
    project.modules.at(0).foreign_exports.at(0).candidates.push_back(
        ForeignBindingId{2});
    break;
  case 27:
    project.modules.at(0)
        .foreign_exports.at(0)
        .c_type_ref->args.push_back(
            project.modules.at(0).foreign_exports.at(0).c_type_ref->args.at(0));
    break;
  case 28:
    project.bindings.at(0).allowed_backends.push_back("mut");
    break;
  case 29:
    grow(project.bindings.at(0).package_requirements);
    break;
  case 30:
    grow(project.bindings.at(0).modules);
    break;
  case 31:
    project.bindings.at(0).declarations.push_back(ForeignBindingId{9});
    break;
  case 32:
    project.bindings.at(0).conformance_required.push_back("mut");
    break;
  case 33:
    project.foreign_bindings.at(0).host_attribute.push_back("mut");
    break;
  case 34:
    std::get<modules::BindingType>(project.foreign_bindings.at(0).detail)
        .members.push_back(ForeignBindingId{9});
    break;
  case 35:
    std::get<modules::BindingType>(project.foreign_bindings.at(0).detail)
        .properties.push_back(ForeignBindingId{9});
    break;
  case 36:
    std::get<modules::BindingType>(project.foreign_bindings.at(0).detail)
        .indexers.push_back(ForeignBindingId{9});
    break;
  case 37:
    std::get<modules::BindingType>(project.foreign_bindings.at(0).detail)
        .slices.push_back(ForeignBindingId{9});
    break;
  case 38:
    grow(std::get<modules::BindingCallable>(
             project.foreign_bindings.at(2).detail)
             .parameters);
    break;
  case 39:
    std::get<modules::BindingCallable>(
        project.foreign_bindings.at(2).detail)
        .effects.push_back("mut");
    break;
  case 40:
    std::get<modules::BindingProperty>(
        project.foreign_bindings.at(3).detail)
        .getter.push_back("mut");
    break;
  case 41:
    std::get<modules::BindingProperty>(
        project.foreign_bindings.at(3).detail)
        .setter.push_back("mut");
    break;
  case 42:
    grow(project.c_bindings.at(0).modules);
    break;
  case 43:
    project.c_bindings.at(0).types.push_back(2);
    break;
  case 44:
    project.c_bindings.at(0).callables.push_back(2);
    break;
  case 45:
    grow(project.c_types.at(0).fields);
    break;
  case 46:
    project.c_types.at(0).fields.at(1).type.args.push_back(
        project.c_types.at(0).fields.at(1).type.args.at(0));
    break;
  case 47:
    project.c_types.at(0).result.args.push_back(
        project.c_types.at(0).result.args.at(0));
    break;
  case 48:
    grow(project.c_types.at(0).parameters);
    break;
  case 49:
    grow(project.c_callables.at(0).parameters);
    break;
  case 50:
    project.c_callables.at(0).result.type.args.push_back(
        project.c_callables.at(0).result.type.args.at(0));
    break;
  case 51:
    grow(w.resolved->symbols);
    break;
  case 52:
    grow(w.resolved->occurrences);
    break;
  case 53:
    grow(w.resolved->deferred_members);
    break;
  case 54:
    grow(w.resolved->self_parameters);
    break;
  case 55: {
    Type fresh;
    fresh.kind = TypeKind::SignedInt;
    fresh.bits = 8;
    m.types.intern(fresh);
    break;
  }
  case 56:
    grow(m.functions);
    break;
  case 57:
    grow(m.functions.at(0).parameters);
    break;
  case 58:
    grow(m.functions.at(0).captures);
    break;
  case 59:
    grow(m.nodes);
    break;
  case 60:
    m.nodes.at(0).children.push_back(HirNodeId{0});
    break;
  case 61:
    m.nodes.at(14).errors.push_back(TypeId{2});
    break;
  case 62:
    grow(m.nodes.at(6).exits);
    break;
  case 63:
    m.nodes.at(6).exits.at(0).exited_scopes.push_back(ScopeId{5});
    break;
  case 64:
    m.nodes.at(6).exits.at(0).defers.push_back(HirNodeId{5});
    break;
  case 65:
    m.nodes.at(6).exits.at(0).actions.push_back(
        m.nodes.at(6).exits.at(0).actions.at(0));
    break;
  case 66:
    grow(std::get<FunctionValue>(m.nodes.at(7).data).captures);
    break;
  case 67:
    grow(std::get<FunctionValue>(m.nodes.at(7).data).type_args);
    break;
  case 68:
    grow(std::get<Call>(m.nodes.at(14).data).args);
    break;
  case 69:
    grow(std::get<Call>(m.nodes.at(14).data).type_args);
    break;
  case 70:
    std::get<Call>(m.nodes.at(14).data).omitted.push_back(9);
    break;
  case 71:
    grow(std::get<StructInit>(m.nodes.at(24).data).fields);
    break;
  case 72:
    grow(std::get<EnumInit>(m.nodes.at(25).data).payload);
    break;
  case 73:
    grow(std::get<Switch>(m.nodes.at(28).data).arms);
    break;
  case 74:
    grow(std::get<Switch>(m.nodes.at(28).data)
             .arms.at(0)
             .pattern.payload);
    break;
  case 75:
    grow(std::get<Switch>(m.nodes.at(28).data)
             .arms.at(0)
             .pattern.payload.at(0)
             .payload);
    break;
  case 76:
    grow(std::get<ConstraintCall>(m.nodes.at(29).data).trait.args);
    break;
  case 77:
    grow(std::get<ConstraintCall>(m.nodes.at(29).data).args);
    break;
  case 78:
    grow(std::get<Try>(m.nodes.at(30).data).catches);
    break;
  case 79:
    std::get<Defer>(m.nodes.at(31).data).references.push_back(SymbolId{9});
    break;
  case 80:
    grow(std::get<ConstantAggregate>(
             std::get<ConstantLiteral>(m.nodes.at(40).data).value.data)
             .fields);
    break;
  case 81:
    grow(std::get<ConstantAggregate>(
             std::get<ConstantAggregate>(
                 std::get<ConstantLiteral>(m.nodes.at(40).data).value.data)
                 .fields.at(1)
                 .data)
             .fields);
    break;
  case 82:
    std::get<TraitObjectInit>(m.nodes.at(43).data)
        .methods.push_back(SymbolId{21});
    break;
  case 83:
    grow(std::get<TraitObjectInit>(m.nodes.at(43).data).method_args);
    break;
  case 84:
    std::get<TraitObjectInit>(m.nodes.at(43).data)
        .method_args.at(0)
        .push_back(TypeId{7});
    break;
  case 85:
    grow(std::get<TraitDispatch>(m.nodes.at(44).data).trait.args);
    break;
  case 86:
    grow(std::get<TraitDispatch>(m.nodes.at(44).data).args);
    break;
  case 87:
    grow(m.scopes);
    break;
  case 88:
    grow(m.nominals);
    break;
  case 89:
    grow(m.nominals.at(0).params);
    break;
  case 90:
    m.nominals.at(0).params.at(0).trait->args.push_back(TypeId{3});
    break;
  case 91:
    grow(m.nominals.at(0).fields);
    break;
  case 92:
    grow(m.nominals.at(0).variants);
    break;
  case 93:
    grow(m.nominals.at(0).variants.at(0).payload);
    break;
  case 94:
    grow(m.traits);
    break;
  case 95:
    grow(m.traits.at(0).params);
    break;
  case 96:
    grow(m.traits.at(0).methods);
    break;
  case 97:
    grow(m.traits.at(0).methods.at(0).parameters);
    break;
  case 98:
    grow(m.impls);
    break;
  case 99:
    grow(m.impls.at(0).params);
    break;
  case 100:
    m.impls.at(0).trait->args.push_back(TypeId{3});
    break;
  case 101:
    m.impls.at(0).methods.push_back(SymbolId{9});
    break;
  case 102:
    grow(m.templates);
    break;
  case 103:
    grow(m.templates.at(0).function.parameters);
    break;
  case 104:
    grow(m.templates.at(0).function.captures);
    break;
  case 105:
    grow(m.templates.at(0).params);
    break;
  case 106:
    grow(m.instances);
    break;
  case 107:
    grow(m.instances.at(0).type_args);
    break;
  case 108:
    grow(m.instances.at(0).renamed);
    break;
  case 109:
    grow(m.ownership.uses);
    break;
  case 110:
    grow(m.ownership.temporaries);
    break;
  case 111:
    grow(m.ownership.locals);
    break;
  case 112:
    grow(m.ownership.full_expressions);
    break;
  case 113:
    grow(m.ownership.full_expressions.at(0).actions);
    break;
  case 114:
    grow(m.ownership.calls);
    break;
  case 115:
    grow(m.ownership.calls.at(0).on_return);
    break;
  case 116:
    grow(m.foreign_bindings);
    break;
  case 117:
    grow(std::get<FunctionValue>(
             std::get<ConstantAggregate>(
                 std::get<ConstantLiteral>(m.nodes.at(40).data).value.data)
                 .fields.at(2)
                 .data)
             .captures);
    break;
  case 118:
    grow(std::get<FunctionValue>(
             std::get<ConstantAggregate>(
                 std::get<ConstantLiteral>(m.nodes.at(40).data).value.data)
                 .fields.at(2)
                 .data)
             .type_args);
    break;
  default:
    project.c_libraries.push_back("unknown");
    break;
  }
}

bool option_some_zero(const HirModule &m) {
  const auto &call = std::get<Call>(m.nodes.at(14).data);
  if (!call.direct_symbol || call.direct_symbol->value != 0)
    return false;
  if (!call.binding || call.binding->value != 0)
    return false;
  if (!call.c_callable || *call.c_callable != 0)
    return false;
  if (!call.system || static_cast<int>(*call.system) != 0)
    return false;
  const auto &assignment = std::get<Assign>(m.nodes.at(9).data);
  if (!assignment.compound || static_cast<int>(*assignment.compound) != 0)
    return false;
  const auto &move = std::get<Move>(m.nodes.at(33).data);
  if (!move.binding || move.binding->value != 0)
    return false;
  const auto &slice = std::get<VecSlice>(m.nodes.at(37).data);
  if (!slice.start || slice.start->value != 0 || slice.end)
    return false;
  const auto &aggregate = std::get<ConstantAggregate>(
      std::get<ConstantLiteral>(m.nodes.at(40).data).value.data);
  if (!aggregate.variant || aggregate.variant->value != 0)
    return false;
  const auto &export_value = m.resolved->project->modules.at(0)
                                 .foreign_exports.at(0);
  if (!export_value.declaration || export_value.declaration->value != 0)
    return false;
  return true;
}

bool context_isolation(const Fixture &f) {
  Working copy = clone_working(f);
  const size_t before = copy.module.types.size();
  Type fresh;
  fresh.kind = TypeKind::SignedInt;
  fresh.bits = 8;
  copy.module.types.intern(fresh);
  if (copy.module.types.size() != before + 1)
    return false;
  if (f.module.types.size() != before)
    return false;
  const Type *original = f.module.types.find(TypeId{12});
  if (!original || original->kind != TypeKind::Function ||
      original->parameters.size() != 2)
    return false;
  // find() returns a pointer into the table; the C++ table is not copy-on-read.
  // Mutating the copy's table must not be visible in the original, which the
  // size check above already proves.
  return true;
}

bool empty_module_clone() {
  Fixture empty;
  empty.project = std::make_shared<modules::Project>();
  empty.resolved = std::make_shared<modules::ResolvedProject>();
  empty.resolved->project = empty.project;
  empty.module.resolved = empty.resolved;
  empty.module.config = Config{Backend::native, 64};
  Working copy = clone_working(empty);
  return dump(empty.module) == dump(copy.module) &&
         copy.module.types.size() == 0 && copy.module.nodes.empty() &&
         copy.module.functions.empty() &&
         copy.resolved->project->modules.empty();
}

} // namespace

int main() {
  try {
    Fixture fixture = make_fixture();
    std::cout << "CHECK fixture ok=1\n";
    std::cout << "hirclone-oracle-report v1\n";
    Working copy = clone_working(fixture);
    const bool fields_equal = fp(fixture) == fp(copy);
    std::cout << "CHECK clone_fields_equal ok=" << fields_equal << '\n';
    const bool some_zero = option_some_zero(fixture.module);
    std::cout << "CHECK option_some_zero ok=" << some_zero << '\n';
    const bool context_ok = context_isolation(fixture);
    std::cout << "CHECK context_isolation ok=" << context_ok << '\n';
    const bool empty_ok = empty_module_clone();
    std::cout << "CHECK empty_module_clone ok=" << empty_ok << '\n';
    int failures = 0;
    if (!fields_equal || !some_zero || !context_ok || !empty_ok)
      failures += 1;
    int probe_count = 0;
    for (int id = 0; id < 123; ++id) {
      ++probe_count;
      const char *name = kProbeNames[id];
      if (is_wz_only(id)) {
        std::cout << "PROBE " << name << " wz_only=1 cpp=none\n";
        continue;
      }
      Working left = clone_working(fixture);
      Working right = clone_working(fixture);
      const std::string left_before = fp(left);
      const std::string right_before = fp(right);
      mutate(left, id);
      const bool left_changed = fp(left) != left_before;
      const bool right_isolated = fp(right) == right_before;
      const std::string left_after = fp(left);
      mutate(right, id);
      const bool reverse_changed = fp(right) != right_before;
      const bool reverse_isolated = fp(left) == left_after;
      const bool ok = left_changed && right_isolated && reverse_changed &&
                      reverse_isolated;
      if (!ok)
        ++failures;
      std::cout << "PROBE " << name << " wz_only=0 left_changed="
                << left_changed << " right_isolated=" << right_isolated
                << " reverse_changed=" << reverse_changed
                << " reverse_isolated=" << reverse_isolated << '\n';
    }
    std::cout << "SUMMARY probes=" << probe_count << " failures=" << failures
              << " wz_only=15\n";
    return failures == 0 ? 0 : 1;
  } catch (const std::exception &error) {
    std::cerr << "oracle failure: " << error.what() << '\n';
    return 2;
  }
}
