// C++ oracle for the ported foreign/signature modules.
//
// This test-only program uses the stage-0 implementation (lang::hir::detail
// from src/hir/foreign.hpp and src/hir/public_type_visibility.hpp plus
// system_operation.cpp) and prints the same canonical case lines as main.wz.
// It is never linked into or called by the new compiler.

#include "hir/foreign.hpp"
#include "hir/public_type_visibility.hpp"
#include "lang/hir/hir.hpp"
#include "lang/modules/project.hpp"
#include "lang/modules/resolve.hpp"
#include "lang/support/source.hpp"

#include <cstdint>
#include <iostream>
#include <sstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace lang;
using namespace lang::hir;
using namespace lang::modules;

namespace {

std::string num(uint64_t value) { return std::to_string(value); }

void emit(const std::string &line) { std::cout << line << "\n"; }

SourceSpan span0() { return SourceSpan{SourceSnapshotId{0}, 0, 0}; }

Source source0() {
  Source source{};
  source.span = span0();
  return source;
}

void dump_table(const std::string &label, TypeContext &context);
HirModule empty_module_default();

// ---------------------------------------------------------------------------
// Python fixtures

ForeignDeclaration foreign_type_decl(size_t id, const std::string &identity) {
  ForeignDeclaration declaration;
  declaration.id = ForeignBindingId{id};
  declaration.bundle = 0;
  declaration.source = span0();
  declaration.identity = identity;
  declaration.module = "demo";
  declaration.name = identity;
  BindingType detail;
  detail.kind = "type";
  detail.copy = "copy";
  declaration.detail = detail;
  declaration.stable_identity = identity;
  return declaration;
}

ForeignDeclaration foreign_value_decl(size_t id, const std::string &identity,
                                      const std::string &type_name) {
  ForeignDeclaration declaration;
  declaration.id = ForeignBindingId{id};
  declaration.bundle = 0;
  declaration.source = span0();
  declaration.identity = identity;
  declaration.module = "demo";
  declaration.name = identity;
  BindingValue detail;
  detail.type = type_name;
  declaration.detail = detail;
  declaration.stable_identity = identity;
  return declaration;
}

BindingParameter foreign_parameter(const std::string &name,
                                   const std::string &type_name,
                                   const std::string &kind, bool optional,
                                   const std::string &mode) {
  BindingParameter parameter;
  parameter.name = name;
  parameter.type = type_name;
  parameter.kind = kind;
  parameter.optional = optional;
  parameter.mode = mode;
  parameter.escapes = "none";
  parameter.source = span0();
  return parameter;
}

ForeignDeclaration foreign_callable_decl(size_t id, const std::string &identity,
                                         const std::string &return_type) {
  ForeignDeclaration declaration;
  declaration.id = ForeignBindingId{id};
  declaration.bundle = 0;
  declaration.source = span0();
  declaration.identity = identity;
  declaration.module = "demo";
  declaration.name = identity;
  BindingCallable callable;
  callable.call_kind = "method";
  callable.owner_type = std::string("demo.Point");
  callable.parameters.push_back(foreign_parameter(
      "self", "demo.Point", "positional_or_keyword", false, "ordinary"));
  callable.parameters.push_back(foreign_parameter(
      "count", "i32", "positional_or_keyword", true, "ordinary"));
  callable.parameters.push_back(
      foreign_parameter("flag", "bool", "positional_or_keyword", false, "move"));
  callable.parameters.push_back(
      foreign_parameter("slot", "demo.Point", "keyword_only", false, "inout"));
  callable.return_type = return_type;
  callable.error_type = std::string("lang.py.Error");
  callable.return_ownership = "owned";
  callable.storage = "reference";
  callable.receiver_escapes = "none";
  declaration.detail = callable;
  declaration.stable_identity = identity;
  return declaration;
}

ForeignDeclaration foreign_property_decl(size_t id, const std::string &identity,
                                         const std::string &type_name) {
  ForeignDeclaration declaration;
  declaration.id = ForeignBindingId{id};
  declaration.bundle = 0;
  declaration.source = span0();
  declaration.identity = identity;
  declaration.module = "demo";
  declaration.name = identity;
  BindingProperty property;
  property.owner_type = "demo.Point";
  property.name = identity;
  property.type = type_name;
  property.getter = {"size"};
  property.error_type = "lang.py.Error";
  declaration.detail = property;
  declaration.stable_identity = identity;
  return declaration;
}

ForeignDeclaration foreign_indexer_decl(size_t id, const std::string &identity,
                                        const std::string &result_type) {
  ForeignDeclaration declaration;
  declaration.id = ForeignBindingId{id};
  declaration.bundle = 0;
  declaration.source = span0();
  declaration.identity = identity;
  declaration.module = "demo";
  declaration.name = identity;
  BindingIndexer indexer;
  indexer.owner_type = "demo.Items";
  indexer.index_type = "usize";
  indexer.result_type = result_type;
  indexer.error_type = "lang.py.Error";
  indexer.readable = true;
  indexer.writable = false;
  declaration.detail = indexer;
  declaration.stable_identity = identity;
  return declaration;
}

ForeignDeclaration foreign_slice_decl(size_t id, const std::string &identity,
                                      const std::string &result_type) {
  ForeignDeclaration declaration;
  declaration.id = ForeignBindingId{id};
  declaration.bundle = 0;
  declaration.source = span0();
  declaration.identity = identity;
  declaration.module = "demo";
  declaration.name = identity;
  BindingSlice slice;
  slice.owner_type = "demo.Items";
  slice.result_type = result_type;
  slice.error_type = "lang.py.Error";
  slice.start_type = std::string("usize");
  slice.end_type = std::string("usize");
  slice.readable = true;
  slice.writable = false;
  declaration.detail = slice;
  declaration.stable_identity = identity;
  return declaration;
}

Project build_python_project() {
  Project project;
  project.foreign_bindings.push_back(foreign_type_decl(0, "lang.py.Error"));
  project.foreign_bindings.push_back(foreign_type_decl(1, "demo.Point"));
  project.foreign_bindings.push_back(
      foreign_value_decl(2, "demo.number", "i64"));
  project.foreign_bindings.push_back(
      foreign_callable_decl(3, "demo.make", "string"));
  project.foreign_bindings.push_back(
      foreign_property_decl(4, "demo.Point.size", "bool"));
  project.foreign_bindings.push_back(
      foreign_indexer_decl(5, "demo.Items.at", "u8"));
  project.foreign_bindings.push_back(
      foreign_slice_decl(6, "demo.Items.window", "string"));
  project.foreign_bindings.push_back(
      foreign_value_decl(7, "demo.point_value", "demo.Point"));
  project.foreign_bindings.push_back(foreign_value_decl(8, "demo.ready", "Unit"));
  return project;
}

Project project_with(std::vector<ForeignDeclaration> declarations) {
  Project project;
  project.foreign_bindings = std::move(declarations);
  return project;
}

std::string support_message(const std::invalid_argument &error) {
  return error.what();
}

void case_binding_type(const std::string &label, const Project &project,
                       TypeContext &context, const std::string &name) {
  try {
    TypeId id = detail::binding_type(project, context, name);
    emit("case " + label + " ok " + num(id.value));
  } catch (const std::invalid_argument &error) {
    emit("case " + label + " invalid " + support_message(error));
  }
}

std::string id_text(const std::optional<TypeId> &value) {
  return value ? num(value->value) : "-";
}

void print_records(const std::vector<ForeignBinding> &records) {
  emit("records " + num(records.size()));
  for (const auto &record : records) {
    emit("record " + num(record.binding.value) +
         " binding=" + num(record.binding.value) +
         " type=" + num(record.type.value) +
         " signature=" + id_text(record.signature) +
         " owner=" + id_text(record.owner) +
         " read_error=" + num(record.read_error.value));
  }
}

void case_binding_records(const std::string &label, const Project &project,
                          TypeContext &context) {
  try {
    auto records = detail::binding_records(project, context);
    emit("case " + label + " ok " + num(records.size()));
    print_records(records);
  } catch (const std::invalid_argument &error) {
    emit("case " + label + " invalid " + support_message(error));
  }
}

BindingCallable bind_callable() {
  BindingCallable callable;
  callable.call_kind = "function";
  callable.parameters.push_back(foreign_parameter(
      "first", "i32", "positional_or_keyword", false, "ordinary"));
  callable.parameters.push_back(foreign_parameter(
      "second", "i32", "positional_or_keyword", true, "ordinary"));
  callable.parameters.push_back(
      foreign_parameter("only", "i32", "positional_only", true, "ordinary"));
  callable.parameters.push_back(
      foreign_parameter("later", "i32", "keyword_only", true, "ordinary"));
  callable.parameters.push_back(
      foreign_parameter("last", "i32", "keyword_only", false, "ordinary"));
  callable.return_type = "Unit";
  callable.return_ownership = "value";
  callable.storage = "reference";
  callable.receiver_escapes = "none";
  return callable;
}

BindingCallable optional_callable() {
  BindingCallable callable;
  callable.call_kind = "function";
  callable.parameters.push_back(foreign_parameter(
      "first", "i32", "positional_or_keyword", true, "ordinary"));
  callable.parameters.push_back(
      foreign_parameter("later", "i32", "keyword_only", true, "ordinary"));
  callable.return_type = "Unit";
  callable.return_ownership = "value";
  callable.storage = "reference";
  callable.receiver_escapes = "none";
  return callable;
}

BindingCallable empty_callable() {
  BindingCallable callable;
  callable.call_kind = "function";
  callable.return_type = "Unit";
  callable.return_ownership = "value";
  callable.storage = "reference";
  callable.receiver_escapes = "none";
  return callable;
}

BindingCallable two_positional_callable() {
  BindingCallable callable;
  callable.call_kind = "function";
  callable.parameters.push_back(foreign_parameter(
      "left", "i32", "positional_or_keyword", false, "ordinary"));
  callable.parameters.push_back(foreign_parameter(
      "right", "i32", "positional_or_keyword", false, "ordinary"));
  callable.return_type = "Unit";
  callable.return_ownership = "value";
  callable.storage = "reference";
  callable.receiver_escapes = "none";
  return callable;
}

std::optional<std::string> label(const std::string &text_value) {
  return std::optional<std::string>(text_value);
}

std::optional<std::string> positional() {
  return std::optional<std::string>();
}

void case_bind(const std::string &name, const BindingCallable &callable,
               const std::vector<std::optional<std::string>> &labels) {
  auto result = detail::bind_arguments(callable, labels);
  if (!result) {
    emit("bind " + name + " none");
    return;
  }
  std::string parameters, omitted;
  for (size_t index = 0; index < result->parameters.size(); ++index) {
    if (index != 0) parameters += ",";
    parameters += num(result->parameters[index]);
  }
  for (size_t index = 0; index < result->omitted.size(); ++index) {
    if (index != 0) omitted += ",";
    omitted += num(result->omitted[index]);
  }
  emit("bind " + name + " ok params=" + parameters + " omitted=" + omitted);
}

void run_python_phase() {
  emit("phase python");
  Project project = build_python_project();
  TypeContext context;
  case_binding_type("binding_type.foreign-error", project, context,
                    "lang.py.Error");
  case_binding_type("binding_type.named-type", project, context, "demo.Point");
  case_binding_type("binding_type.i64", project, context, "i64");
  case_binding_type("binding_type.u8", project, context, "u8");
  case_binding_type("binding_type.i32", project, context, "i32");
  case_binding_type("binding_type.bool", project, context, "bool");
  case_binding_type("binding_type.string", project, context, "string");
  case_binding_type("binding_type.unit", project, context, "Unit");
  case_binding_type("binding_type.isize", project, context, "isize");
  case_binding_type("binding_type.usize", project, context, "usize");
  case_binding_type("binding_type.f32", project, context, "f32");
  case_binding_type("binding_type.f64", project, context, "f64");
  case_binding_type("binding_type.i8", project, context, "i8");
  case_binding_type("binding_type.i16", project, context, "i16");
  case_binding_type("binding_type.u16", project, context, "u16");
  case_binding_type("binding_type.u32", project, context, "u32");
  case_binding_type("binding_type.u64", project, context, "u64");
  case_binding_type("binding_type.unknown", project, context, "unknown");
  case_binding_type("binding_type.empty", project, context, "");
  case_binding_type("binding_type.value-identity", project, context,
                    "demo.number");
  case_binding_records("binding_records.full", project, context);
  Project empty_project;
  case_binding_records("binding_records.empty", empty_project, context);
  std::vector<ForeignDeclaration> only_value;
  only_value.push_back(foreign_value_decl(0, "demo.value", "i64"));
  case_binding_records("binding_records.no-error", project_with(only_value),
                       context);
  std::vector<ForeignDeclaration> bad_index;
  bad_index.push_back(foreign_type_decl(1, "lang.py.Error"));
  case_binding_records("binding_records.bad-index", project_with(bad_index),
                       context);
  std::vector<ForeignDeclaration> bad_type;
  bad_type.push_back(foreign_type_decl(0, "lang.py.Error"));
  bad_type.push_back(foreign_value_decl(1, "demo.value", "nope.Type"));
  case_binding_records("binding_records.bad-type", project_with(bad_type),
                       context);
  std::vector<ForeignDeclaration> bad_callable;
  bad_callable.push_back(foreign_type_decl(0, "lang.py.Error"));
  bad_callable.push_back(foreign_callable_decl(1, "demo.make", "nope.Type"));
  case_binding_records("binding_records.bad-callable",
                       project_with(bad_callable), context);

  BindingCallable bindable = bind_callable();
  std::vector<std::optional<std::string>> empty_labels;
  case_bind("bind.none-supplied", bindable, empty_labels);
  case_bind("bind.one-positional", bindable, {positional()});
  case_bind("bind.three-and-last", bindable,
            {positional(), positional(), positional(), label("last")});
  case_bind("bind.reordered", bindable,
            {label("second"), label("first"), label("last")});
  case_bind("bind.positional-only-named", bindable,
            {label("only"), label("last")});
  case_bind("bind.duplicate", bindable,
            {positional(), label("first"), label("last")});
  case_bind("bind.keyword-only-positional", bindable,
            {positional(), positional(), positional(), positional()});
  case_bind("bind.unknown-label", bindable, {label("nope")});
  case_bind("bind.missing-required", bindable, {label("last")});
  case_bind("bind.extra-label", bindable,
            {positional(), positional(), positional(), label("later"),
             label("last"), label("x")});
  case_bind("bind.optional-only-omitted", bindable,
            {positional(), positional(), positional(), label("later")});
  case_bind("bind.all-optional", optional_callable(), empty_labels);
  case_bind("bind.empty-callable", empty_callable(), empty_labels);
  case_bind("bind.empty-callable-positional", empty_callable(),
            {positional()});
  case_bind("bind.too-many-positional", two_positional_callable(),
            {positional(), positional(), positional()});
  case_bind("bind.named-then-positional", bindable,
            {label("second"), positional()});
  dump_table("python", context);
}

void dump_type(TypeContext &context, uint64_t id) {
  const Type *found = context.find(TypeId{id});
  if (!found) {
    emit("type " + num(id) + " missing");
    return;
  }
  std::string line = "type " + num(id) +
                     " kind=" +
                     num(static_cast<uint32_t>(found->kind)) +
                     " bits=" + num(found->bits) +
                     " decl=" + num(found->declaration.value) +
                     " bind=" + num(found->binding.value) + " params=";
  {
    std::ostringstream parameters;
    for (size_t index = 0; index < found->parameters.size(); ++index) {
      if (index != 0) parameters << ",";
      parameters << num(static_cast<uint32_t>(found->parameters[index].mode))
                 << ":" << num(found->parameters[index].type.value);
    }
    line += parameters.str();
  }
  line += " result=" + num(found->result.value) + " throws=";
  line += found->throws_type ? num(found->throws_type->value) : "-";
  line += " args=";
  {
    std::ostringstream arguments;
    for (size_t index = 0; index < found->type_args.size(); ++index) {
      if (index != 0) arguments << ",";
      arguments << num(found->type_args[index].value);
    }
    line += arguments.str();
  }
  line += " c=" + num(found->c_type);
  emit(line);
}

void dump_table(const std::string &label, TypeContext &context) {
  emit("table " + label + " " + num(context.size()));
  for (uint64_t id = 1; id <= context.size(); ++id) dump_type(context, id);
}

// ---------------------------------------------------------------------------
// C ABI fixtures

CTypeRef scalar_ref(const std::string &name) {
  CTypeRef ref;
  ref.kind = CTypeRef::Kind::scalar;
  ref.scalar = name;
  return ref;
}

CTypeRef ptr_ref(CTypeRef inner, bool writable) {
  CTypeRef ref;
  ref.kind = CTypeRef::Kind::pointer;
  ref.writable = writable;
  ref.args.push_back(std::move(inner));
  return ref;
}

CTypeRef buffer_ref(CTypeRef inner, bool writable) {
  CTypeRef ref;
  ref.kind = CTypeRef::Kind::buffer;
  ref.writable = writable;
  ref.args.push_back(std::move(inner));
  return ref;
}

CTypeRef named_ref(uint32_t index) {
  CTypeRef ref;
  ref.kind = CTypeRef::Kind::named;
  ref.named = index;
  return ref;
}

CTypeRef ptr_arity_ref(size_t count) {
  CTypeRef ref;
  ref.kind = CTypeRef::Kind::pointer;
  for (size_t index = 0; index < count; ++index)
    ref.args.push_back(scalar_ref("i32"));
  return ref;
}

CTypeRef deep_pointer(size_t depth) {
  CTypeRef result = scalar_ref("i32");
  for (size_t index = 0; index < depth; ++index)
    result = ptr_ref(result, false);
  return result;
}

CParameterDecl c_parameter(const std::string &name, CTypeRef ref) {
  CParameterDecl parameter;
  parameter.name = name;
  parameter.type = std::move(ref);
  parameter.source = span0();
  return parameter;
}

CReturnDecl c_return(CTypeRef ref) {
  CReturnDecl result;
  result.type = std::move(ref);
  result.ownership = "value";
  result.source = span0();
  return result;
}

CTypeDecl c_type_decl(uint32_t id, const std::string &identity,
                      CDeclKind kind) {
  CTypeDecl declaration;
  declaration.id = id;
  declaration.identity = identity;
  declaration.module = "demo";
  declaration.name = identity;
  declaration.c_name = identity;
  declaration.kind = kind;
  declaration.result = scalar_ref("Unit");
  declaration.source = span0();
  return declaration;
}

CTypeDecl callback_decl(uint32_t id, const std::string &identity,
                        std::vector<CParameterDecl> parameters,
                        CTypeRef result) {
  CTypeDecl declaration = c_type_decl(id, identity, CDeclKind::callback);
  declaration.parameters = std::move(parameters);
  declaration.result = std::move(result);
  return declaration;
}

CCallableDecl c_callable(uint32_t id, const std::string &identity,
                         std::vector<CParameterDecl> parameters,
                         CTypeRef result,
                         std::optional<CRegistrationCall> registers) {
  CCallableDecl callable;
  callable.id = id;
  callable.identity = identity;
  callable.module = "demo";
  callable.name = identity;
  callable.symbol = identity;
  callable.parameters = std::move(parameters);
  callable.result = c_return(std::move(result));
  callable.stable_identity = identity;
  callable.registers = registers;
  callable.source = span0();
  return callable;
}

Project build_c_project() {
  Project project;
  project.c_types.push_back(c_type_decl(1, "demo.Record", CDeclKind::record));
  project.c_types.push_back(
      c_type_decl(2, "demo.Resource", CDeclKind::resource));
  std::vector<CParameterDecl> callback_parameters;
  callback_parameters.push_back(c_parameter("count", scalar_ref("i32")));
  callback_parameters.push_back(
      c_parameter("state", ptr_ref(named_ref(4), true)));
  callback_parameters.push_back(c_parameter("tag", scalar_ref("u8")));
  project.c_types.push_back(callback_decl(3, "demo.Callback",
                                          std::move(callback_parameters),
                                          scalar_ref("Unit")));
  project.c_types.push_back(c_type_decl(4, "demo.Opaque", CDeclKind::opaque));
  std::vector<CParameterDecl> other_parameters;
  other_parameters.push_back(c_parameter("value", scalar_ref("i32")));
  project.c_types.push_back(callback_decl(5, "demo.Other",
                                          std::move(other_parameters),
                                          scalar_ref("i32")));

  std::vector<CParameterDecl> plain;
  plain.push_back(c_parameter("count", scalar_ref("i32")));
  plain.push_back(c_parameter("record", ptr_ref(named_ref(1), false)));
  plain.push_back(c_parameter("bytes", buffer_ref(scalar_ref("u8"), true)));
  project.c_callables.push_back(
      c_callable(1, "demo.plain", std::move(plain), scalar_ref("i64"),
                 std::nullopt));

  std::vector<CParameterDecl> registered;
  registered.push_back(c_parameter("callback", named_ref(3)));
  registered.push_back(
      c_parameter("userdata", ptr_ref(scalar_ref("u8"), true)));
  registered.push_back(c_parameter("count", scalar_ref("i32")));
  CRegistrationCall registers;
  registers.callback_parameter = 0;
  registers.userdata_parameter = 1;
  registers.callback_userdata_parameter = 1;
  project.c_callables.push_back(
      c_callable(2, "demo.registered", std::move(registered),
                 scalar_ref("i64"), registers));

  std::vector<CParameterDecl> first_slot;
  first_slot.push_back(c_parameter("userdata", ptr_ref(scalar_ref("u8"), true)));
  first_slot.push_back(c_parameter("count", scalar_ref("i32")));
  first_slot.push_back(c_parameter("callback", named_ref(3)));
  CRegistrationCall first_registers;
  first_registers.callback_parameter = 2;
  first_registers.userdata_parameter = 0;
  first_registers.callback_userdata_parameter = 0;
  project.c_callables.push_back(
      c_callable(3, "demo.first_slot", std::move(first_slot),
                 scalar_ref("Unit"), first_registers));

  std::vector<CParameterDecl> bad_kind;
  bad_kind.push_back(c_parameter("callback", scalar_ref("i32")));
  bad_kind.push_back(c_parameter("userdata", ptr_ref(scalar_ref("u8"), true)));
  CRegistrationCall bad_kind_registers;
  bad_kind_registers.callback_parameter = 0;
  bad_kind_registers.userdata_parameter = 1;
  bad_kind_registers.callback_userdata_parameter = 0;
  project.c_callables.push_back(
      c_callable(4, "demo.bad_kind", std::move(bad_kind), scalar_ref("Unit"),
                 bad_kind_registers));

  std::vector<CParameterDecl> bad_userdata;
  bad_userdata.push_back(c_parameter("callback", named_ref(3)));
  bad_userdata.push_back(
      c_parameter("userdata", ptr_ref(scalar_ref("u8"), true)));
  CRegistrationCall bad_userdata_registers;
  bad_userdata_registers.callback_parameter = 0;
  bad_userdata_registers.userdata_parameter = 1;
  bad_userdata_registers.callback_userdata_parameter = 99;
  project.c_callables.push_back(
      c_callable(5, "demo.bad_userdata", std::move(bad_userdata),
                 scalar_ref("Unit"), bad_userdata_registers));

  std::vector<CParameterDecl> unknown_callback;
  unknown_callback.push_back(c_parameter("callback", named_ref(3)));
  unknown_callback.push_back(
      c_parameter("userdata", ptr_ref(scalar_ref("u8"), true)));
  CRegistrationCall unknown_callback_registers;
  unknown_callback_registers.callback_parameter = 99;
  unknown_callback_registers.userdata_parameter = 1;
  unknown_callback_registers.callback_userdata_parameter = 0;
  project.c_callables.push_back(
      c_callable(6, "demo.unknown_callback", std::move(unknown_callback),
                 scalar_ref("Unit"), unknown_callback_registers));

  std::vector<CParameterDecl> unknown_userdata;
  unknown_userdata.push_back(c_parameter("callback", named_ref(3)));
  unknown_userdata.push_back(
      c_parameter("userdata", ptr_ref(scalar_ref("u8"), true)));
  CRegistrationCall unknown_userdata_registers;
  unknown_userdata_registers.callback_parameter = 0;
  unknown_userdata_registers.userdata_parameter = 99;
  unknown_userdata_registers.callback_userdata_parameter = 0;
  project.c_callables.push_back(
      c_callable(7, "demo.unknown_userdata", std::move(unknown_userdata),
                 scalar_ref("Unit"), unknown_userdata_registers));

  std::vector<CParameterDecl> bad_result;
  project.c_callables.push_back(
      c_callable(8, "demo.bad_result", std::move(bad_result), scalar_ref("zz"),
                 std::nullopt));
  return project;
}

void case_c_type(const std::string &label, const Project &project,
                 TypeContext &context, const CTypeRef &ref) {
  try {
    TypeId id = detail::c_type_id(project, context, ref);
    emit("case " + label + " ok " + num(id.value));
  } catch (const std::invalid_argument &error) {
    emit("case " + label + " invalid " + support_message(error));
  }
}

void case_closure(const std::string &label, const Project &project,
                  TypeContext &context, uint32_t callback_type,
                  size_t userdata_index) {
  try {
    TypeId id = detail::c_callback_closure_signature(project, context,
                                                     callback_type,
                                                     userdata_index);
    emit("case " + label + " ok " + num(id.value));
  } catch (const std::invalid_argument &error) {
    emit("case " + label + " invalid " + support_message(error));
  }
}

void case_callable_signature(const std::string &label, const Project &project,
                             TypeContext &context,
                             const CCallableDecl &callable) {
  try {
    TypeId id = detail::c_callable_signature(project, context, callable);
    emit("case " + label + " ok " + num(id.value));
  } catch (const std::invalid_argument &error) {
    emit("case " + label + " invalid " + support_message(error));
  }
}

void case_source_indices(const std::string &label,
                         const CCallableDecl &callable) {
  auto indices = c_source_parameter_indices(callable);
  std::string body;
  for (size_t index = 0; index < indices.size(); ++index) {
    if (index != 0) body += ",";
    body += num(indices[index]);
  }
  emit("indices " + label + " [" + body + "]");
}

void run_c_phase() {
  emit("phase c");
  Project project = build_c_project();
  TypeContext context;
  case_c_type("c_type.scalar-i32", project, context, scalar_ref("i32"));
  case_c_type("c_type.scalar-u8", project, context, scalar_ref("u8"));
  case_c_type("c_type.scalar-f64", project, context, scalar_ref("f64"));
  case_c_type("c_type.scalar-unit", project, context, scalar_ref("Unit"));
  case_c_type("c_type.scalar-bool", project, context, scalar_ref("bool"));
  case_c_type("c_type.scalar-isize", project, context, scalar_ref("isize"));
  case_c_type("c_type.scalar-usize", project, context, scalar_ref("usize"));
  case_c_type("c_type.scalar-leading", project, context, scalar_ref("i32x"));
  case_c_type("c_type.scalar-nodigit", project, context, scalar_ref("iabc"));
  case_c_type("c_type.scalar-empty", project, context, scalar_ref(""));
  case_c_type("c_type.scalar-one", project, context, scalar_ref("i"));
  case_c_type("c_type.scalar-other", project, context, scalar_ref("x8"));
  case_c_type("c_type.scalar-i128", project, context, scalar_ref("i128"));
  case_c_type("c_type.scalar-f16", project, context, scalar_ref("f16"));
  case_c_type("c_type.ptr", project, context,
              ptr_ref(scalar_ref("i32"), false));
  case_c_type("c_type.ptr-mut", project, context,
              ptr_ref(scalar_ref("i32"), true));
  case_c_type("c_type.ptr-zero", project, context, ptr_arity_ref(0));
  case_c_type("c_type.ptr-two", project, context, ptr_arity_ref(2));
  case_c_type("c_type.buffer", project, context,
              buffer_ref(scalar_ref("u8"), true));
  case_c_type("c_type.buffer-record", project, context,
              buffer_ref(named_ref(1), false));
  case_c_type("c_type.named-record", project, context, named_ref(1));
  case_c_type("c_type.named-resource", project, context, named_ref(2));
  case_c_type("c_type.named-callback", project, context, named_ref(3));
  case_c_type("c_type.named-opaque", project, context, named_ref(4));
  case_c_type("c_type.named-zero", project, context, named_ref(0));
  case_c_type("c_type.named-outside", project, context, named_ref(99));
  case_c_type("c_type.ptr-depth-32", project, context, deep_pointer(32));
  case_c_type("c_type.ptr-depth-33", project, context, deep_pointer(33));

  case_source_indices("plain", project.c_callables[0]);
  case_source_indices("registered", project.c_callables[1]);
  case_source_indices("first-slot", project.c_callables[2]);
  case_source_indices("bad-userdata", project.c_callables[4]);
  case_source_indices("unknown-userdata", project.c_callables[6]);

  case_closure("closure.userdata-1", project, context, 3, 1);
  case_closure("closure.userdata-0", project, context, 3, 0);
  case_closure("closure.userdata-2", project, context, 3, 2);
  case_closure("closure.zero", project, context, 0, 0);
  case_closure("closure.outside", project, context, 99, 0);
  case_closure("closure.record", project, context, 1, 0);
  case_closure("closure.userdata-outside", project, context, 3, 3);

  case_callable_signature("callable.plain", project, context,
                          project.c_callables[0]);
  case_callable_signature("callable.registered", project, context,
                          project.c_callables[1]);
  case_callable_signature("callable.first-slot", project, context,
                          project.c_callables[2]);
  case_callable_signature("callable.bad-kind", project, context,
                          project.c_callables[3]);
  case_callable_signature("callable.bad-userdata", project, context,
                          project.c_callables[4]);
  case_callable_signature("callable.unknown-callback", project, context,
                          project.c_callables[5]);
  case_callable_signature("callable.unknown-userdata", project, context,
                          project.c_callables[6]);
  case_callable_signature("callable.bad-result", project, context,
                          project.c_callables[7]);
  dump_table("c", context);
}

// ---------------------------------------------------------------------------
// Public interface visibility

Symbol private_symbol(uint64_t id, const std::string &name, SymbolKind kind,
                      bool exported) {
  Symbol symbol;
  symbol.id = SymbolId{id};
  symbol.name = name;
  symbol.kind = kind;
  symbol.module = ModuleId{0};
  symbol.exported = exported;
  return symbol;
}

ResolvedProject build_private_resolved() {
  ResolvedProject resolved;
  resolved.symbols.push_back(
      private_symbol(0, "PubStruct", SymbolKind::type, true));
  resolved.symbols.push_back(
      private_symbol(1, "PrivStruct", SymbolKind::type, false));
  resolved.symbols.push_back(
      private_symbol(2, "PubTrait", SymbolKind::trait, true));
  resolved.symbols.push_back(
      private_symbol(3, "PrivTrait", SymbolKind::trait, false));
  resolved.symbols.push_back(
      private_symbol(4, "ValueSym", SymbolKind::value, false));
  resolved.symbols.push_back(
      private_symbol(5, "ParamSym", SymbolKind::type_parameter, false));
  resolved.symbols.push_back(
      private_symbol(6, "PubEnum", SymbolKind::type, true));
  resolved.symbols.push_back(
      private_symbol(7, "PrivEnum", SymbolKind::type, false));
  auto project = std::make_shared<Project>();
  Module module;
  module.id = ModuleId{0};
  module.logical_path = "demo";
  module.identity = "demo";
  module.package_name = "demo";
  project->modules.push_back(module);
  resolved.project = project;
  return resolved;
}

TypeId intern_named(TypeContext &context, TypeKind kind, uint64_t declaration) {
  Type value;
  value.kind = kind;
  value.declaration = SymbolId{declaration};
  return context.intern(std::move(value));
}

TypeId intern_function(TypeContext &context,
                       std::vector<ParameterType> parameters, TypeId result,
                       std::optional<TypeId> throws_type) {
  Type value;
  value.kind = TypeKind::Function;
  value.parameters = std::move(parameters);
  value.result = result;
  value.throws_type = throws_type;
  return context.intern(std::move(value));
}

ParameterType ordinary_parameter(TypeId type_id) {
  return ParameterType{ParamMode::ordinary, type_id};
}

TypeId intern_vec(TypeContext &context, TypeId element) {
  Type value;
  value.kind = TypeKind::Vec;
  value.type_args.push_back(element);
  return context.intern(std::move(value));
}

void case_private(const std::string &label, const TypeContext &context,
                  const ResolvedProject &resolved,
                  const std::vector<TypeId> &roots) {
  auto found = detail::private_interface_type(context, resolved, roots);
  emit("private " + label + " " + (found ? num(found->value) : "-"));
}

void run_private_phase() {
  emit("phase private");
  ResolvedProject resolved = build_private_resolved();
  TypeContext context;
  TypeId pub_struct = intern_named(context, TypeKind::NominalStruct, 0);
  TypeId priv_struct = intern_named(context, TypeKind::NominalStruct, 1);
  TypeId pub_trait = intern_named(context, TypeKind::TraitObject, 2);
  TypeId priv_trait = intern_named(context, TypeKind::TraitObject, 3);
  TypeId value_sym = intern_named(context, TypeKind::NominalStruct, 4);
  TypeId param_sym = intern_named(context, TypeKind::TypeParameter, 5);
  TypeId priv_enum = intern_named(context, TypeKind::NominalEnum, 7);
  TypeId vec_priv = intern_vec(context, priv_struct);
  TypeId vec_pub = intern_vec(context, pub_struct);
  std::vector<ParameterType> parameter_priv{ordinary_parameter(priv_struct)};
  std::vector<ParameterType> parameter_two{ordinary_parameter(priv_struct),
                                           ordinary_parameter(priv_struct)};
  TypeId fn_param_priv =
      intern_function(context, parameter_priv, pub_struct, std::nullopt);
  TypeId fn_result_priv =
      intern_function(context, {}, priv_struct, std::nullopt);
  TypeId fn_throws_priv =
      intern_function(context, {}, pub_struct, priv_struct);
  TypeId fn_throws_pub =
      intern_function(context, {}, pub_struct, pub_struct);
  TypeId fn_two_refs =
      intern_function(context, parameter_two, pub_struct, std::nullopt);
  TypeId nom_outside = intern_named(context, TypeKind::NominalStruct, 99);
  case_private("public-struct", context, resolved, {pub_struct});
  case_private("private-struct", context, resolved, {priv_struct});
  case_private("public-trait", context, resolved, {pub_trait});
  case_private("private-trait", context, resolved, {priv_trait});
  case_private("value-symbol", context, resolved, {value_sym});
  case_private("type-parameter", context, resolved, {param_sym});
  case_private("vec-private", context, resolved, {vec_priv});
  case_private("vec-public", context, resolved, {vec_pub});
  case_private("function-param", context, resolved, {fn_param_priv});
  case_private("function-result", context, resolved, {fn_result_priv});
  case_private("function-throws", context, resolved, {fn_throws_priv});
  case_private("function-throws-public", context, resolved, {fn_throws_pub});
  case_private("function-two-refs", context, resolved, {fn_two_refs});
  case_private("roots-two", context, resolved, {pub_struct, priv_enum});
  case_private("roots-empty", context, resolved, {});
  case_private("zero-root", context, resolved, {TypeId{0}});
  case_private("nominal-outside", context, resolved, {nom_outside});
  case_private("order-private", context, resolved, {priv_struct, priv_enum});
  case_private("order-private-enum", context, resolved,
               {priv_enum, priv_struct});
  dump_table("private", context);
}

// ---------------------------------------------------------------------------
// Standard module fixture

size_t fixture_symbol(std::vector<Symbol> &symbols, const std::string &name,
                      SymbolKind kind, uint64_t module, bool exported) {
  Symbol symbol;
  symbol.id = SymbolId{symbols.size()};
  symbol.name = name;
  symbol.kind = kind;
  symbol.module = ModuleId{module};
  symbol.exported = exported;
  symbols.push_back(std::move(symbol));
  return symbols.size() - 1;
}

Module fixture_module(const std::string &logical_path) {
  Module module;
  module.id = ModuleId{0};
  module.logical_path = logical_path;
  module.identity = logical_path;
  module.package_name = "";
  return module;
}

GenericParam generic_parameter(size_t symbol) {
  GenericParam parameter;
  parameter.symbol = SymbolId{symbol};
  parameter.trait = std::nullopt;
  parameter.source = source0();
  return parameter;
}

FieldDecl field_decl(size_t symbol, TypeId type_id) {
  FieldDecl field;
  field.symbol = SymbolId{symbol};
  field.type = type_id;
  field.exported = true;
  field.source = source0();
  return field;
}

VariantDecl variant_decl(size_t symbol, std::vector<FieldDecl> payload) {
  VariantDecl variant;
  variant.symbol = SymbolId{symbol};
  variant.payload = std::move(payload);
  variant.source = source0();
  return variant;
}

TypeId fixture_type_parameter(TypeContext &context, uint64_t declaration) {
  Type value;
  value.kind = TypeKind::TypeParameter;
  value.declaration = SymbolId{declaration};
  return context.intern(std::move(value));
}

TypeId fixture_leaf(TypeContext &context, TypeKind kind, uint32_t bits) {
  Type value;
  value.kind = kind;
  value.bits = bits;
  return context.intern(std::move(value));
}

TypeId fixture_vec(TypeContext &context, TypeId element) {
  Type value;
  value.kind = TypeKind::Vec;
  value.type_args.push_back(element);
  return context.intern(std::move(value));
}

TypeId fixture_named(TypeContext &context, TypeKind kind, uint64_t declaration) {
  Type value;
  value.kind = kind;
  value.declaration = SymbolId{declaration};
  return context.intern(std::move(value));
}

size_t symbol_by_name(const std::vector<Symbol> &symbols,
                      const std::string &name) {
  for (size_t index = 0; index < symbols.size(); ++index)
    if (symbols[index].name == name) return index;
  return 0;
}

TypeId fixture_field_type(TypeContext &context, const std::string &name,
                          const std::vector<Symbol> &symbols) {
  if (name == "i16") return fixture_leaf(context, TypeKind::SignedInt, 16);
  if (name == "i32") return fixture_leaf(context, TypeKind::SignedInt, 32);
  if (name == "i64") return fixture_leaf(context, TypeKind::SignedInt, 64);
  if (name == "u8") return fixture_leaf(context, TypeKind::UnsignedInt, 8);
  if (name == "u16") return fixture_leaf(context, TypeKind::UnsignedInt, 16);
  if (name == "u32") return fixture_leaf(context, TypeKind::UnsignedInt, 32);
  if (name == "u64") return fixture_leaf(context, TypeKind::UnsignedInt, 64);
  if (name == "usize") return fixture_leaf(context, TypeKind::Usize, 0);
  if (name == "f64") return fixture_leaf(context, TypeKind::Float, 64);
  if (name == "string") return fixture_leaf(context, TypeKind::String, 0);
  if (name == "bool") return fixture_leaf(context, TypeKind::Bool, 0);
  if (name == "Unit") return fixture_leaf(context, TypeKind::Unit, 0);
  if (name == "Vec[u8]") {
    TypeId element = fixture_leaf(context, TypeKind::UnsignedInt, 8);
    return fixture_vec(context, element);
  }
  if (name == "std/os.File")
    return fixture_named(context, TypeKind::NominalStruct, symbols[9].id.value);
  if (name == "std/os.Timespec")
    return fixture_named(context, TypeKind::NominalStruct, symbols[10].id.value);
  if (name == "std/net.Socket")
    return fixture_named(context, TypeKind::NominalStruct, symbols[13].id.value);
  if (name == "std/net.Address")
    return fixture_named(context, TypeKind::NominalStruct, symbols[14].id.value);
  return TypeId{0};
}

void push_struct(TypeContext &context, std::vector<Symbol> &symbols,
                 std::vector<NominalDecl> &nominals, const std::string &name,
                 uint64_t module, std::vector<std::string> field_names,
                 std::vector<std::string> field_types) {
  std::vector<FieldDecl> fields;
  for (size_t index = 0; index < field_names.size(); ++index) {
    size_t symbol = fixture_symbol(symbols, field_names[index],
                                   SymbolKind::field, module, true);
    fields.push_back(
        field_decl(symbol, fixture_field_type(context, field_types[index],
                                              symbols)));
  }
  size_t symbol_id = symbol_by_name(symbols, name);
  NominalDecl nominal;
  nominal.symbol = SymbolId{symbol_id};
  nominal.kind = TypeKind::NominalStruct;
  nominal.fields = std::move(fields);
  nominal.source = source0();
  nominals.push_back(std::move(nominal));
}

HirModule build_std_module(TypeContext &context) {
  auto project = std::make_shared<Project>();
  project->modules.push_back(fixture_module("std/result"));
  project->modules.push_back(fixture_module("std/option"));
  project->modules.push_back(fixture_module("std/os"));
  project->modules.push_back(fixture_module("std/net"));
  project->modules.push_back(fixture_module("std/math"));
  project->modules.push_back(fixture_module("std/process"));
  project->modules.push_back(fixture_module("std/signal"));
  auto resolved = std::make_shared<ResolvedProject>();
  resolved->project = project;

  std::vector<Symbol> &symbols = resolved->symbols;
  std::vector<NominalDecl> nominals;
  size_t result_symbol =
      fixture_symbol(symbols, "Result", SymbolKind::type, 0, true);
  size_t result_a =
      fixture_symbol(symbols, "A", SymbolKind::type_parameter, 0, false);
  size_t result_b =
      fixture_symbol(symbols, "B", SymbolKind::type_parameter, 0, false);
  size_t ok_symbol = fixture_symbol(symbols, "Ok", SymbolKind::variant, 0, true);
  size_t err_symbol =
      fixture_symbol(symbols, "Err", SymbolKind::variant, 0, true);
  size_t option_symbol =
      fixture_symbol(symbols, "Option", SymbolKind::type, 1, true);
  size_t option_a =
      fixture_symbol(symbols, "A", SymbolKind::type_parameter, 1, false);
  size_t none_symbol =
      fixture_symbol(symbols, "None", SymbolKind::variant, 1, true);
  size_t some_symbol =
      fixture_symbol(symbols, "Some", SymbolKind::variant, 1, true);
  fixture_symbol(symbols, "File", SymbolKind::type, 2, true);
  fixture_symbol(symbols, "Timespec", SymbolKind::type, 2, true);
  fixture_symbol(symbols, "Pipe", SymbolKind::type, 2, true);
  fixture_symbol(symbols, "FileInfo", SymbolKind::type, 2, true);
  fixture_symbol(symbols, "Socket", SymbolKind::type, 3, true);
  fixture_symbol(symbols, "Address", SymbolKind::type, 3, true);
  fixture_symbol(symbols, "Accepted", SymbolKind::type, 3, true);
  fixture_symbol(symbols, "Received", SymbolKind::type, 3, true);
  fixture_symbol(symbols, "Pair", SymbolKind::type, 3, true);
  fixture_symbol(symbols, "PollFd", SymbolKind::type, 3, true);
  fixture_symbol(symbols, "Fraction", SymbolKind::type, 4, true);
  fixture_symbol(symbols, "WaitStatus", SymbolKind::type, 5, true);
  fixture_symbol(symbols, "Info", SymbolKind::type, 6, true);
  TypeId a_type = fixture_type_parameter(context, result_a);
  TypeId b_type = fixture_type_parameter(context, result_b);
  TypeId option_a_type = fixture_type_parameter(context, option_a);
  std::vector<GenericParam> result_params;
  result_params.push_back(generic_parameter(result_a));
  result_params.push_back(generic_parameter(result_b));
  std::vector<FieldDecl> ok_payload{field_decl(ok_symbol, a_type)};
  std::vector<FieldDecl> err_payload{field_decl(err_symbol, b_type)};
  std::vector<VariantDecl> result_variants;
  result_variants.push_back(variant_decl(ok_symbol, std::move(ok_payload)));
  result_variants.push_back(variant_decl(err_symbol, std::move(err_payload)));
  NominalDecl result_nominal;
  result_nominal.symbol = SymbolId{result_symbol};
  result_nominal.kind = TypeKind::NominalEnum;
  result_nominal.params = std::move(result_params);
  result_nominal.variants = std::move(result_variants);
  result_nominal.source = source0();
  nominals.push_back(std::move(result_nominal));
  std::vector<GenericParam> option_params;
  option_params.push_back(generic_parameter(option_a));
  std::vector<FieldDecl> none_payload;
  std::vector<FieldDecl> some_payload{field_decl(some_symbol, option_a_type)};
  std::vector<VariantDecl> option_variants;
  option_variants.push_back(variant_decl(none_symbol, std::move(none_payload)));
  option_variants.push_back(variant_decl(some_symbol, std::move(some_payload)));
  NominalDecl option_nominal;
  option_nominal.symbol = SymbolId{option_symbol};
  option_nominal.kind = TypeKind::NominalEnum;
  option_nominal.params = std::move(option_params);
  option_nominal.variants = std::move(option_variants);
  option_nominal.source = source0();
  nominals.push_back(std::move(option_nominal));

  push_struct(context, symbols, nominals, "File", 2, {"descriptor"}, {"i32"});
  push_struct(context, symbols, nominals, "Timespec", 2,
              {"seconds", "nanoseconds"}, {"i64", "i64"});
  push_struct(context, symbols, nominals, "Pipe", 2, {"read_end", "write_end"},
              {"std/os.File", "std/os.File"});
  push_struct(context, symbols, nominals, "FileInfo", 2,
              {"device", "inode", "links", "mode", "uid", "gid", "rdev", "size",
               "block_size", "blocks", "accessed", "modified", "changed"},
              {"u64", "u64", "u64", "u32", "u32", "u32", "u64", "i64", "i64",
               "i64", "std/os.Timespec", "std/os.Timespec",
               "std/os.Timespec"});
  push_struct(context, symbols, nominals, "Socket", 3, {"descriptor"}, {"i32"});
  push_struct(context, symbols, nominals, "Address", 3,
              {"family", "port", "flags", "scope", "reserved", "data"},
              {"i32", "u16", "u16", "u32", "u32", "Vec[u8]"});
  push_struct(context, symbols, nominals, "Accepted", 3,
              {"socket", "address"}, {"std/net.Socket", "std/net.Address"});
  push_struct(context, symbols, nominals, "Received", 3, {"count", "address"},
              {"usize", "std/net.Address"});
  push_struct(context, symbols, nominals, "Pair", 3, {"first", "second"},
              {"std/net.Socket", "std/net.Socket"});
  push_struct(context, symbols, nominals, "PollFd", 3,
              {"fd", "events", "revents"}, {"i32", "i16", "i16"});
  push_struct(context, symbols, nominals, "Fraction", 4,
              {"fraction", "exponent"}, {"f64", "i32"});
  push_struct(context, symbols, nominals, "WaitStatus", 5, {"pid", "status"},
              {"i32", "i32"});
  push_struct(context, symbols, nominals, "Info", 6,
              {"number", "code", "pid", "uid", "status", "reserved", "value"},
              {"i32", "i32", "i32", "u32", "i32", "u32", "i64"});

  HirModule module;
  module.resolved = resolved;
  module.identity = ModuleId{0};
  module.types = context;
  module.config = Config{Backend::native, 64};
  module.nominals = std::move(nominals);
  return module;
}

std::vector<FieldDecl> copy_fields(const std::vector<FieldDecl> &values) {
  return values;
}

std::vector<VariantDecl> copy_variants(const std::vector<VariantDecl> &values) {
  std::vector<VariantDecl> out;
  for (const auto &value : values) {
    VariantDecl variant = value;
    variant.payload = copy_fields(value.payload);
    out.push_back(std::move(variant));
  }
  return out;
}

std::vector<NominalDecl> copy_nominals(
    const std::vector<NominalDecl> &values) {
  std::vector<NominalDecl> out;
  for (const auto &value : values) {
    NominalDecl nominal = value;
    nominal.params = value.params;
    nominal.fields = copy_fields(value.fields);
    nominal.variants = copy_variants(value.variants);
    out.push_back(std::move(nominal));
  }
  return out;
}

ResolvedProject &own_resolved(HirModule &module) {
  auto resolved = std::make_shared<ResolvedProject>(*module.resolved);
  resolved->project = std::make_shared<Project>(*module.resolved->project);
  resolved->symbols = module.resolved->symbols;
  module.resolved = resolved;
  return *resolved;
}

Project &own_project(HirModule &module) {
  auto resolved = std::make_shared<ResolvedProject>(*module.resolved);
  auto project = std::make_shared<Project>(*module.resolved->project);
  resolved->project = project;
  resolved->symbols = module.resolved->symbols;
  module.resolved = resolved;
  return *project;
}

HirModule copy_module(const HirModule &module) {
  HirModule result = module;
  auto resolved = std::make_shared<ResolvedProject>(*module.resolved);
  resolved->project = std::make_shared<Project>(*module.resolved->project);
  resolved->symbols = module.resolved->symbols;
  result.resolved = resolved;
  result.nominals = copy_nominals(module.nominals);
  return result;
}

HirModule without_nominal(const HirModule &module, size_t skip) {
  HirModule result = module;
  std::vector<NominalDecl> kept;
  for (size_t index = 0; index < module.nominals.size(); ++index)
    if (index != skip) kept.push_back(module.nominals[index]);
  result.nominals = std::move(kept);
  return result;
}

HirModule keep_parameters(const HirModule &module, size_t index, size_t count) {
  HirModule result = copy_module(module);
  std::vector<GenericParam> kept;
  for (size_t position = 0; position < count; ++position)
    kept.push_back(module.nominals[index].params[position]);
  result.nominals[index].params = std::move(kept);
  return result;
}

HirModule keep_fields(const HirModule &module, size_t index, size_t count) {
  HirModule result = copy_module(module);
  std::vector<FieldDecl> kept;
  for (size_t position = 0; position < count; ++position)
    kept.push_back(module.nominals[index].fields[position]);
  result.nominals[index].fields = std::move(kept);
  return result;
}

void case_operation(const std::string &label, TypeContext &context,
                    const std::optional<HirModule> &module, uint32_t operation,
                    const std::vector<TypeId> &arguments) {
  try {
    TypeId id;
    if (module) {
      id = system_operation_signature(context, *module,
                                      static_cast<SystemOperation>(operation),
                                      arguments);
    } else {
      id = system_operation_signature(context,
                                      static_cast<SystemOperation>(operation),
                                      arguments);
    }
    emit("case " + label + " ok " + num(id.value));
  } catch (const std::invalid_argument &error) {
    emit("case " + label + " invalid " + support_message(error));
  }
}

std::vector<TypeId> op_args1(TypeId first) { return {first}; }
std::vector<TypeId> op_args2(TypeId first, TypeId second) {
  return {first, second};
}
std::vector<TypeId> op_args3(TypeId first, TypeId second, TypeId third) {
  return {first, second, third};
}
std::vector<TypeId> op_args4(TypeId first, TypeId second, TypeId third,
                             TypeId fourth) {
  return {first, second, third, fourth};
}
std::vector<TypeId> op_args5(TypeId first, TypeId second, TypeId third,
                             TypeId fourth, TypeId fifth) {
  return {first, second, third, fourth, fifth};
}

void run_system_phase() {
  emit("phase system");
  TypeContext context;
  HirModule base = build_std_module(context);
  TypeId a_type = fixture_field_type(context, "i32", base.resolved->symbols);
  TypeId b_type = fixture_field_type(context, "i64", base.resolved->symbols);
  TypeId string_type =
      fixture_field_type(context, "string", base.resolved->symbols);
  TypeId vec_u8 = fixture_field_type(context, "Vec[u8]", base.resolved->symbols);
  TypeId u32_type = fixture_field_type(context, "u32", base.resolved->symbols);
  TypeId u64_type = fixture_field_type(context, "u64", base.resolved->symbols);
  TypeId f64_type = fixture_field_type(context, "f64", base.resolved->symbols);
  TypeId c_type = u32_type;
  TypeId d_type = f64_type;
  TypeId pollfd_type =
      fixture_named(context, TypeKind::NominalStruct,
                    base.resolved->symbols[18].id.value);
  TypeId vec_pollfd = fixture_vec(context, pollfd_type);
  uint32_t operation_count = 0;
  for (const auto &spec : all_system_operations) {
    std::vector<TypeId> arguments;
    if (spec.type_arity >= 1) arguments.push_back(a_type);
    if (spec.type_arity >= 2) arguments.push_back(b_type);
    if (spec.type_arity >= 3) arguments.push_back(c_type);
    if (spec.type_arity >= 4) arguments.push_back(d_type);
    uint32_t operation = static_cast<uint32_t>(spec.operation);
    case_operation("op." + num(operation) + ".module", context, base,
                   operation, arguments);
    case_operation("op." + num(operation) + ".none", context, std::nullopt,
                   operation, arguments);
    if (operation >= 213 && operation <= 226) {
      emit("registry op." + num(operation) + " owner=" +
           std::string(spec.owner) + " arity=" + num(spec.type_arity));
    }
    ++operation_count;
  }
  emit("operations " + num(operation_count));
  TypeId bool_type = fixture_leaf(context, TypeKind::Bool, 0);
  TypeId f32_type = fixture_leaf(context, TypeKind::Float, 32);
  TypeId param_type = fixture_type_parameter(context, 42);
  case_operation("op.buffer-bool", context, base, 193, op_args1(bool_type));
  case_operation("op.buffer-float", context, base, 193, op_args1(f32_type));
  case_operation("op.buffer-parameter", context, base, 193,
                 op_args1(param_type));
  case_operation("op.vec-parameter", context, base, 34, op_args1(param_type));
  case_operation("op.task-parameters", context, base, 204,
                 op_args2(param_type, bool_type));

  case_operation("op.scope-add-missing", context, base, 223,
                 op_args2(a_type, b_type));
  case_operation("op.scope-add-wrong-arity", context, base, 223,
                 op_args3(a_type, b_type, c_type));
  case_operation("op.scope-add-extra-type-args", context, base, 223,
                 op_args5(a_type, b_type, c_type, d_type, string_type));
  case_operation("op.scope-add-none", context, std::nullopt, 223,
                 op_args4(a_type, b_type, c_type, d_type));
  case_operation("op.scope-begin-missing", context, base, 222, {});
  case_operation("op.scope-activate-missing", context, base, 224, {});
  case_operation("op.scope-wait-extra-type-arg", context, base, 225,
                 op_args1(a_type));
  case_operation("op.task-status-missing", context, base, 226, {});

  HirModule empty_module = empty_module_default();
  case_operation("op.unknown", context, base, 65535, {});
  case_operation("op.read-zero", context, base, 0, {});
  case_operation("op.read-four", context, base, 0,
                 op_args4(a_type, vec_u8, b_type, b_type));
  case_operation("op.zero-not-public", context, base, 36, op_args1(TypeId{0}));
  case_operation("op.outside-not-public", context, base, 36,
                 op_args1(TypeId{9999}));
  case_operation("op.extra-type-args", context, base, 9, op_args1(a_type));
  case_operation("op.thread-no-type-args", context, base, 13, {});
  case_operation("op.thread-two-type-args", context, base, 13,
                 op_args2(a_type, b_type));
  case_operation("op.async-one-type-arg", context, base, 204,
                 op_args1(a_type));
  case_operation("op.fs-stat-none", context, std::nullopt, 43, {});
  case_operation("op.text-from-bytes-none", context, std::nullopt, 31, {});
  case_operation("op.vec-len-none", context, std::nullopt, 36,
                 op_args1(a_type));
  case_operation("op.uid-none", context, std::nullopt, 64, {});
  case_operation("op.thread-spawn-none", context, std::nullopt, 13,
                 op_args1(a_type));
  case_operation("op.empty-module-std", context, empty_module, 43, {});
  case_operation("op.empty-module-builtin", context, empty_module, 36,
                 op_args1(a_type));

  HirModule module = without_nominal(base, 0);
  case_operation("op.missing-result", context, module, 31, {});
  module = keep_parameters(base, 0, 1);
  case_operation("op.result-wrong-arity", context, module, 31, {});
  module = copy_module(base);
  module.nominals.push_back(module.nominals[0]);
  case_operation("op.duplicate-result", context, module, 31, {});
  module = copy_module(base);
  own_resolved(module).symbols[3].name = "Bad";
  case_operation("op.result-variant-name", context, module, 31, {});
  module = copy_module(base);
  module.nominals[0].variants[0].payload.clear();
  case_operation("op.result-payload-count", context, module, 31, {});
  module = copy_module(base);
  module.nominals[0].variants[0].payload[0].type = a_type;
  case_operation("op.result-payload-type", context, module, 31, {});
  module = copy_module(base);
  module.nominals[0].kind = TypeKind::NominalStruct;
  case_operation("op.result-kind", context, module, 31, {});
  module = copy_module(base);
  own_resolved(module).symbols[0].exported = false;
  case_operation("op.result-private", context, module, 31, {});
  module = copy_module(base);
  own_project(module).modules[0].binding = BindingModuleRef{0, 0};
  case_operation("op.result-binding", context, module, 31, {});
  module = copy_module(base);
  own_project(module).modules[0].logical_path = "std/other";
  case_operation("op.result-path", context, module, 31, {});
  module = copy_module(base);
  own_resolved(module).symbols[0].name = "Res";
  case_operation("op.result-name", context, module, 31, {});
  module = copy_module(base);
  own_resolved(module).symbols[0].kind = SymbolKind::value;
  case_operation("op.result-symbol-kind", context, module, 31, {});
  module = copy_module(base);
  module.nominals[0].variants.pop_back();
  case_operation("op.result-variant-count", context, module, 31, {});

  module = without_nominal(base, 1);
  case_operation("op.missing-option", context, module, 40, op_args1(a_type));
  module = keep_parameters(base, 1, 0);
  case_operation("op.option-wrong-arity", context, module, 40,
                 op_args1(a_type));
  module = copy_module(base);
  own_resolved(module).symbols[7].name = "No";
  case_operation("op.option-variant-name", context, module, 40,
                 op_args1(a_type));
  module = copy_module(base);
  module.nominals[1].variants[1].payload[0].type = a_type;
  case_operation("op.option-payload-param", context, module, 40,
                 op_args1(a_type));
  module = copy_module(base);
  module.nominals[1].variants[0].payload.push_back(field_decl(7, a_type));
  case_operation("op.option-none-payload", context, module, 40,
                 op_args1(a_type));

  module = without_nominal(base, 2);
  case_operation("op.pipe-missing-file", context, module, 61, {});
  module = keep_fields(base, 5, 12);
  case_operation("op.fileinfo-field-count", context, module, 43, {});
  module = copy_module(base);
  own_resolved(module).symbols[module.nominals[5].fields[0].symbol.value].name =
      "dev";
  case_operation("op.fileinfo-field-name", context, module, 43, {});
  module = copy_module(base);
  module.nominals[5].fields[0].type = a_type;
  case_operation("op.fileinfo-field-type", context, module, 43, {});
  module = copy_module(base);
  module.nominals[5].kind = TypeKind::NominalEnum;
  case_operation("op.fileinfo-kind", context, module, 43, {});
  module = copy_module(base);
  own_resolved(module).symbols[module.nominals[3].fields[0].symbol.value].name =
      "sec";
  case_operation("op.timespec-field", context, module, 74, {});
  module = copy_module(base);
  own_resolved(module).symbols[module.nominals[12].fields[0].symbol.value].name =
      "frac";
  case_operation("op.fraction-field", context, module, 133, {});
  module = copy_module(base);
  own_resolved(module).symbols[module.nominals[13].fields[1].symbol.value].name =
      "code";
  case_operation("op.waitstatus-field", context, module, 153, {});
  module = copy_module(base);
  own_resolved(module).symbols[module.nominals[14].fields[6].symbol.value].name =
      "data";
  case_operation("op.signalinfo-field", context, module, 156, {});
  module = copy_module(base);
  own_resolved(module).symbols[module.nominals[8].fields[0].symbol.value].name =
      "sock";
  case_operation("op.accepted-field", context, module, 89, {});
  module = copy_module(base);
  own_resolved(module).symbols[module.nominals[9].fields[0].symbol.value].name =
      "total";
  case_operation("op.received-field", context, module, 93, {});
  module = copy_module(base);
  own_resolved(module).symbols[module.nominals[10].fields[1].symbol.value].name =
      "next";
  case_operation("op.pair-field", context, module, 101, {});
  module = copy_module(base);
  own_resolved(module).symbols[module.nominals[11].fields[0].symbol.value].name =
      "handle";
  case_operation("op.pollfd-field", context, module, 102, {});
  module = copy_module(base);
  own_resolved(module).symbols[module.nominals[7].fields[0].symbol.value].name =
      "fam";
  case_operation("op.address-field", context, module, 95, {});
  case_operation("op.buffer-element", context, base, 193,
                 op_args1(string_type));
  dump_table("system", context);
}

HirModule empty_module_default() {
  HirModule module;
  auto resolved = std::make_shared<ResolvedProject>();
  resolved->project = std::make_shared<Project>();
  module.resolved = resolved;
  module.identity = ModuleId{0};
  module.config = Config{Backend::native, 64};
  return module;
}

} // namespace

int main() {
  emit("operation_id woozy-SH06f-foreign-signature-20261113");
  emit("protocol woozy-selfhost-foreign-signature/1");
  run_python_phase();
  run_c_phase();
  run_private_phase();
  run_system_phase();
  return 0;
}
