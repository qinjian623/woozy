// Test-only C++ oracle for the self-hosted binding-bundle reader.
//
// It loads a generated project with src/modules/project.cpp (the frozen
// reference implementation) and prints the same length-prefixed token tree as
// selfhost/tests/bindings/src/main.wz. It links the already-built Root
// liblang_modules.a/liblang_frontend.a; it never rebuilds the C++ compiler and
// is never used by the Wz candidate.
#include "lang/modules/project.hpp"
#include "lang/support/source.hpp"
#include <cstdio>
#include <iostream>
#include <string>
#include <variant>
#include <vector>

using namespace lang;
using namespace lang::modules;

namespace {
std::string str(const std::string &value) {
  return "s" + std::to_string(value.size()) + ":" + value;
}
std::string num(uint64_t value) { return "n" + std::to_string(value); }
std::string boolean(bool value) { return value ? "t" : "f"; }
std::string absent() { return "z"; }
std::string present(bool yes, const std::string &value) {
  return yes ? value : absent();
}
std::string strings(const std::vector<std::string> &values) {
  std::string out = "[";
  for (const auto &value : values) out += str(value);
  return out + "]";
}
std::string ids(const std::vector<size_t> &values) {
  std::string out = "[";
  for (auto value : values) out += num(value);
  return out + "]";
}
std::string ids(const std::vector<uint32_t> &values) {
  std::string out = "[";
  for (auto value : values) out += num(value);
  return out + "]";
}
std::string ids(const std::vector<ForeignBindingId> &values) {
  std::string out = "[";
  for (const auto &value : values) out += num(value.value);
  return out + "]";
}

const SourceManager *sources = nullptr;

std::string span(const std::optional<SourceSpan> &value) {
  if (!value) return absent();
  auto snapshot = sources->get(value->snapshot);
  return "{" + str(snapshot->path) + num(value->begin_byte) +
         num(value->end_byte) + "}";
}
std::string span(const SourceSpan &value) {
  return span(std::optional<SourceSpan>(value));
}

std::string state(PhaseState value) {
  switch (value) {
  case PhaseState::complete: return "complete";
  case PhaseState::recovered: return "recovered";
  case PhaseState::failed: return "failed";
  case PhaseState::cancelled: return "cancelled";
  }
  return "failed";
}
std::string failure(FailureKind value) {
  switch (value) {
  case FailureKind::source: return "source";
  case FailureKind::unsupported: return "unsupported";
  case FailureKind::environment: return "environment";
  case FailureKind::internal: return "internal";
  }
  return "internal";
}

std::string diagnostics(const std::vector<Diagnostic> &values) {
  std::string out = "[";
  for (const auto &value : values) {
    out += "{" + str(value.code) + str(value.phase) + str(value.severity) +
           str(value.message) + span(value.primary) + "[";
    for (const auto &related : value.related)
      out += "{" + str(related.message) + span(related.span) + "}";
    out += "]}";
  }
  return out + "]";
}

std::string type_ref(const CTypeRef &value) {
  std::string args = "[";
  for (const auto &item : value.args) args += type_ref(item);
  args += "]";
  auto kind = value.kind == CTypeRef::Kind::scalar      ? "scalar"
              : value.kind == CTypeRef::Kind::pointer   ? "pointer"
              : value.kind == CTypeRef::Kind::buffer    ? "buffer"
                                                        : "named";
  return "{" + str(kind) + str(value.scalar) + boolean(value.writable) +
         args + num(value.named) + "}";
}
std::string decl_kind(CDeclKind value) {
  switch (value) {
  case CDeclKind::record: return "record";
  case CDeclKind::resource: return "resource";
  case CDeclKind::callback: return "callback";
  case CDeclKind::opaque: return "opaque";
  }
  return "opaque";
}

std::string python_bundle(const BindingBundle &value) {
  std::string requirements = "[";
  for (const auto &item : value.package_requirements)
    requirements += "{" + str(item.package) + str(item.version_policy) +
                    span(item.source) + "}";
  requirements += "]";
  std::string modules = "[";
  for (const auto &item : value.modules)
    modules += "{" + str(item.logical_path) + str(item.host_module) +
               str(item.provider) + span(item.source) + "}";
  modules += "]";
  return "{" + str(value.path.string()) + str(value.json) + str(value.sha256) +
         span(value.source) + num(uint64_t(value.schema_version)) +
         str(value.contract) + str(value.review_status) +
         strings(value.allowed_backends) + requirements + modules +
         ids(value.declarations) + strings(value.conformance_required) + "}";
}

std::string python_parameter(const BindingParameter &value) {
  return "{" + str(value.name) + str(value.type) + str(value.kind) +
         boolean(value.optional) + str(value.mode) + str(value.escapes) +
         span(value.source) + "}";
}

std::string python_decl(const ForeignDeclaration &value) {
  std::string out = "{" + num(value.id.value) + num(value.bundle) +
                    span(value.source) + str(value.json_pointer) +
                    str(value.identity) + str(value.module) + str(value.name) +
                    strings(value.host_attribute) + str(value.stable_identity);
  if (const auto *item = std::get_if<BindingType>(&value.detail)) {
    out += "{" + str("type") + str(item->kind) + str(item->copy) +
           ids(item->members) + ids(item->properties) + ids(item->indexers) +
           ids(item->slices) + "}";
  } else if (const auto *item = std::get_if<BindingValue>(&value.detail)) {
    out += "{" + str("value") + str(item->type) + "}";
  } else if (const auto *item = std::get_if<BindingCallable>(&value.detail)) {
    std::string parameters = "[";
    for (const auto &parameter : item->parameters)
      parameters += python_parameter(parameter);
    parameters += "]";
    out += "{" + str("callable") + str(item->call_kind) +
           present(item->owner_type.has_value(),
                   item->owner_type ? str(*item->owner_type) : "") +
           parameters + str(item->return_type) +
           present(item->error_type.has_value(),
                   item->error_type ? str(*item->error_type) : "") +
           str(item->return_ownership) + str(item->storage) +
           str(item->receiver_escapes) + strings(item->effects) + "}";
  } else if (const auto *item = std::get_if<BindingProperty>(&value.detail)) {
    out += "{" + str("property") + str(item->owner_type) + str(item->name) +
           str(item->type) + strings(item->getter) + strings(item->setter) +
           str(item->error_type) + "}";
  } else if (const auto *item = std::get_if<BindingIndexer>(&value.detail)) {
    out += "{" + str("indexer") + str(item->owner_type) +
           str(item->index_type) + str(item->result_type) +
           str(item->error_type) + boolean(item->readable) +
           boolean(item->writable) + "}";
  } else if (const auto *item = std::get_if<BindingSlice>(&value.detail)) {
    out += "{" + str("slice") + str(item->owner_type) +
           str(item->result_type) + str(item->error_type) +
           present(item->start_type.has_value(),
                   item->start_type ? str(*item->start_type) : "") +
           present(item->end_type.has_value(),
                   item->end_type ? str(*item->end_type) : "") +
           boolean(item->readable) + boolean(item->writable) + "}";
  }
  return out + "}";
}

std::string c_parameter(const CParameterDecl &value) {
  return "{" + str(value.name) + type_ref(value.type) + span(value.source) +
         "}";
}
std::string c_field(const CFieldDecl &value) {
  return "{" + str(value.name) + type_ref(value.type) + num(value.offset) +
         span(value.source) + "}";
}
std::string c_bundle(const CBindingBundle &value) {
  std::string modules = "[";
  for (const auto &item : value.modules)
    modules += "{" + str(item.logical_path) + str(item.provider) +
               str(item.library) + span(item.source) + "}";
  modules += "]";
  return "{" + str(value.path.string()) + str(value.json) + str(value.sha256) +
         span(value.source) + num(uint64_t(value.schema_version)) +
         str(value.contract) + str(value.abi) + str(value.target) +
         boolean(value.requires_hosted) + modules + ids(value.types) +
         ids(value.callables) + "}";
}
std::string c_type(const CTypeDecl &value) {
  std::string fields = "[";
  for (const auto &item : value.fields) fields += c_field(item);
  fields += "]";
  std::string parameters = "[";
  for (const auto &item : value.parameters)
    parameters += c_parameter(item);
  parameters += "]";
  std::string registration = absent();
  if (value.registration)
    registration = "{" + num(value.registration->callback_type) +
                   str(value.registration->unregister_symbol) + "}";
  return "{" + num(value.id) + num(value.bundle) + span(value.source) +
         str(value.identity) + str(value.module) + str(value.name) +
         str(value.c_name) + str(decl_kind(value.kind)) + fields +
         num(value.size) + num(value.align) + str(value.release_symbol) +
         str(value.retain_symbol) + type_ref(value.result) + parameters +
         registration + "}";
}
std::string c_callable(const CCallableDecl &value) {
  std::string parameters = "[";
  for (const auto &item : value.parameters)
    parameters += c_parameter(item);
  parameters += "]";
  std::string registers = absent();
  if (value.registers)
    registers = "{" + num(value.registers->callback_parameter) +
                num(value.registers->userdata_parameter) +
                num(value.registers->callback_userdata_parameter) + "}";
  std::string result = "{" + type_ref(value.result.type) +
                       str(value.result.ownership) +
                       str(value.result.release_symbol) +
                       str(value.result.retain_symbol) +
                       span(value.result.source) + "}";
  return "{" + num(value.id) + num(value.bundle) + span(value.source) +
         str(value.identity) + str(value.module) + str(value.name) +
         str(value.symbol) + parameters + result + str(value.stable_identity) +
         registers + "}";
}

std::string paths(const std::vector<std::string> &values) {
  std::string out = "[";
  for (size_t index = 0; index < values.size(); ++index)
    out += "{" + str(values[index]) + num(index) + "}";
  return out + "]";
}
std::string identities(const std::vector<std::pair<std::string, SourceSpan>> &values) {
  std::string out = "[";
  for (const auto &item : values)
    out += "{" + str(item.first) + span(item.second) + "}";
  return out + "]";
}

std::string store(const Project &project) {
  std::string python_bundles = "[";
  for (const auto &item : project.bindings) python_bundles += python_bundle(item);
  python_bundles += "]";
  std::string foreign = "[";
  for (const auto &item : project.foreign_bindings) foreign += python_decl(item);
  foreign += "]";
  std::string c_bundles = "[";
  for (const auto &item : project.c_bindings) c_bundles += c_bundle(item);
  c_bundles += "]";
  std::string c_types = "[";
  for (const auto &item : project.c_types) c_types += c_type(item);
  c_types += "]";
  std::string c_callables = "[";
  for (const auto &item : project.c_callables) c_callables += c_callable(item);
  c_callables += "]";

  std::vector<std::string> python_paths, c_paths;
  std::vector<std::pair<std::string, SourceSpan>> python_identities,
      c_type_identities, c_callable_identities;
  for (size_t bundle_index = 0; bundle_index < project.bindings.size();
       ++bundle_index) {
    const auto &bundle = project.bindings[bundle_index];
    python_paths.push_back(bundle.path.string());
    for (auto id : bundle.declarations) {
      const auto &declaration = project.foreign_binding(id);
      if (!std::holds_alternative<BindingValue>(declaration.detail))
        python_identities.emplace_back(declaration.identity, declaration.source);
    }
  }
  for (size_t bundle_index = 0; bundle_index < project.c_bindings.size();
       ++bundle_index) {
    const auto &bundle = project.c_bindings[bundle_index];
    c_paths.push_back(bundle.path.string());
    for (auto id : bundle.types)
      c_type_identities.emplace_back(project.c_type(id).identity,
                                     project.c_type(id).source);
    for (auto id : bundle.callables)
      c_callable_identities.emplace_back(project.c_callable(id).identity,
                                         project.c_callable(id).source);
  }
  return "{" + python_bundles + foreign + c_bundles + c_types + c_callables +
         paths(python_paths) + paths(c_paths) +
         identities(python_identities) + identities(c_type_identities) +
         identities(c_callable_identities) + "}";
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: bindings-oracle lang.toml\n";
    return 2;
  }
  SourceManager manager;
  sources = &manager;
  ProjectOptions options;
  options.manifest = argv[1];
  auto result = load_project(options, manager);
  std::string out = "{" + str(state(result.state)) +
                    str(failure(result.failure)) +
                    diagnostics(result.diagnostics);
  if (result.value)
    out += str("value") + store(*result.value);
  else
    out += str("value") + absent();
  out += "}";
  std::fwrite(out.data(), 1, out.size(), stdout);
  return 0;
}
