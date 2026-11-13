// Test-only oracle for the pure-Wz type table.
//
// This program exercises the frozen C++ lang::hir::TypeContext,
// substitute() and type_patterns_overlap() directly.  It is linked against
// the already-built Root liblang_hir.a (plus its normal link dependencies);
// the test runner never rebuilds the C++ compiler.  Its printed trace must
// match src/main.wz byte-for-byte.
#include "lang/hir/hir.hpp"

#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using lang::ForeignBindingId;
using lang::SymbolId;
using lang::TypeId;
using namespace lang::hir;

namespace {

Type scalar(TypeKind kind, unsigned bits) {
  Type value;
  value.kind = kind;
  value.bits = bits;
  return value;
}

Type nominal(TypeKind kind, uint64_t declaration,
             std::vector<TypeId> type_args = {}) {
  Type value;
  value.kind = kind;
  value.declaration = SymbolId{declaration};
  value.type_args = std::move(type_args);
  return value;
}

Type foreign(TypeKind kind, uint64_t declaration, uint64_t binding) {
  Type value;
  value.kind = kind;
  value.declaration = SymbolId{declaration};
  value.binding = ForeignBindingId{binding};
  return value;
}

Type collection(TypeKind kind, std::vector<TypeId> type_args) {
  Type value;
  value.kind = kind;
  value.type_args = std::move(type_args);
  return value;
}

Type c_pointer(TypeKind kind, unsigned bits, std::vector<TypeId> type_args) {
  Type value;
  value.kind = kind;
  value.bits = bits;
  value.type_args = std::move(type_args);
  return value;
}

Type c_declared(TypeKind kind, uint32_t c_type) {
  Type value;
  value.kind = kind;
  value.c_type = c_type;
  return value;
}

Type trait_object(uint64_t declaration, std::vector<TypeId> type_args) {
  Type value;
  value.kind = TypeKind::TraitObject;
  value.declaration = SymbolId{declaration};
  value.type_args = std::move(type_args);
  return value;
}

ParameterType param(ParamMode mode, TypeId type) {
  return ParameterType{mode, type};
}

std::vector<TypeId> ids1(TypeId a) { return {a}; }
std::vector<TypeId> ids2(TypeId a, TypeId b) { return {a, b}; }

Type function2(ParamMode mode1, TypeId type1, ParamMode mode2, TypeId type2,
               TypeId result, std::optional<TypeId> throws_type) {
  Type value;
  value.kind = TypeKind::Function;
  value.parameters = {param(mode1, type1), param(mode2, type2)};
  value.result = result;
  value.throws_type = throws_type;
  return value;
}

Type alias_function(TypeId i32_id, TypeId i64_id) {
  return function2(ParamMode::ordinary, i32_id, ParamMode::inout, i64_id,
                   i32_id, std::nullopt);
}

Type alias_nominal(TypeId i32_id, TypeId i64_id) {
  return nominal(TypeKind::NominalStruct, 42, ids2(i32_id, i64_id));
}

uint64_t safe_intern(TypeContext &ctx, Type value) {
  try {
    return ctx.intern(std::move(value)).value;
  } catch (const std::invalid_argument &) {
    return 0;
  }
}

uint64_t safe_substitute(TypeId id, const Substitutions &subs,
                         TypeContext &ctx) {
  try {
    return substitute(id, subs, ctx).value;
  } catch (const std::invalid_argument &) {
    return 0;
  }
}

int32_t safe_overlap(TypeId left, TypeId right, const TypeContext &ctx) {
  try {
    return type_patterns_overlap(left, right, ctx) ? 1 : 0;
  } catch (const std::invalid_argument &) {
    return -1;
  }
}

Type find_type(const TypeContext &ctx, TypeId id) {
  if (const auto *value = ctx.find(id)) {
    return *value;
  }
  return Type{};
}

void print_id(const std::string &label, uint64_t value) {
  std::cout << label << "=" << value << "\n";
}

void print_i32(const std::string &label, int32_t value) {
  std::cout << label << "=" << value << "\n";
}

void print_flag(const std::string &label, bool value) {
  std::cout << label << "=" << (value ? 1 : 0) << "\n";
}

void print_overlap(const std::string &label, TypeId left, TypeId right,
                   const TypeContext &ctx) {
  print_i32(label, safe_overlap(left, right, ctx));
}

} // namespace

