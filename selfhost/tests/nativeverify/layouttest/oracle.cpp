// C++ oracle for the Native aggregate layout view (src/ir/layout.hpp).
//
// It constructs the same type table and nominal declarations as the Wz test
// and prints the same trace through the real lang::ir::detail::AggregateTypes.
// This is a test-only comparison program; it never participates in the
// self-hosted compiler.
#include "ir/layout.hpp"

#include <iostream>
#include <optional>
#include <string>
#include <vector>

using namespace lang;
using namespace lang::hir;

namespace {
Type simple(TypeKind kind, unsigned bits) {
  Type value;
  value.kind = kind;
  value.bits = bits;
  return value;
}

Type with_args(TypeKind kind, std::uint64_t declaration,
               std::vector<TypeId> args) {
  Type value;
  value.kind = kind;
  value.declaration = SymbolId{declaration};
  value.type_args = std::move(args);
  return value;
}

Type function(TypeId parameter, TypeId result) {
  Type value;
  value.kind = TypeKind::Function;
  value.parameters.push_back(ParameterType{ParamMode::ordinary, parameter});
  value.result = result;
  return value;
}

void print_type(const TypeContext &context, TypeId id) {
  const Type *found = context.find(id);
  if (!found) {
    std::cout << " missing";
    return;
  }
  std::cout << " kind=" << static_cast<unsigned>(found->kind)
            << " bits=" << found->bits
            << " decl=" << found->declaration.value
            << " args=" << found->type_args.size();
  for (auto arg : found->type_args) {
    const Type *inner = context.find(arg);
    if (!inner) {
      std::cout << " [missing]";
      continue;
    }
    std::cout << " [" << static_cast<unsigned>(inner->kind) << " "
              << inner->bits << " " << inner->declaration.value << " "
              << inner->type_args.size() << "]";
  }
}

void print_error(const char *prefix, const std::string &message) {
  std::cout << prefix << message << "\n";
}

void run() {
  TypeContext context;
  TypeId unit = context.intern(simple(TypeKind::Unit, 0));
  TypeId i32 = context.intern(simple(TypeKind::SignedInt, 32));
  TypeId parameter =
      context.intern(with_args(TypeKind::TypeParameter, 100, {}));
  TypeId function_type = context.intern(function(i32, unit));
  TypeId vec =
      context.intern(with_args(TypeKind::Vec, 0, {i32}));
  TypeId struct_generic =
      context.intern(with_args(TypeKind::NominalStruct, 7, {parameter}));
  TypeId struct_i32 =
      context.intern(with_args(TypeKind::NominalStruct, 7, {i32}));
  TypeId enum_generic =
      context.intern(with_args(TypeKind::NominalEnum, 8, {parameter}));
  TypeId enum_i32 =
      context.intern(with_args(TypeKind::NominalEnum, 8, {i32}));
  TypeId missing = context.intern(with_args(TypeKind::NominalStruct, 55, {}));
  TypeId plain_struct = context.intern(with_args(TypeKind::NominalStruct, 9, {}));

  std::cout << "context size=" << context.size() << "\n";
  std::cout << "ids i32=" << i32.value << " unit=" << unit.value
            << " param=" << parameter.value << " func=" << function_type.value
            << " vec=" << vec.value << " sg=" << struct_generic.value
            << " si=" << struct_i32.value << " eg=" << enum_generic.value
            << " ei=" << enum_i32.value << " missing=" << missing.value
            << " plain=" << plain_struct.value << "\n";

  std::vector<FieldDecl> struct_fields{
      FieldDecl{SymbolId{10}, parameter},
      FieldDecl{SymbolId{11}, i32},
      FieldDecl{SymbolId{12}, vec}};
  std::vector<FieldDecl> variant_fields{
      FieldDecl{SymbolId{21}, parameter},
      FieldDecl{SymbolId{22}, i32}};
  std::vector<FieldDecl> plain_fields{FieldDecl{SymbolId{30}, function_type}};

  HirModule module;
  module.types = context;
  module.config = Config{Backend::native, 64};
  module.nominals.push_back(NominalDecl{
      SymbolId{7}, TypeKind::NominalStruct,
      {GenericParam{SymbolId{100}}}, struct_fields, {}, {}});
  module.nominals.push_back(NominalDecl{
      SymbolId{8}, TypeKind::NominalEnum, {GenericParam{SymbolId{100}}}, {},
      {VariantDecl{SymbolId{20}, variant_fields, {}},
       VariantDecl{SymbolId{23}, {}, {}}},
      {}});
  module.nominals.push_back(NominalDecl{
      SymbolId{9}, TypeKind::NominalStruct, {}, plain_fields, {}, {}});

  ir::detail::AggregateTypes aggregates(module);
  auto declaration = [&](TypeId id, const char *label) {
    try {
      const NominalDecl &value = aggregates.declaration(id);
      std::cout << label << " symbol=" << value.symbol.value
                << " kind=" << static_cast<unsigned>(value.kind)
                << " params=" << value.params.size()
                << " fields=" << value.fields.size()
                << " variants=" << value.variants.size() << "\n";
    } catch (const std::exception &error) {
      print_error("declaration err=", error.what());
    }
  };
  declaration(struct_i32, "decl S");
  declaration(enum_i32, "decl E");
  declaration(plain_struct, "decl P");
  declaration(missing, "decl missing");

  auto fields = [&](TypeId id, std::optional<SymbolId> variant,
                    const char *label) {
    try {
      auto values = aggregates.fields(id, variant);
      std::cout << label << " count=" << values.size() << "\n";
      for (const auto &field : values) {
        std::cout << " f " << field.symbol.value << " type=" << field.type.value;
        print_type(aggregates.types, field.type);
        std::cout << "\n";
      }
    } catch (const std::exception &error) {
      print_error("fields err=", error.what());
    }
  };
  fields(struct_i32, std::nullopt, "fields S");
  fields(enum_i32, SymbolId{20}, "fields Ev20");
  fields(enum_i32, SymbolId{23}, "fields Ev23");
  fields(enum_i32, std::nullopt, "fields Enone");
  fields(plain_struct, std::nullopt, "fields P");
  fields(struct_i32, SymbolId{99}, "fields badvariant");

  auto field_index = [&](TypeId id, SymbolId field,
                         std::optional<SymbolId> variant, const char *label) {
    std::cout << label;
    try {
      const auto index = aggregates.field_index(id, field, variant);
      std::cout << "=" << index << "\n";
    } catch (const std::exception &error) {
      print_error(" err=", error.what());
    }
  };
  field_index(struct_i32, SymbolId{11}, std::nullopt, "field_index S");
  field_index(enum_i32, SymbolId{22}, SymbolId{20}, "field_index E");
  field_index(struct_i32, SymbolId{99}, std::nullopt, "field_index bad");

  auto variant_index = [&](TypeId id, SymbolId variant, const char *label) {
    std::cout << label;
    try {
      const auto index = aggregates.variant_index(id, variant);
      std::cout << "=" << index << "\n";
    } catch (const std::exception &error) {
      print_error(" err=", error.what());
    }
  };
  variant_index(enum_i32, SymbolId{20}, "variant_index A");
  variant_index(enum_i32, SymbolId{23}, "variant_index B");
  variant_index(struct_i32, SymbolId{20}, "variant_index S");
  variant_index(missing, SymbolId{20}, "variant_index missing");

  std::cout << "private context after=" << aggregates.types.size() << "\n";
  std::cout << "public types=" << module.types.size() << "\n";
}
} // namespace

int main() {
  run();
  return 0;
}
