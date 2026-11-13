// Test-only C++ oracle for the Wz pattern coverage port (SH06e).
//
// This executable links the real src/hir/pattern_analysis.cpp translation unit
// and a tiny support TU; it never links, executes or is executed by the Wz
// compiler.  It builds the same fixtures as selfhost/tests/pattern/src/main.wz
// and prints a canonical dump.  Both dumps are compared line by line by
// run.py, including constructor order, substituted field TypeIds, domain
// classification, usefulness/exhaustiveness results and every error text.
#include "lang/hir/hir.hpp"

#include "../../../src/hir/pattern_analysis.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {
using namespace lang;
using namespace lang::hir;

std::string hex_byte(unsigned char byte) {
  static const char digits[] = "0123456789ABCDEF";
  std::string out = "\\x";
  out += digits[byte >> 4];
  out += digits[byte & 0xf];
  return out;
}

// Deterministic quoting shared with the Wz driver: printable ASCII stays,
// backslash/quote are escaped, every other byte becomes \xNN.
std::string quote(const std::string &value) {
  std::string out = "\"";
  for (unsigned char byte : value) {
    if (byte == '\\') {
      out += "\\\\";
    } else if (byte == '"') {
      out += "\\\"";
    } else if (byte >= 0x20 && byte <= 0x7e) {
      out += static_cast<char>(byte);
    } else {
      out += hex_byte(byte);
    }
  }
  out += '"';
  return out;
}

std::string number(uint64_t value) { return std::to_string(value); }

std::string ids(const std::vector<TypeId> &values) {
  std::string out;
  for (size_t index = 0; index < values.size(); ++index) {
    if (index) out += ",";
    out += number(values[index].value);
  }
  return out;
}

std::string mode_id(ParamMode mode) {
  switch (mode) {
  case ParamMode::ordinary: return "0";
  case ParamMode::inout: return "1";
  case ParamMode::move: return "2";
  }
  return "?";
}

std::vector<std::string> records;
std::vector<std::string> type_records;

void record(const std::string &line) { records.push_back(line); }

TypeId intern(TypeContext &types, TypeKind kind, unsigned bits = 0,
              uint64_t declaration = 0, std::vector<TypeId> args = {}) {
  Type value;
  value.kind = kind;
  value.bits = bits;
  value.declaration = SymbolId{declaration};
  value.type_args = std::move(args);
  return types.intern(std::move(value));
}

std::string type_signature(const TypeContext &types, TypeId id) {
  const Type *value = types.find(id);
  if (!value) return "missing";
  std::string out = number(static_cast<uint64_t>(value->kind)) + ":" +
                    number(value->bits) + ":" +
                    number(value->declaration.value) + ":";
  for (size_t index = 0; index < value->type_args.size(); ++index) {
    if (index) out += ".";
    out += number(value->type_args[index].value);
  }
  return out;
}

std::string fields_text(const TypeContext &types,
                        const std::vector<TypeId> &fields) {
  std::string out;
  for (size_t index = 0; index < fields.size(); ++index) {
    if (index) out += ",";
    out += type_signature(types, fields[index]);
  }
  return out;
}

void dump_type(const Type &value, TypeId id, uint64_t index) {
  std::string params;
  for (size_t slot = 0; slot < value.parameters.size(); ++slot) {
    if (slot) params += ",";
    params += mode_id(value.parameters[slot].mode) + ":" +
              number(value.parameters[slot].type.value);
  }
  type_records.push_back("type " + number(id.value) + " index=" + number(index) +
         " kind=" + number(static_cast<uint64_t>(value.kind)) +
         " bits=" + number(value.bits) +
         " decl=" + number(value.declaration.value) +
         " binding=" + number(value.binding.value) +
         " params=" + params + " result=" + number(value.result.value) +
         " throws=" +
         (value.throws_type ? number(value.throws_type->value) : "none") +
         " args=" + ids(value.type_args) +
         " ctype=" + number(value.c_type));
}

void dump_constructors(const std::string &label, const TypeContext &types,
                       const std::vector<PatternConstructor> &values) {
  record("constructors " + label + " n=" + number(values.size()));
  for (size_t index = 0; index < values.size(); ++index) {
    const auto &value = values[index];
    record("ctor " + label + " " + number(index) + " kind=" +
           number(static_cast<uint64_t>(value.kind)) +
           " variant=" + number(value.variant.value) +
           " key=" + quote(value.literal_key) +
           " fields=" + ids(value.fields) +
           " sigs=" + fields_text(types, value.fields));
  }
}