int main() {
  TypeContext ctx;
  print_id("size0", ctx.size());
  print_flag("find0", ctx.find(TypeId{}) != nullptr);
  print_flag("find9999", ctx.find(TypeId{9999}) != nullptr);

  Type unit;
  unit.kind = TypeKind::Unit;
  const auto unit_id = safe_intern(ctx, unit);
  print_id("unit", unit_id);
  print_id("unit_dup", safe_intern(ctx, unit));

  for (unsigned bits : {8u, 16u, 32u, 64u}) {
    print_id("si" + std::to_string(bits),
             safe_intern(ctx, scalar(TypeKind::SignedInt, bits)));
  }
  for (unsigned bits : {8u, 16u, 32u, 64u}) {
    print_id("ui" + std::to_string(bits),
             safe_intern(ctx, scalar(TypeKind::UnsignedInt, bits)));
  }
  for (unsigned bits : {32u, 64u}) {
    print_id("f" + std::to_string(bits),
             safe_intern(ctx, scalar(TypeKind::Float, bits)));
  }
  const auto i8_id = TypeId{safe_intern(ctx, scalar(TypeKind::SignedInt, 8))};
  const auto i16_id = TypeId{safe_intern(ctx, scalar(TypeKind::SignedInt, 16))};
  const auto i32_id = TypeId{safe_intern(ctx, scalar(TypeKind::SignedInt, 32))};
  const auto i64_id = TypeId{safe_intern(ctx, scalar(TypeKind::SignedInt, 64))};
  const auto u8_id = TypeId{safe_intern(ctx, scalar(TypeKind::UnsignedInt, 8))};
  const auto u16_id =
      TypeId{safe_intern(ctx, scalar(TypeKind::UnsignedInt, 16))};
  const auto u32_id =
      TypeId{safe_intern(ctx, scalar(TypeKind::UnsignedInt, 32))};
  const auto u64_id =
      TypeId{safe_intern(ctx, scalar(TypeKind::UnsignedInt, 64))};
  const auto f32_id = TypeId{safe_intern(ctx, scalar(TypeKind::Float, 32))};
  const auto f64_id = TypeId{safe_intern(ctx, scalar(TypeKind::Float, 64))};

  Type bool_type;
  bool_type.kind = TypeKind::Bool;
  const auto bool_id = TypeId{safe_intern(ctx, bool_type)};
  print_id("bool", bool_id.value);
  Type string_type;
  string_type.kind = TypeKind::String;
  const auto string_id = TypeId{safe_intern(ctx, string_type)};
  print_id("string", string_id.value);
  Type isize_type;
  isize_type.kind = TypeKind::Isize;
  const auto isize_id = TypeId{safe_intern(ctx, isize_type)};
  print_id("isize", isize_id.value);
  Type usize_type;
  usize_type.kind = TypeKind::Usize;
  const auto usize_id = TypeId{safe_intern(ctx, usize_type)};
  print_id("usize", usize_id.value);
  Type error_type;
  error_type.kind = TypeKind::Error;
  print_id("error", safe_intern(ctx, error_type));
  Type thread_handle;
  thread_handle.kind = TypeKind::ThreadHandle;
  print_id("thread_handle", safe_intern(ctx, thread_handle));
  Type mutex_handle;
  mutex_handle.kind = TypeKind::MutexHandle;
  print_id("mutex_handle", safe_intern(ctx, mutex_handle));

  Type fn1;
  fn1.kind = TypeKind::Function;
  fn1.result = TypeId{unit_id};
  const auto fn1_id = TypeId{safe_intern(ctx, fn1)};
  print_id("fn1", fn1_id.value);
  print_id("fn1_dup", safe_intern(ctx, fn1));

  const auto fn2 = function2(ParamMode::ordinary, i8_id, ParamMode::inout,
                              u16_id, i32_id, i64_id);
  const auto fn2_id = TypeId{safe_intern(ctx, fn2)};
  print_id("fn2", fn2_id.value);
  print_id("fn2_dup", safe_intern(ctx, fn2));

  const auto ns0 = nominal(TypeKind::NominalStruct, 0, ids1(i32_id));
  const auto ns0_id = TypeId{safe_intern(ctx, ns0)};
  print_id("ns0", ns0_id.value);
  const auto ns1 = nominal(TypeKind::NominalStruct, 1, ids1(i32_id));
  const auto ns1_id = TypeId{safe_intern(ctx, ns1)};
  print_id("ns1", ns1_id.value);
  const auto ne1 = nominal(TypeKind::NominalEnum, 1);
  const auto ne1_id = TypeId{safe_intern(ctx, ne1)};
  print_id("ne1", ne1_id.value);
  const auto tp7 = nominal(TypeKind::TypeParameter, 7);
  const auto tp7_id = TypeId{safe_intern(ctx, tp7)};
  print_id("tp7", tp7_id.value);
  const auto tp8 = nominal(TypeKind::TypeParameter, 8);
  const auto tp8_id = TypeId{safe_intern(ctx, tp8)};
  print_id("tp8", tp8_id.value);
  const auto tp9 = nominal(TypeKind::TypeParameter, 9);
  const auto tp9_id = TypeId{safe_intern(ctx, tp9)};
  print_id("tp9", tp9_id.value);

  const auto vec_i32 = collection(TypeKind::Vec, ids1(i32_id));
  const auto vec_i32_id = TypeId{safe_intern(ctx, vec_i32)};
  print_id("vec_i32", vec_i32_id.value);
  const auto vec_i64 = collection(TypeKind::Vec, ids1(i64_id));
  const auto vec_i64_id = TypeId{safe_intern(ctx, vec_i64)};
  print_id("vec_i64", vec_i64_id.value);
  const auto vec_p = collection(TypeKind::Vec, ids1(tp7_id));
  const auto vec_p_id = TypeId{safe_intern(ctx, vec_p)};
  print_id("vec_p", vec_p_id.value);
  const auto slice_i32 = collection(TypeKind::Slice, ids1(i32_id));
  const auto slice_i32_id = TypeId{safe_intern(ctx, slice_i32)};
  print_id("slice_i32", slice_i32_id.value);
  const auto slice_i64 = collection(TypeKind::Slice, ids1(i64_id));
  const auto slice_i64_id = TypeId{safe_intern(ctx, slice_i64)};
  print_id("slice_i64", slice_i64_id.value);
  const auto weak_i32 = collection(TypeKind::Weak, ids1(i32_id));
  const auto weak_i32_id = TypeId{safe_intern(ctx, weak_i32)};
  print_id("weak_i32", weak_i32_id.value);
  const auto weak_p = collection(TypeKind::Weak, ids1(tp7_id));
  const auto weak_p_id = TypeId{safe_intern(ctx, weak_p)};
  print_id("weak_p", weak_p_id.value);
  const auto task_p_i64 = collection(TypeKind::Task, ids2(tp7_id, i64_id));
  const auto task_p_i64_id = TypeId{safe_intern(ctx, task_p_i64)};
  print_id("task_p_i64", task_p_i64_id.value);
  const auto task_i64_i64 = collection(TypeKind::Task, ids2(i64_id, i64_id));
  const auto task_i64_i64_id = TypeId{safe_intern(ctx, task_i64_i64)};
  print_id("task_i64_i64", task_i64_i64_id.value);
  const auto trait3_i32 = trait_object(3, ids1(i32_id));
  const auto trait3_i32_id = TypeId{safe_intern(ctx, trait3_i32)};
  print_id("trait3_i32", trait3_i32_id.value);
  const auto trait3_i64 = trait_object(3, ids1(i64_id));
  const auto trait3_i64_id = TypeId{safe_intern(ctx, trait3_i64)};
  print_id("trait3_i64", trait3_i64_id.value);
  const auto trait3_bool = trait_object(3, ids1(bool_id));
  const auto trait3_bool_id = TypeId{safe_intern(ctx, trait3_bool)};
  print_id("trait3_bool", trait3_bool_id.value);

  const auto fopaque57 = foreign(TypeKind::ForeignOpaque, 5, 7);
  const auto fopaque57_id = TypeId{safe_intern(ctx, fopaque57)};
  print_id("fopaque57", fopaque57_id.value);
  const auto fopaque58 = foreign(TypeKind::ForeignOpaque, 5, 8);
  const auto fopaque58_id = TypeId{safe_intern(ctx, fopaque58)};
  print_id("fopaque58", fopaque58_id.value);
  const auto fcall57 = foreign(TypeKind::ForeignCallable, 5, 7);
  const auto fcall57_id = TypeId{safe_intern(ctx, fcall57)};
  print_id("fcall57", fcall57_id.value);
  const auto fcall58 = foreign(TypeKind::ForeignCallable, 5, 8);
  const auto fcall58_id = TypeId{safe_intern(ctx, fcall58)};
  print_id("fcall58", fcall58_id.value);

  const auto cptr0 = c_pointer(TypeKind::CForeignPointer, 0, ids1(u8_id));
  const auto cptr0_id = TypeId{safe_intern(ctx, cptr0)};
  print_id("cptr0", cptr0_id.value);
  const auto cptr1 = c_pointer(TypeKind::CForeignPointer, 1, ids1(u8_id));
  const auto cptr1_id = TypeId{safe_intern(ctx, cptr1)};
  print_id("cptr1", cptr1_id.value);
  const auto cbuf0 = c_pointer(TypeKind::CForeignBuffer, 0, ids1(u8_id));
  const auto cbuf0_id = TypeId{safe_intern(ctx, cbuf0)};
  print_id("cbuf0", cbuf0_id.value);
  const auto crecord4 = c_declared(TypeKind::CForeignRecord, 4);
  const auto crecord4_id = TypeId{safe_intern(ctx, crecord4)};
  print_id("crecord4", crecord4_id.value);
  const auto crecord5 = c_declared(TypeKind::CForeignRecord, 5);
  const auto crecord5_id = TypeId{safe_intern(ctx, crecord5)};
  print_id("crecord5", crecord5_id.value);
  const auto cresource5 = c_declared(TypeKind::CForeignResource, 5);
  const auto cresource5_id = TypeId{safe_intern(ctx, cresource5)};
  print_id("cresource5", cresource5_id.value);
  const auto ccallback6 = c_declared(TypeKind::CForeignCallback, 6);
  const auto ccallback6_id = TypeId{safe_intern(ctx, ccallback6)};
  print_id("ccallback6", ccallback6_id.value);
  const auto copaque7 = c_declared(TypeKind::CForeignOpaque, 7);
  const auto copaque7_id = TypeId{safe_intern(ctx, copaque7)};
  print_id("copaque7", copaque7_id.value);

  const Config cfg64{};
  const Config cfg32{Backend::native, 32};
  print_id("width_i8",
           bit_width(scalar(TypeKind::SignedInt, 8), cfg64));
  print_id("width_i32",
           bit_width(scalar(TypeKind::SignedInt, 32), cfg64));
  print_id("width_u64",
           bit_width(scalar(TypeKind::UnsignedInt, 64), cfg32));
  print_id("width_f32", bit_width(scalar(TypeKind::Float, 32), cfg64));
  print_id("width_f64", bit_width(scalar(TypeKind::Float, 64), cfg64));
  print_id("width_isize64", bit_width(isize_type, cfg64));
  print_id("width_isize32", bit_width(isize_type, cfg32));
  print_id("width_usize64", bit_width(usize_type, cfg64));
  print_id("width_usize32", bit_width(usize_type, cfg32));

  print_id("bad_i0", safe_intern(ctx, scalar(TypeKind::SignedInt, 0)));
  print_id("bad_i17", safe_intern(ctx, scalar(TypeKind::SignedInt, 17)));
  print_id("bad_u128", safe_intern(ctx, scalar(TypeKind::UnsignedInt, 128)));
  print_id("bad_f16", safe_intern(ctx, scalar(TypeKind::Float, 16)));
  Type bad_fn_result;
  bad_fn_result.kind = TypeKind::Function;
  print_id("bad_fn_result0", safe_intern(ctx, bad_fn_result));
  Type bad_fn_param;
  bad_fn_param.kind = TypeKind::Function;
  bad_fn_param.parameters = {param(ParamMode::ordinary, TypeId{9999})};
  bad_fn_param.result = i32_id;
  print_id("bad_fn_param_ref", safe_intern(ctx, bad_fn_param));
  Type bad_fn_throws;
  bad_fn_throws.kind = TypeKind::Function;
  bad_fn_throws.result = TypeId{unit_id};
  bad_fn_throws.throws_type = TypeId{9999};
  print_id("bad_fn_throws_ref", safe_intern(ctx, bad_fn_throws));
  Type mut_unit_result = scalar(TypeKind::Unit, 0);
  mut_unit_result.result = i32_id;
  print_id("bad_unit_result", safe_intern(ctx, mut_unit_result));
  Type mut_unit_bits = scalar(TypeKind::Unit, 1);
  print_id("bad_unit_bits", safe_intern(ctx, mut_unit_bits));
  Type mut_unit_decl;
  mut_unit_decl.kind = TypeKind::Unit;
  mut_unit_decl.declaration = SymbolId{1};
  print_id("bad_unit_decl", safe_intern(ctx, mut_unit_decl));
  print_id("bad_vec0", safe_intern(ctx, collection(TypeKind::Vec, {})));
  print_id("bad_vec2",
           safe_intern(ctx, collection(TypeKind::Vec, ids2(i8_id, i16_id))));
  print_id("bad_slice0",
           safe_intern(ctx, collection(TypeKind::Slice, {})));
  print_id("bad_weak0", safe_intern(ctx, collection(TypeKind::Weak, {})));
  print_id("bad_task1",
           safe_intern(ctx, collection(TypeKind::Task, ids1(i32_id))));
  print_id("bad_task3", safe_intern(ctx,
                                    collection(TypeKind::Task,
                                               {i32_id, i64_id, bool_id})));
  print_id("bad_trait_decl0",
           safe_intern(ctx, trait_object(0, ids1(i32_id))));
  auto mut_trait_bits = trait_object(3, ids1(i32_id));
  mut_trait_bits.bits = 1;
  print_id("bad_trait_bits", safe_intern(ctx, mut_trait_bits));
  auto mut_trait_params = trait_object(3, ids1(i32_id));
  mut_trait_params.parameters = {param(ParamMode::ordinary, i32_id)};
  print_id("bad_trait_params", safe_intern(ctx, mut_trait_params));
  print_id("bad_crecord0",
           safe_intern(ctx, c_declared(TypeKind::CForeignRecord, 0)));
  auto mut_crecord_args = c_declared(TypeKind::CForeignRecord, 4);
  mut_crecord_args.type_args = ids1(i32_id);
  print_id("bad_crecord_args", safe_intern(ctx, mut_crecord_args));
  auto mut_crecord_bits = c_declared(TypeKind::CForeignRecord, 4);
  mut_crecord_bits.bits = 1;
  print_id("bad_crecord_bits", safe_intern(ctx, mut_crecord_bits));
  print_id("bad_cptr_args",
           safe_intern(ctx, c_pointer(TypeKind::CForeignPointer, 0, {})));
  print_id("bad_cptr_bits",
           safe_intern(ctx,
                       c_pointer(TypeKind::CForeignPointer, 2, ids1(u8_id))));
  auto mut_cptr_ctype = c_pointer(TypeKind::CForeignPointer, 0, ids1(u8_id));
  mut_cptr_ctype.c_type = 1;
  print_id("bad_cptr_ctype", safe_intern(ctx, mut_cptr_ctype));
  auto mut_nominal_binding = nominal(TypeKind::NominalStruct, 1);
  mut_nominal_binding.binding = ForeignBindingId{1};
  print_id("bad_nominal_binding", safe_intern(ctx, mut_nominal_binding));
  auto mut_foreign_args = foreign(TypeKind::ForeignOpaque, 5, 7);
  mut_foreign_args.type_args = ids1(i32_id);
  print_id("bad_foreign_args", safe_intern(ctx, mut_foreign_args));
  auto mut_tp_binding = nominal(TypeKind::TypeParameter, 7);
  mut_tp_binding.binding = ForeignBindingId{1};
  print_id("bad_tp_binding", safe_intern(ctx, mut_tp_binding));
  print_id("bad_vec_arg_ref",
           safe_intern(ctx, collection(TypeKind::Vec, ids1(TypeId{9999}))));
  print_id("bad_nominal_arg_ref",
           safe_intern(ctx, nominal(TypeKind::NominalStruct, 1,
                                    ids1(TypeId{9999}))));
  print_id("bad_cptr_arg_ref",
           safe_intern(ctx, c_pointer(TypeKind::CForeignPointer, 0,
                                      ids1(TypeId{9999}))));
  print_id("invalid_size", ctx.size());

  auto alias_fn = alias_function(i32_id, i64_id);
  const auto alias_fn_expected = alias_function(i32_id, i64_id);
  const auto alias_fn_id = TypeId{safe_intern(ctx, alias_fn)};
  print_id("alias_fn", alias_fn_id.value);
  auto alias_nom = alias_nominal(i32_id, i64_id);
  const auto alias_nom_expected = alias_nominal(i32_id, i64_id);
  const auto alias_nom_id = TypeId{safe_intern(ctx, alias_nom)};
  print_id("alias_nom", alias_nom_id.value);

  alias_fn.parameters[0].type = i8_id;
  alias_fn.parameters.push_back(param(ParamMode::move, f64_id));
  alias_nom.type_args[0] = i8_id;
  alias_nom.type_args.push_back(f64_id);
  print_id("alias_fn_input_mutated", safe_intern(ctx, alias_fn));
  print_id("alias_nom_input_mutated", safe_intern(ctx, alias_nom));
  auto alias_fn_found = find_type(ctx, alias_fn_id);
  auto alias_nom_found = find_type(ctx, alias_nom_id);
  print_flag("alias_fn_after_input",
             alias_fn_found == alias_fn_expected);
  print_flag("alias_nom_after_input",
             alias_nom_found == alias_nom_expected);
  print_id("alias_fn_param0", alias_fn_found.parameters[0].type.value);
  print_id("alias_fn_param_len", alias_fn_found.parameters.size());
  print_id("alias_nom_arg0", alias_nom_found.type_args[0].value);
  print_id("alias_nom_arg_len", alias_nom_found.type_args.size());

  alias_fn_found.parameters[0].type = i8_id;
  alias_fn_found.parameters.push_back(param(ParamMode::move, f64_id));
  alias_nom_found.type_args[0] = i8_id;
  alias_nom_found.type_args.push_back(f64_id);
  const auto alias_fn_again = find_type(ctx, alias_fn_id);
  const auto alias_nom_again = find_type(ctx, alias_nom_id);
  print_flag("alias_fn_found_isolated", alias_fn_again == alias_fn_expected);
  print_flag("alias_nom_found_isolated", alias_nom_again == alias_nom_expected);

  const auto old_size = ctx.size();
  auto clone = ctx;
  print_id("clone_size", clone.size());
  const auto clone_only = nominal(TypeKind::NominalStruct, 777, ids1(i32_id));
  const auto clone_new = safe_intern(clone, clone_only);
  print_id("clone_new", clone_new);
  print_id("clone_size_after", clone.size());
  print_id("ctx_size_after_clone", ctx.size());
  const auto ctx_new = safe_intern(ctx, clone_only);
  print_id("ctx_new", ctx_new);
  print_flag("ctx_new_is_old_plus_one", ctx_new == old_size + 1);
  print_flag("clone_old_alias",
             find_type(clone, alias_fn_id) == alias_fn_expected);
  print_flag("ctx_old_alias", find_type(ctx, alias_fn_id) == alias_fn_expected);

  const auto struct_p =
      nominal(TypeKind::NominalStruct, 100, ids1(tp7_id));
  const auto struct_p_id = TypeId{safe_intern(ctx, struct_p)};
  print_id("struct_p", struct_p_id.value);
  const auto vec_struct_p = collection(TypeKind::Vec, ids1(struct_p_id));
  const auto vec_struct_p_id = TypeId{safe_intern(ctx, vec_struct_p)};
  print_id("vec_struct_p", vec_struct_p_id.value);
  const auto fn_p = function2(ParamMode::ordinary, tp7_id, ParamMode::inout,
                              i64_id, tp7_id, tp7_id);
  const auto fn_p_id = TypeId{safe_intern(ctx, fn_p)};
  print_id("fn_p", fn_p_id.value);
  const auto fn_i64 = function2(ParamMode::ordinary, i64_id, ParamMode::inout,
                                i64_id, i64_id, i64_id);
  const auto fn_i64_id = TypeId{safe_intern(ctx, fn_i64)};
  print_id("fn_i64", fn_i64_id.value);
  const auto fn_i64_inout =
      function2(ParamMode::inout, i64_id, ParamMode::inout, i64_id, i64_id,
                i64_id);
  const auto fn_i64_inout_id = TypeId{safe_intern(ctx, fn_i64_inout)};
  print_id("fn_i64_inout", fn_i64_inout_id.value);
  const auto fn_i64_no_throws =
      function2(ParamMode::ordinary, i64_id, ParamMode::inout, i64_id,
                i64_id, std::nullopt);
  const auto fn_i64_no_throws_id = TypeId{safe_intern(ctx, fn_i64_no_throws)};
  print_id("fn_i64_no_throws", fn_i64_no_throws_id.value);
  const auto fn_i64_bool =
      function2(ParamMode::ordinary, i64_id, ParamMode::inout, i64_id,
                bool_id, i64_id);
  const auto fn_i64_bool_id = TypeId{safe_intern(ctx, fn_i64_bool)};
  print_id("fn_i64_bool", fn_i64_bool_id.value);

  const Substitutions substitutions{{SymbolId{7}, i64_id},
                                    {SymbolId{8}, i32_id}};
  print_id("sub_tp7", safe_substitute(tp7_id, substitutions, ctx));
  print_id("sub_tp8", safe_substitute(tp8_id, substitutions, ctx));
  print_id("sub_tp9", safe_substitute(tp9_id, substitutions, ctx));
  const auto sub_vec_p = safe_substitute(vec_p_id, substitutions, ctx);
  print_id("sub_vec_p", sub_vec_p);
  print_flag("sub_vec_p_is_vec_i64", sub_vec_p == vec_i64_id.value);
  const auto sub_struct_p = safe_substitute(struct_p_id, substitutions, ctx);
  const auto struct_i64 = nominal(TypeKind::NominalStruct, 100, ids1(i64_id));
  const auto struct_i64_id = TypeId{safe_intern(ctx, struct_i64)};
  print_id("sub_struct_p", sub_struct_p);
  print_flag("sub_struct_p_is_struct_i64",
             sub_struct_p == struct_i64_id.value);
  const auto sub_vec_struct_p =
      safe_substitute(vec_struct_p_id, substitutions, ctx);
  const auto vec_struct_i64 =
      collection(TypeKind::Vec, ids1(struct_i64_id));
  const auto vec_struct_i64_id = TypeId{safe_intern(ctx, vec_struct_i64)};
  print_id("sub_vec_struct_p", sub_vec_struct_p);
  print_flag("sub_vec_struct_p_is_expected",
             sub_vec_struct_p == vec_struct_i64_id.value);
  const auto sub_task_p_i64 =
      safe_substitute(task_p_i64_id, substitutions, ctx);
  print_flag("sub_task_p_i64_is_expected",
             sub_task_p_i64 == task_i64_i64_id.value);
  const auto sub_fn_p = safe_substitute(fn_p_id, substitutions, ctx);
  print_id("sub_fn_p", sub_fn_p);
  const auto sub_fn_p_type = find_type(ctx, TypeId{sub_fn_p});
  print_flag("sub_fn_result", sub_fn_p_type.result == i64_id);
  print_flag("sub_fn_param0", sub_fn_p_type.parameters[0].type == i64_id);
  print_flag("sub_fn_param1", sub_fn_p_type.parameters[1].type == i64_id);
  print_flag("sub_fn_throws",
             sub_fn_p_type.throws_type &&
                 *sub_fn_p_type.throws_type == i64_id);
  const auto sub_fn_p_again =
      safe_substitute(fn_p_id, substitutions, ctx);
  print_flag("sub_fn_p_deterministic", sub_fn_p_again == sub_fn_p);
  const auto fn_p_after = find_type(ctx, fn_p_id);
  print_flag("sub_original_result", fn_p_after.result == tp7_id);
  print_flag("sub_original_param0", fn_p_after.parameters[0].type == tp7_id);
  print_flag("sub_original_throws",
             fn_p_after.throws_type && *fn_p_after.throws_type == tp7_id);
  print_id("sub_bad0", safe_substitute(TypeId{}, substitutions, ctx));
  print_id("sub_bad9999", safe_substitute(TypeId{9999}, substitutions, ctx));

  print_overlap("overlap_same_unit", TypeId{unit_id}, TypeId{unit_id}, ctx);
  print_overlap("overlap_same_i32", i32_id, i32_id, ctx);
  print_overlap("overlap_i32_i64", i32_id, i64_id, ctx);
  print_overlap("overlap_i32_bool", i32_id, bool_id, ctx);
  print_overlap("overlap_tp_i64", tp7_id, i64_id, ctx);
  print_overlap("overlap_tp_tp", tp7_id, tp8_id, ctx);
  print_overlap("overlap_vec_p_i64", vec_p_id, vec_i64_id, ctx);
  print_overlap("overlap_vec_p_struct_p", vec_p_id, vec_struct_p_id, ctx);
  print_overlap("overlap_tp_vec_p", tp7_id, vec_p_id, ctx);
  print_overlap("overlap_task_p", task_p_i64_id, task_i64_i64_id, ctx);
  const auto task_i64_p_id = TypeId{safe_intern(
      ctx, collection(TypeKind::Task, ids2(i64_id, tp7_id)))};
  print_overlap("overlap_task_swap", task_p_i64_id, task_i64_p_id, ctx);
  print_overlap("overlap_weak_p", weak_p_id, weak_i32_id, ctx);
  print_overlap("overlap_fn_p_i64", fn_p_id, fn_i64_id, ctx);
  print_overlap("overlap_fn_mode", fn_i64_id, fn_i64_inout_id, ctx);
  print_overlap("overlap_fn_throws", fn_i64_id, fn_i64_no_throws_id, ctx);
  print_overlap("overlap_fn_result", fn_i64_id, fn_i64_bool_id, ctx);
  const auto ns2_id = TypeId{safe_intern(
      ctx, nominal(TypeKind::NominalStruct, 2, ids1(i32_id)))};
  print_overlap("overlap_ns1_ns2", ns1_id, ns2_id, ctx);
  print_overlap("overlap_ns_ne", ns1_id, ne1_id, ctx);
  print_overlap("overlap_foreign_kind", fopaque57_id, fcall57_id, ctx);
  print_overlap("overlap_foreign_binding", fopaque57_id, fopaque58_id, ctx);
  print_overlap("overlap_foreign_callable_binding", fcall57_id, fcall58_id,
                ctx);
  print_overlap("overlap_trait_same", trait3_i32_id, trait3_i32_id, ctx);
  print_overlap("overlap_trait_arg", trait3_i32_id, trait3_i64_id, ctx);
  print_overlap("overlap_trait_arg_bool", trait3_i32_id, trait3_bool_id, ctx);
  print_overlap("overlap_slice_vec", slice_i64_id, vec_i64_id, ctx);
  print_overlap("overlap_crecord_ctype", crecord4_id, cresource5_id, ctx);
  print_overlap("overlap_crecord_ctype_ignored", crecord4_id, crecord5_id,
                ctx);
  print_overlap("overlap_cptr_bits", cptr0_id, cptr1_id, ctx);
  print_overlap("overlap_callback_opaque", ccallback6_id, copaque7_id, ctx);
  print_i32("overlap_bad0", safe_overlap(TypeId{}, i32_id, ctx));
  print_i32("overlap_bad9999", safe_overlap(TypeId{9999}, i32_id, ctx));
  print_i32("overlap_bad_right", safe_overlap(i32_id, TypeId{9999}, ctx));

  print_flag("final_alias_fn",
             find_type(ctx, alias_fn_id) == alias_fn_expected);
  print_flag("final_alias_nom",
             find_type(ctx, alias_nom_id) == alias_nom_expected);
  print_id("final_size", ctx.size());
  return 0;
}