void dump_domain(const std::string &label, const TypeContext &types,
                 const PatternDomain &value) {
  record("domain " + label + " kind=" +
         number(static_cast<uint64_t>(value.kind)) +
         " bits=" + number(value.bits) +
         " n=" + number(value.constructors.size()));
  for (size_t index = 0; index < value.constructors.size(); ++index) {
    const auto &ctor = value.constructors[index];
    record("ctor " + label + " " + number(index) + " kind=" +
           number(static_cast<uint64_t>(ctor.kind)) +
           " variant=" + number(ctor.variant.value) +
           " key=" + quote(ctor.literal_key) + " fields=" + ids(ctor.fields) +
           " sigs=" + fields_text(types, ctor.fields));
  }
}

void dump_error(const std::string &label, const std::exception &error,
                bool limit) {
  record("error " + label + " " + (limit ? "Limit" : "Invalid") + " " +
         quote(error.what()));
}

template <class Fn>
void guarded(const std::string &label, Fn &&body) {
  try {
    body();
  } catch (const PatternAnalysisLimit &error) {
    dump_error(label, error, true);
  } catch (const std::invalid_argument &error) {
    dump_error(label, error, false);
  } catch (const std::exception &error) {
    dump_error(label, error, false);
  }
}

void test_constructors(const std::string &label, const HirModule &module,
                       TypeId type, TypeContext &types) {
  guarded(label, [&] {
    dump_constructors(label, types, pattern_constructors(module, type, types));
  });
}

void test_domain(const std::string &label, const HirModule &module,
                 TypeId type, TypeContext &types) {
  guarded(label,
          [&] { dump_domain(label, types, pattern_domain(module, type, types)); });
}

Pattern wild(TypeId type) {
  Pattern value;
  value.type = type;
  return value;
}

Pattern binding(TypeId type, uint64_t symbol) {
  Pattern value;
  value.type = type;
  value.binding = SymbolId{symbol};
  return value;
}

Pattern variant(TypeId type, uint64_t symbol, std::vector<Pattern> payload) {
  Pattern value;
  value.type = type;
  value.variant = SymbolId{symbol};
  value.payload = std::move(payload);
  return value;
}

Pattern bool_literal(TypeId type, bool flag) {
  Pattern value;
  value.type = type;
  value.literal = BoolLiteral{flag};
  return value;
}

Pattern int_literal(TypeId type, uint64_t bits) {
  Pattern value;
  value.type = type;
  value.literal = IntegerLiteral{"0", bits};
  return value;
}

Pattern float_literal(TypeId type, uint64_t bits) {
  Pattern value;
  value.type = type;
  value.literal = FloatLiteral{bits};
  return value;
}

Pattern string_literal(TypeId type, std::string text) {
  Pattern value;
  value.type = type;
  value.literal = StringLiteral{std::move(text)};
  return value;
}

struct CoverageFixture {
  CoverageFixture(TypeId scrutinee, const HirModule &module, TypeContext &types,
                  size_t budget)
      : coverage(scrutinee,
                 [&module, &types](TypeId type) {
                   return pattern_domain(module, type, types);
                 },
                 std::stop_token{}, budget) {}

  void add(const std::string &label, const Pattern &value) {
    guarded(label, [&] {
      record("add " + label + " " + (coverage.add(value) ? "1" : "0"));
    });
  }

  void exhaustive(const std::string &label) {
    guarded(label, [&] {
      record("exhaust " + label + " " + (coverage.exhaustive() ? "1" : "0"));
    });
  }

  PatternCoverage coverage;
};


} // namespace

int main() {
  TypeContext types;
  const TypeId unit = intern(types, TypeKind::Unit);
  const TypeId boolean = intern(types, TypeKind::Bool);
  const TypeId i8 = intern(types, TypeKind::SignedInt, 8);
  const TypeId u8 = intern(types, TypeKind::UnsignedInt, 8);
  const TypeId i16 = intern(types, TypeKind::SignedInt, 16);
  const TypeId u16 = intern(types, TypeKind::UnsignedInt, 16);
  const TypeId i32 = intern(types, TypeKind::SignedInt, 32);
  const TypeId u32 = intern(types, TypeKind::UnsignedInt, 32);
  const TypeId i64 = intern(types, TypeKind::SignedInt, 64);
  const TypeId u64 = intern(types, TypeKind::UnsignedInt, 64);
  const TypeId isize = intern(types, TypeKind::Isize);
  const TypeId usize_type = intern(types, TypeKind::Usize);
  const TypeId f32 = intern(types, TypeKind::Float, 32);
  const TypeId f64 = intern(types, TypeKind::Float, 64);
  const TypeId string_type = intern(types, TypeKind::String);
  (void)unit;
  (void)i8;
  (void)u8;
  (void)i16;
  (void)u16;
  (void)u32;
  (void)i64;
  (void)u64;

  const TypeId tp400 = intern(types, TypeKind::TypeParameter, 0, 400);
  const TypeId tp401 = intern(types, TypeKind::TypeParameter, 0, 401);
  const TypeId tp402 = intern(types, TypeKind::TypeParameter, 0, 402);
  const TypeId tp403 = intern(types, TypeKind::TypeParameter, 0, 403);
  const TypeId tp404 = intern(types, TypeKind::TypeParameter, 0, 404);
  const TypeId tp405 = intern(types, TypeKind::TypeParameter, 0, 405);

  const TypeId list_self =
      intern(types, TypeKind::NominalEnum, 0, 104, {tp403});
  const TypeId vec_holder =
      intern(types, TypeKind::Vec, 0, 0, {tp404});
  const TypeId vec_wrap = intern(types, TypeKind::Vec, 0, 0, {tp405});
  const TypeId struct_type = intern(types, TypeKind::NominalStruct, 0, 106);
  const TypeId empty_enum_type = intern(types, TypeKind::NominalEnum, 0, 100);
  const TypeId e1_type = intern(types, TypeKind::NominalEnum, 0, 101);
  const TypeId option_i32 =
      intern(types, TypeKind::NominalEnum, 0, 102, {i32});
  const TypeId option_string =
      intern(types, TypeKind::NominalEnum, 0, 102, {string_type});
  const TypeId pair_i32_f64 =
      intern(types, TypeKind::NominalEnum, 0, 103, {i32, f64});
  const TypeId holder_i32 =
      intern(types, TypeKind::NominalEnum, 0, 105, {i32});
  const TypeId option_option_i32 =
      intern(types, TypeKind::NominalEnum, 0, 102, {option_i32});
  const TypeId list_i32 =
      intern(types, TypeKind::NominalEnum, 0, 104, {i32});
  const TypeId wrap_u8 = intern(types, TypeKind::NominalEnum, 0, 107, {u8});
  const TypeId e1_with_args =
      intern(types, TypeKind::NominalEnum, 0, 101, {i32});
  const TypeId orphan_enum = intern(types, TypeKind::NominalEnum, 0, 777);
  const TypeId vec_i32 = intern(types, TypeKind::Vec, 0, 0, {i32});
  (void)vec_i32;

  auto field = [](uint64_t symbol, TypeId type) {
    return FieldDecl{SymbolId{symbol}, type, false, Source{}};
  };
  auto param = [](uint64_t symbol) {
    return GenericParam{SymbolId{symbol}, std::nullopt, Source{}};
  };
  auto bare_variant = [](uint64_t symbol) {
    return VariantDecl{SymbolId{symbol}, {}, Source{}};
  };

  NominalDecl empty_decl{SymbolId{100}, TypeKind::NominalEnum, {}, {}, {}, Source{}};
  NominalDecl e1_decl{SymbolId{101},
                      TypeKind::NominalEnum,
                      {},
                      {},
                      {bare_variant(200),
                       VariantDecl{SymbolId{201},
                                   {field(300, i32), field(301, boolean)},
                                   Source{}},
                       VariantDecl{SymbolId{202},
                                   {field(302, boolean), field(303, boolean)},
                                   Source{}}},
                      Source{}};
  NominalDecl option_decl{SymbolId{102},
                          TypeKind::NominalEnum,
                          {param(400)},
                          {},
                          {bare_variant(203),
                           VariantDecl{SymbolId{204}, {field(310, tp400)},
                                       Source{}}},
                          Source{}};
  NominalDecl pair_decl{SymbolId{103},
                        TypeKind::NominalEnum,
                        {param(401), param(402)},
                        {},
                        {VariantDecl{SymbolId{205},
                                     {field(311, tp401), field(312, tp402)},
                                     Source{}},
                         bare_variant(206)},
                        Source{}};
  NominalDecl list_decl{SymbolId{104},
                        TypeKind::NominalEnum,
                        {param(403)},
                        {},
                        {bare_variant(207),
                         VariantDecl{SymbolId{208}, {field(313, list_self)},
                                     Source{}}},
                        Source{}};
  NominalDecl holder_decl{SymbolId{105},
                          TypeKind::NominalEnum,
                          {param(404)},
                          {},
                          {VariantDecl{SymbolId{209}, {field(314, vec_holder)},
                                       Source{}}},
                          Source{}};
  NominalDecl wrap_decl{SymbolId{107},
                        TypeKind::NominalEnum,
                        {param(405)},
                        {},
                        {VariantDecl{SymbolId{210}, {field(315, vec_wrap)},
                                     Source{}}},
                        Source{}};
  NominalDecl struct_decl{SymbolId{106},
                          TypeKind::NominalStruct,
                          {},
                          {field(316, i32)},
                          {},
                          Source{}};

  HirModule module;
  module.config = Config{Backend::native, 64};
  module.nominals = {empty_decl, e1_decl, option_decl, pair_decl, list_decl,
                     holder_decl, wrap_decl, struct_decl};

  // 1. Domain classification.
  test_domain("d_unit", module, unit, types);
  test_domain("d_bool", module, boolean, types);
  test_domain("d_i8", module, i8, types);
  test_domain("d_u8", module, u8, types);
  test_domain("d_i16", module, i16, types);
  test_domain("d_u16", module, u16, types);
  test_domain("d_i32", module, i32, types);
  test_domain("d_u32", module, u32, types);
  test_domain("d_i64", module, i64, types);
  test_domain("d_u64", module, u64, types);
  test_domain("d_isize", module, isize, types);
  test_domain("d_usize", module, usize_type, types);
  test_domain("d_f32", module, f32, types);
  test_domain("d_f64", module, f64, types);
  test_domain("d_string", module, string_type, types);
  test_domain("d_struct", module, struct_type, types);
  test_domain("d_empty_enum", module, empty_enum_type, types);
  test_domain("d_list_i32", module, list_i32, types);
  test_domain("d_option_i32", module, option_i32, types);
  test_domain("d_pair_i32_f64", module, pair_i32_f64, types);
  test_domain("d_holder_i32", module, holder_i32, types);
  test_domain("d_wrap_u8", module, wrap_u8, types);

  // 2. Constructor views, including substituted generic fields.
  test_constructors("c_i32", module, i32, types);
  test_constructors("c_bool", module, boolean, types);
  test_constructors("c_struct", module, struct_type, types);
  test_constructors("c_empty_enum", module, empty_enum_type, types);
  test_constructors("c_e1", module, e1_type, types);
  test_constructors("c_option_i32", module, option_i32, types);
  test_constructors("c_option_string", module, option_string, types);
  test_constructors("c_pair_i32_f64", module, pair_i32_f64, types);
  test_constructors("c_holder_i32", module, holder_i32, types);
  test_constructors("c_option_option_i32", module, option_option_i32, types);
  test_constructors("c_list_i32", module, list_i32, types);
  test_constructors("c_wrap_u8", module, wrap_u8, types);
  test_constructors("c_missing_type", module, TypeId{9999}, types);
  test_constructors("c_zero_type", module, TypeId{0}, types);
  test_constructors("c_e1_args", module, e1_with_args, types);
  test_constructors("c_orphan", module, orphan_enum, types);

  // Duplicate declaration symbol: the last matching nominal wins.
  HirModule duplicate_module = module;
  duplicate_module.nominals = {NominalDecl{SymbolId{101},
                                           TypeKind::NominalEnum,
                                           {},
                                           {},
                                           {bare_variant(200)},
                                           Source{}},
                               e1_decl};
  test_constructors("c_duplicate_last", duplicate_module, e1_type, types);
  duplicate_module.nominals[0].variants = e1_decl.variants;
  duplicate_module.nominals[1].variants = {bare_variant(200)};
  test_constructors("c_duplicate_first", duplicate_module, e1_type, types);

  // 3. Target widths come from the module config.
  HirModule module32 = module;
  module32.config = Config{Backend::native, 32};
  test_domain("d32_isize", module32, isize, types);
  test_domain("d32_usize", module32, usize_type, types);
  test_domain("d32_i32", module32, i32, types);
  test_domain("d32_f64", module32, f64, types);
  test_constructors("c32_e1", module32, e1_type, types);
  HirModule module48 = module;
  module48.config = Config{Backend::native, 48};
  test_domain("d48_i32", module48, i32, types);
  test_domain("d48_isize", module48, isize, types);
  test_domain("d48_bool", module48, boolean, types);
  test_domain("d48_f32", module48, f32, types);
  test_constructors("c48_e1", module48, e1_type, types);

  // 4. Missing domain types.
  test_domain("d_missing_type", module, TypeId{9999}, types);
  test_domain("d_zero_type", module, TypeId{0}, types);

  // 5. Coverage.
  {
    CoverageFixture fixture(e1_type, module, types, 100000);
    fixture.add("e1_add_A", variant(e1_type, 200, {}));
    fixture.add("e1_add_A_again", variant(e1_type, 200, {}));
    fixture.exhaustive("e1_after_A");
    fixture.add("e1_add_B", variant(e1_type, 201, {wild(i32), wild(boolean)}));
    fixture.exhaustive("e1_after_AB");
    fixture.add("e1_add_C_true",
                variant(e1_type, 202, {bool_literal(boolean, true),
                                       wild(boolean)}));
    fixture.exhaustive("e1_after_C_true");
    fixture.add("e1_add_C_false",
                variant(e1_type, 202, {bool_literal(boolean, false),
                                       wild(boolean)}));
    fixture.exhaustive("e1_complete");
    fixture.add("e1_add_C_false_again",
                variant(e1_type, 202, {bool_literal(boolean, false),
                                       wild(boolean)}));
    fixture.add("e1_add_wild", wild(e1_type));
    fixture.add("e1_add_B_again",
                variant(e1_type, 201, {wild(i32), wild(boolean)}));
    fixture.exhaustive("e1_still_complete");
  }
  {
    CoverageFixture fixture(e1_type, module, types, 100000);
    fixture.add("e1p_add_B_true",
                variant(e1_type, 201,
                        {wild(i32), bool_literal(boolean, true)}));
    fixture.exhaustive("e1p_after_B_true");
    fixture.add("e1p_add_B_false",
                variant(e1_type, 201,
                        {wild(i32), bool_literal(boolean, false)}));
    fixture.exhaustive("e1p_B_complete");
    fixture.add("e1_never_added_A",
                variant(e1_type, 200, {}));
  }
  {
    CoverageFixture fixture(boolean, module, types, 100000);
    fixture.add("bool_add_true", bool_literal(boolean, true));
    fixture.add("bool_add_false", bool_literal(boolean, false));
    fixture.exhaustive("bool_complete");
    fixture.add("bool_add_true_again", bool_literal(boolean, true));
    fixture.add("bool_add_wild", wild(boolean));
    fixture.exhaustive("bool_still_complete");
  }
  {
    CoverageFixture fixture(boolean, module, types, 100000);
    fixture.add("bool_bind", binding(boolean, 700));
    fixture.exhaustive("bool_bind_complete");
    fixture.add("bool_bind_then_true", bool_literal(boolean, true));
  }
  {
    CoverageFixture fixture(u8, module, types, 1000000);
    for (uint64_t value = 0; value <= 255; ++value)
      fixture.add("u8_add_" + number(value), int_literal(u8, value));
    fixture.exhaustive("u8_complete");
    fixture.add("u8_add_0_again", int_literal(u8, 0));
    fixture.add("u8_add_255_again", int_literal(u8, 255));
    fixture.add("u8_add_wild", wild(u8));
  }
  {
    CoverageFixture fixture(u8, module, types, 1000000);
    for (uint64_t value = 0; value <= 254; ++value)
      fixture.add("u8p_add_" + number(value), int_literal(u8, value));
    fixture.exhaustive("u8_partial");
    fixture.add("u8p_add_255", int_literal(u8, 255));
    fixture.exhaustive("u8_partial_complete");
  }
  {
    CoverageFixture fixture(i32, module, types, 100000);
    fixture.add("i32_add_0", int_literal(i32, 0));
    fixture.add("i32_add_1", int_literal(i32, 1));
    fixture.add("i32_add_1_again", int_literal(i32, 1));
    fixture.exhaustive("i32_partial");
    fixture.add("i32_add_wild", wild(i32));
    fixture.exhaustive("i32_complete");
    fixture.add("i32_add_2_after_wild", int_literal(i32, 2));
  }
  {
    CoverageFixture fixture(f32, module, types, 100000);
    fixture.add("f32_add_pos0", float_literal(f32, 0x00000000ULL));
    fixture.add("f32_add_neg0", float_literal(f32, 0x80000000ULL));
    fixture.add("f32_add_one", float_literal(f32, 0x3f800000ULL));
    fixture.add("f32_add_pos0_again", float_literal(f32, 0x00000000ULL));
    fixture.exhaustive("f32_partial");
    fixture.add("f32_add_nan", float_literal(f32, 0x7fc00000ULL));
    fixture.add("f32_add_inf", float_literal(f32, 0x7f800000ULL));
    fixture.exhaustive("f32_still_partial");
    fixture.add("f32_add_wild", wild(f32));
    fixture.exhaustive("f32_complete");
  }
  {
    CoverageFixture fixture(f64, module, types, 100000);
    fixture.add("f64_add_pos0", float_literal(f64, 0));
    fixture.add("f64_add_neg0", float_literal(f64, 0x8000000000000000ULL));
    fixture.add("f64_add_one", float_literal(f64, 0x3ff0000000000000ULL));
    fixture.exhaustive("f64_partial");
    fixture.add("f64_add_wild", wild(f64));
    fixture.exhaustive("f64_complete");
  }
  {
    CoverageFixture fixture(string_type, module, types, 100000);
    fixture.add("str_add_empty", string_literal(string_type, ""));
    fixture.add("str_add_empty_again", string_literal(string_type, ""));
    fixture.add("str_add_a", string_literal(string_type, "a"));
    fixture.add("str_add_a_again", string_literal(string_type, "a"));
    fixture.add("str_add_cjk", string_literal(string_type, "\xe4\xb8\xad\xe6\x96\x87"));
    fixture.add("str_add_ab", string_literal(string_type, "ab"));
    fixture.exhaustive("str_partial");
    fixture.add("str_add_wild", wild(string_type));
    fixture.exhaustive("str_complete");
  }
  {
    CoverageFixture fixture(struct_type, module, types, 100000);
    fixture.exhaustive("opaque_partial");
    fixture.add("opaque_add_wild", wild(struct_type));
    fixture.exhaustive("opaque_complete");
    fixture.add("opaque_add_wild_again", wild(struct_type));
  }
  {
    CoverageFixture fixture(empty_enum_type, module, types, 100000);
    fixture.exhaustive("empty_complete");
    fixture.add("empty_add_wild", wild(empty_enum_type));
    fixture.exhaustive("empty_still_complete");
    fixture.add("empty_add_A", variant(empty_enum_type, 200, {}));
  }
  {
    CoverageFixture fixture(list_i32, module, types, 100000);
    fixture.add("list_add_cons_cons_nil",
                variant(list_i32, 208,
                        {variant(list_i32, 208,
                                 {variant(list_i32, 207, {})})}));
    fixture.add("list_add_nil", variant(list_i32, 207, {}));
    fixture.exhaustive("list_after_two");
    fixture.add("list_add_cons_nil",
                variant(list_i32, 208, {variant(list_i32, 207, {})}));
    fixture.exhaustive("list_after_three");
    fixture.add("list_add_wild", wild(list_i32));
    fixture.exhaustive("list_complete");
    fixture.add("list_add_cons_wild",
                variant(list_i32, 208, {wild(list_i32)}));
  }
  {
    CoverageFixture fixture(list_i32, module, types, 10);
    fixture.add("lazy_add_wild", wild(list_i32));
    fixture.exhaustive("lazy_complete");
  }
  {
    CoverageFixture fixture(list_i32, module, types, 9);
    fixture.add("lazy9_add_wild", wild(list_i32));
    fixture.exhaustive("lazy9_exhaust");
  }
  {
    CoverageFixture fixture(boolean, module, types, 0);
    fixture.add("budget0_add_wild", wild(boolean));
  }
  {
    CoverageFixture fixture(boolean, module, types, 1);
    fixture.add("budget1_add_wild", wild(boolean));
  }
  {
    CoverageFixture fixture(boolean, module, types, 2);
    fixture.add("budget2_add_wild", wild(boolean));
  }
  {
    CoverageFixture fixture(boolean, module, types, 3);
    fixture.add("budget3_add_wild", wild(boolean));
  }
  {
    CoverageFixture fixture(boolean, module, types, 4);
    fixture.add("budget4_add_wild", wild(boolean));
  }
  {
    CoverageFixture fixture(boolean, module, types, 5);
    fixture.add("budget5_add_wild", wild(boolean));
  }
  {
    CoverageFixture fixture(boolean, module, types, 7);
    fixture.add("budget7_add_wild", wild(boolean));
    fixture.exhaustive("budget7_exhaust");
  }
  {
    CoverageFixture fixture(boolean, module, types, 8);
    fixture.add("budget8_add_wild", wild(boolean));
    fixture.exhaustive("budget8_exhaust");
  }
  {
    CoverageFixture fixture(u8, module, types, 2);
    fixture.add("budget_u8_2_add", int_literal(u8, 0));
  }
  {
    CoverageFixture fixture(e1_type, module, types, 100000);
    fixture.add("err_variant_outside", variant(i32, 200, {}));
  }
  {
    Pattern both = bool_literal(boolean, true);
    both.variant = SymbolId{200};
    CoverageFixture fixture(boolean, module, types, 100000);
    fixture.add("err_both", both);
  }
  {
    CoverageFixture fixture(boolean, module, types, 100000);
    Pattern value = variant(boolean, 200, {});
    value.binding = SymbolId{700};
    fixture.add("err_variant_binding", value);
  }
  {
    CoverageFixture fixture(boolean, module, types, 100000);
    Pattern value = bool_literal(boolean, true);
    value.binding = SymbolId{700};
    fixture.add("err_literal_binding", value);
  }
  {
    CoverageFixture fixture(boolean, module, types, 100000);
    Pattern value = bool_literal(boolean, true);
    value.payload = {wild(boolean)};
    fixture.add("err_literal_payload", value);
  }
  {
    CoverageFixture fixture(e1_type, module, types, 100000);
    fixture.add("err_payload_count",
                variant(e1_type, 201, {wild(i32)}));
  }
  {
    CoverageFixture fixture(e1_type, module, types, 100000);
    fixture.add("column_seed",
                variant(e1_type, 201, {int_literal(u8, 0), wild(boolean)}));
    fixture.add("err_column",
                variant(e1_type, 201, {int_literal(i32, 5), wild(boolean)}));
  }
  {
    CoverageFixture fixture(i32, module, types, 100000);
    fixture.add("err_bool_on_int", bool_literal(i32, true));
  }
  {
    CoverageFixture fixture(string_type, module, types, 100000);
    fixture.add("err_int_on_string", int_literal(string_type, 0));
  }
  {
    CoverageFixture fixture(string_type, module, types, 100000);
    fixture.add("err_float_on_string", float_literal(string_type, 0));
  }
  {
    CoverageFixture fixture(f32, module, types, 100000);
    fixture.add("err_string_on_float", string_literal(f32, "x"));
  }
  {
    CoverageFixture fixture(struct_type, module, types, 100000);
    fixture.add("err_opaque_literal", bool_literal(struct_type, true));
  }
  {
    CoverageFixture fixture(e1_type, module, types, 100000);
    fixture.add("err_variant_absent", variant(e1_type, 999, {}));
  }
  {
    CoverageFixture fixture(empty_enum_type, module, types, 100000);
    fixture.add("err_empty_variant_absent", variant(empty_enum_type, 200, {}));
  }
  {
    CoverageFixture fixture(list_i32, module, types, 100000);
    Pattern nested = variant(list_i32, 207, {});
    for (size_t depth = 0; depth < 260; ++depth)
      nested = variant(list_i32, 208, {nested});
    fixture.add("err_depth", nested);
  }
  {
    CoverageFixture fixture(list_i32, module, types, 100000);
    Pattern nested = variant(list_i32, 207, {});
    for (size_t depth = 0; depth < 200; ++depth)
      nested = variant(list_i32, 208, {nested});
    fixture.add("depth_200_add", nested);
    fixture.exhaustive("depth_200_exhaust");
  }

  std::cout << "woozy-pattern/1\n";
  std::cout << "types " << types.size() << "\n";
  for (size_t index = 0; index < types.size(); ++index) {
    const TypeId id{index + 1};
    dump_type(*types.find(id), id, index);
  }
  for (const auto &line : type_records) std::cout << line << "\n";
  for (const auto &line : records) std::cout << line << "\n";
  return 0;
}
