// C++ oracle for the pure-Wz literal/constant acceptance tests.
//
// It links the existing lang_frontend/lang_modules/lang_hir libraries from the
// stage-0 build.  It does not rebuild the compiler and does not execute the
// old driver; it only calls the published literal/constant algorithms and
// prints the same canonical field order as the Wz test program.
#include "hir/constant_value.hpp"
#include "lang/hir/hir.hpp"
#include "hir/literal_value.hpp"
#include "lang/modules/project.hpp"
#include "lang/modules/resolve.hpp"
#include "lang/syntax/parser.hpp"
#include "lang/syntax/token.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace lang;
using namespace lang::hir;
using modules::NodeRef;
using modules::Project;
using modules::ResolvedProject;
using modules::Symbol;
using modules::SymbolKind;

namespace {

Config config() { return Config{Backend::native, 64}; }

std::string hex(std::string_view value) {
  static const char *digits = "0123456789abcdef";
  std::string out;
  for (unsigned char byte : value) {
    out.push_back(digits[byte >> 4]);
    out.push_back(digits[byte & 15]);
  }
  return out;
}

TypeId scalar(TypeContext &types, TypeKind kind, unsigned bits = 0) {
  Type value;
  value.kind = kind;
  value.bits = bits;
  return types.intern(value);
}

TypeId nominal(TypeContext &types, TypeKind kind, SymbolId declaration) {
  Type value;
  value.kind = kind;
  value.declaration = declaration;
  return types.intern(value);
}

TypeId parameterized(TypeContext &types, TypeId parameter, TypeId result) {
  Type value;
  value.kind = TypeKind::Function;
  value.parameters.push_back({ParamMode::ordinary, parameter});
  value.result = result;
  return types.intern(value);
}

std::string type_text(const TypeContext &types, TypeId id) {
  const Type *value = types.find(id);
  if (!value)
    return "missing";
  return std::to_string(static_cast<int>(value->kind)) + ":" +
         std::to_string(value->bits) + ":" +
         std::to_string(value->declaration.value);
}

struct Parsed {
  PhaseResult<AstFile> result;
};

Parsed parse_source(const std::string &text) {
  auto snapshot = std::make_shared<SourceSnapshot>(
      FileId(1), RevisionId(1), SourceSnapshotId(1), "test.wz", text);
  auto lexed = lex(snapshot);
  if (!lexed.value) {
    PhaseResult<AstFile> failed;
    failed.state = lexed.state;
    failed.failure = lexed.failure;
    return Parsed{std::move(failed)};
  }
  return Parsed{parse(*lexed.value)};
}

std::shared_ptr<Project> make_project(const AstFile &file) {
  auto project = std::make_shared<Project>();
  modules::Module module;
  module.id = ModuleId{0};
  module.logical_path = "app";
  module.identity = "app";
  module.package_name = "app";
  module.files.push_back(std::make_shared<const AstFile>(file));
  project->modules.push_back(std::move(module));
  return project;
}

size_t find_kind(const AstFile &file, AstKind kind) {
  size_t found = 0;
  for (size_t index = 0; index < file.nodes.size(); ++index)
    if (file.nodes[index].kind == kind)
      found = index;
  return found;
}

std::string kind_name(AstKind kind) {
  return std::to_string(static_cast<int>(kind));
}

void print_pattern(const std::string &label, const PatternLiteral &value) {
  if (auto p = std::get_if<BoolLiteral>(&value)) {
    std::cout << label << "=bool:" << (p->value ? "true" : "false") << "\n";
  } else if (auto p = std::get_if<IntegerLiteral>(&value)) {
    std::cout << label << "=int:" << p->decimal << ":" << p->bits << "\n";
  } else if (auto p = std::get_if<FloatLiteral>(&value)) {
    std::cout << label << "=float:" << p->bits << "\n";
  } else if (auto p = std::get_if<StringLiteral>(&value)) {
    std::cout << label << "=string:" << hex(p->utf8) << "\n";
  }
}

void print_error(const std::string &label, const std::string &message) {
  std::cout << label << "=error:" << message << "\n";
}

void print_constant(const std::string &label, const TypeContext &types,
                    const ConstantValue &value) {
  const std::string prefix =
      label + "=type=" + type_text(types, value.type) + "|";
  std::visit(
      [&](const auto &payload) {
        using T = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<T, UnitLiteral>) {
          std::cout << prefix << "unit\n";
        } else if constexpr (std::is_same_v<T, BoolLiteral>) {
          std::cout << prefix << "bool:"
                    << (payload.value ? "true" : "false") << "\n";
        } else if constexpr (std::is_same_v<T, IntegerLiteral>) {
          std::cout << prefix << "int:" << payload.decimal << ":"
                    << payload.bits << "\n";
        } else if constexpr (std::is_same_v<T, FloatLiteral>) {
          std::cout << prefix << "float:" << payload.bits << "\n";
        } else if constexpr (std::is_same_v<T, StringLiteral>) {
          std::cout << prefix << "string:" << hex(payload.utf8) << "\n";
        } else if constexpr (std::is_same_v<T, FunctionValue>) {
          std::string args;
          for (TypeId arg : payload.type_args)
            args += "," + type_text(types, arg);
          std::string bound = "none";
          if (payload.bound)
            bound = type_text(types, payload.bound->type);
          std::cout << prefix << "function:" << payload.symbol.value << args
                    << ":caps:" << payload.captures.size() << ":move:"
                    << (payload.move_capture ? "true" : "false")
                    << ":bound:" << bound << "\n";
        } else {
          std::string variant = "none";
          if (payload.variant)
            variant = std::to_string(payload.variant->value);
          std::cout << prefix << "aggregate:" << variant << ":"
                    << payload.fields.size() << "\n";
          for (size_t index = 0; index < payload.fields.size(); ++index)
            print_constant(label + ".field" + std::to_string(index), types,
                           payload.fields[index]);
        }
      },
      value.data);
}

void decode_case(const std::string &label, const std::string &snippet,
                 TypeId expected, TypeContext &types, const Config &cfg) {
  Parsed parsed = parse_source(snippet);
  if (!parsed.result.value || !parsed.result.success()) {
    std::cout << label << "=parse_failed\n";
    return;
  }
  auto project = make_project(*parsed.result.value);
  const size_t node = find_kind(*parsed.result.value, AstKind::LiteralPattern);
  try {
    auto value = decode_pattern_literal(
        *project, types, NodeRef{ModuleId{0}, 0, AstNodeId{node}}, expected,
        cfg);
    print_pattern(label, value);
  } catch (const std::exception &error) {
    print_error(label, error.what());
  }
}

struct Bindings {
  std::shared_ptr<Project> project;
  std::shared_ptr<ResolvedProject> resolved;
  std::vector<TypeId> types;
};

Bindings make_bindings(
    const AstFile &file, const std::vector<std::string> &names,
    const std::vector<SymbolKind> &kinds, const std::vector<TypeId> &ids,
    const std::vector<std::optional<size_t>> &definitions) {
  Bindings out;
  out.project = make_project(file);
  auto resolved = std::make_shared<ResolvedProject>();
  resolved->project = out.project;
  for (size_t index = 0; index < names.size(); ++index) {
    Symbol symbol;
    symbol.id = SymbolId{index};
    symbol.name = names[index];
    symbol.kind = kinds[index];
    symbol.module = ModuleId{0};
    if (index < definitions.size() && definitions[index])
      symbol.definition =
          NodeRef{ModuleId{0}, 0, AstNodeId{*definitions[index]}};
    resolved->symbols.push_back(std::move(symbol));
  }
  for (size_t index = 0; index < file.nodes.size(); ++index) {
    const auto &node = file.nodes[index];
    if (node.kind != AstKind::Name)
      continue;
    const std::string spelling(file.tokens[node.token_begin].spelling);
    for (size_t symbol = 0; symbol < names.size(); ++symbol) {
      if (names[symbol] == spelling)
        resolved->occurrences.push_back(
            {NodeRef{ModuleId{0}, 0, AstNodeId{index}}, SymbolId{symbol},
             node.span});
    }
  }
  out.resolved = std::move(resolved);
  out.types = ids;
  return out;
}

std::string expression(const std::string &value) {
  return "package app;\nfunc f() i32 { return " + value + "; }\n";
}

void source_case(const std::string &label, const std::string &snippet,
                 AstKind kind, TypeId expected, TypeContext &types,
                 const std::vector<std::string> &names,
                 const std::vector<SymbolKind> &kinds,
                 const std::vector<TypeId> &ids,
                 const std::vector<std::optional<size_t>> &definitions,
                 const std::vector<NominalDecl> &nominals,
                 const std::function<FunctionValue(NodeRef, TypeId)> &function) {
  Parsed parsed = parse_source(snippet);
  if (!parsed.result.value || !parsed.result.success()) {
    std::cout << label << "=parse_failed\n";
    return;
  }
  Bindings bindings = make_bindings(*parsed.result.value, names, kinds, ids,
                                    definitions);
  HirModule module;
  module.resolved = bindings.resolved;
  module.identity = ModuleId{0};
  module.config = config();
  module.nominals = nominals;
  const size_t node = find_kind(*parsed.result.value, kind);
  auto resolve = [&](NodeRef candidate) -> TypeId {
    for (const auto &occurrence : bindings.resolved->occurrences)
      if (occurrence.source.module == candidate.module &&
          occurrence.source.file == candidate.file &&
          occurrence.source.node == candidate.node)
        return bindings.types.at(occurrence.symbol.value);
    throw LiteralError("test type resolution failed");
  };
  try {
    auto value = source_constant_value(
        module, types, NodeRef{ModuleId{0}, 0, AstNodeId{node}}, expected,
        resolve, function);
    print_constant(label, types, value);
  } catch (const std::exception &error) {
    print_error(label, error.what());
  }
}

Source empty_source(size_t node = 0) {
  Source source;
  source.node = NodeRef{ModuleId{0}, 0, AstNodeId{node}};
  source.span = SourceSpan{SourceSnapshotId{0}, 0, 0};
  return source;
}

HirNode make_node(TypeId type, NodeData data,
                  std::vector<HirNodeId> children = {}) {
  HirNode node;
  node.type = type;
  node.source = empty_source();
  node.scope = ScopeId{0};
  node.children = std::move(children);
  node.data = std::move(data);
  return node;
}

HirNode literal_node(TypeId type, std::string decimal, uint64_t bits) {
  return make_node(type, IntegerLiteral{std::move(decimal), bits});
}

HirNode bool_node(TypeId type, bool value) {
  return make_node(type, BoolLiteral{value});
}

HirNode float_node(TypeId type, uint64_t bits) {
  return make_node(type, FloatLiteral{bits});
}

CallArg call_arg(uint32_t parameter, size_t expression) {
  CallArg argument;
  argument.source_index = parameter;
  argument.parameter_index = parameter;
  argument.expression = HirNodeId{expression};
  return argument;
}

Symbol symbol_with(size_t id, std::string name, SymbolKind kind) {
  Symbol symbol;
  symbol.id = SymbolId{id};
  symbol.name = std::move(name);
  symbol.kind = kind;
  symbol.module = ModuleId{0};
  return symbol;
}

HirModule hir_module(TypeContext types, std::vector<HirNode> nodes,
                     std::vector<Symbol> symbols,
                     std::vector<NominalDecl> nominals = {}) {
  auto project = std::make_shared<Project>();
  auto resolved = std::make_shared<ResolvedProject>();
  resolved->project = project;
  resolved->symbols = std::move(symbols);
  HirModule module;
  module.resolved = std::move(resolved);
  module.identity = ModuleId{0};
  module.types = std::move(types);
  module.config = config();
  module.nodes = std::move(nodes);
  module.nominals = std::move(nominals);
  return module;
}

void hir_case(const std::string &label, const TypeContext &display,
              HirModule module, size_t node) {
  auto value = evaluate_hir_constant(module, HirNodeId{node});
  if (!value) {
    std::cout << label << "=none\n";
    return;
  }
  print_constant(label, display, *value);
}

void aggregate_case(const std::string &label, const TypeContext &display,
                    HirModule module, size_t node) {
  auto value = evaluate_hir_constant(module, HirNodeId{node});
  if (!value) {
    std::cout << label << "=none\n";
    return;
  }
  if (auto aggregate = std::get_if<ConstantAggregate>(&value->data))
    std::cout << label << "=aggregate:" << aggregate->fields.size() << "\n";
  else
    std::cout << label << "=some\n";
}

FieldDecl field_decl(SymbolId symbol, TypeId type) {
  return FieldDecl{symbol, type, true, empty_source()};
}

void literal_tests(TypeContext &types) {
  const TypeId u8 = scalar(types, TypeKind::UnsignedInt, 8);
  const TypeId i8 = scalar(types, TypeKind::SignedInt, 8);
  const TypeId i32 = scalar(types, TypeKind::SignedInt, 32);
  const TypeId u32 = scalar(types, TypeKind::UnsignedInt, 32);
  const TypeId i64 = scalar(types, TypeKind::SignedInt, 64);
  const TypeId u64 = scalar(types, TypeKind::UnsignedInt, 64);
  const TypeId isize = scalar(types, TypeKind::Isize);
  const TypeId usize = scalar(types, TypeKind::Usize);
  const TypeId f32 = scalar(types, TypeKind::Float, 32);
  const TypeId f64 = scalar(types, TypeKind::Float, 64);
  const TypeId bool_type = scalar(types, TypeKind::Bool);
  const TypeId string_type = scalar(types, TypeKind::String);
  const Config cfg = config();
  decode_case("bool.true", R"(package app;
func f(x bool) i32 { switch x { case true: return 0; default: return 1; } }
)", bool_type, types, cfg);
  decode_case("bool.false", R"(package app;
func f(x bool) i32 { switch x { case false: return 0; default: return 1; } }
)", bool_type, types, cfg);
  decode_case("bool.integer", R"(package app;
func f(x bool) i32 { switch x { case 1: return 0; default: return 1; } }
)", bool_type, types, cfg);
  decode_case("bool.negative", R"(package app;
func f(x bool) i32 { switch x { case -0: return 0; default: return 1; } }
)", bool_type, types, cfg);
  const std::string int_source = R"(package app;
func f(x i32) i32 { switch x { case 42: return 0; default: return 1; } }
)";
  const std::string neg_source = R"(package app;
func f(x i32) i32 { switch x { case -42: return 0; default: return 1; } }
)";
  decode_case("i32.42", int_source, i32, types, cfg);
  decode_case("i32.minus42", neg_source, i32, types, cfg);
  decode_case("i8.min", R"(package app;
func f(x i8) i32 { switch x { case -128: return 0; default: return 1; } }
)", i8, types, cfg);
  decode_case("i8.minus129", R"(package app;
func f(x i8) i32 { switch x { case -129: return 0; default: return 1; } }
)", i8, types, cfg);
  decode_case("i8.hex", R"(package app;
func f(x i8) i32 { switch x { case 0x7f: return 0; default: return 1; } }
)", i8, types, cfg);
  decode_case("u8.max", R"(package app;
func f(x u8) i32 { switch x { case 255: return 0; default: return 1; } }
)", u8, types, cfg);
  decode_case("u8.overflow", R"(package app;
func f(x u8) i32 { switch x { case 256: return 0; default: return 1; } }
)", u8, types, cfg);
  decode_case("u64.max", R"(package app;
func f(x u64) i32 { switch x { case 0xffff_ffff_ffff_ffff: return 0; default: return 1; } }
)", u64, types, cfg);
  decode_case("u64.max.dec", R"(package app;
func f(x u64) i32 { switch x { case 18446744073709551615: return 0; default: return 1; } }
)", u64, types, cfg);
  decode_case("u64.overflow", R"(package app;
func f(x u64) i32 { switch x { case 18446744073709551616: return 0; default: return 1; } }
)", u64, types, cfg);
  decode_case("isize.word64.min", R"(package app;
func f(x isize) i32 { switch x { case -9223372036854775808: return 0; default: return 1; } }
)", isize, types, cfg);
  decode_case("usize.word64.max", R"(package app;
func f(x usize) i32 { switch x { case 18446744073709551615: return 0; default: return 1; } }
)", usize, types, cfg);
  Config word32 = cfg;
  word32.word_bits = 32;
  decode_case("usize.word32.max", R"(package app;
func f(x usize) i32 { switch x { case 4294967295: return 0; default: return 1; } }
)", usize, types, word32);
  decode_case("usize.word32.overflow", R"(package app;
func f(x usize) i32 { switch x { case 4294967296: return 0; default: return 1; } }
)", usize, types, word32);
  decode_case("f32.1.5", R"(package app;
func f(x f32) i32 { switch x { case 1.5: return 0; default: return 1; } }
)", f32, types, cfg);
  decode_case("f32.negative.zero", R"(package app;
func f(x f32) i32 { switch x { case -0.0: return 0; default: return 1; } }
)", f32, types, cfg);
  decode_case("f32.overflow", R"(package app;
func f(x f32) i32 { switch x { case 1e40: return 0; default: return 1; } }
)", f32, types, cfg);
  decode_case("f64.ulp", R"(package app;
func f(x f64) i32 { switch x { case 1.0000000000000002: return 0; default: return 1; } }
)", f64, types, cfg);
  decode_case("f64.overflow", R"(package app;
func f(x f64) i32 { switch x { case 1e400: return 0; default: return 1; } }
)", f64, types, cfg);
  decode_case("f64.underflow", R"(package app;
func f(x f64) i32 { switch x { case 1e-400: return 0; default: return 1; } }
)", f64, types, cfg);
  decode_case("f32.tie", R"(package app;
func f(x f32) i32 { switch x { case 16777217.0: return 0; default: return 1; } }
)", f32, types, cfg);
  decode_case("f32.subnormal", R"(package app;
func f(x f32) i32 { switch x { case 1e-45: return 0; default: return 1; } }
)", f32, types, cfg);
  decode_case("f32.min.normal", R"(package app;
func f(x f32) i32 { switch x { case 1.1754943508222875e-38: return 0; default: return 1; } }
)", f32, types, cfg);
  decode_case("f64.min.subnormal", R"(package app;
func f(x f64) i32 { switch x { case 5e-324: return 0; default: return 1; } }
)", f64, types, cfg);
  decode_case("f64.max", R"(package app;
func f(x f64) i32 { switch x { case 1.7976931348623157e308: return 0; default: return 1; } }
)", f64, types, cfg);
  decode_case("f64.tie", R"(package app;
func f(x f64) i32 { switch x { case 1.00000000000000011102230246251565404236316680908203125: return 0; default: return 1; } }
)", f64, types, cfg);
  decode_case("u8.underscore", R"(package app;
func f(x u8) i32 { switch x { case 0b1111_1111: return 0; default: return 1; } }
)", u8, types, cfg);
  decode_case("u8.octal", R"(package app;
func f(x u8) i32 { switch x { case 0o377: return 0; default: return 1; } }
)", u8, types, cfg);
  decode_case("i32.hex.min", R"(package app;
func f(x i32) i32 { switch x { case -0x8000_0000: return 0; default: return 1; } }
)", i32, types, cfg);
  decode_case("f64.negative", R"(package app;
func f(x f64) i32 { switch x { case -1.25: return 0; default: return 1; } }
)", f64, types, cfg);
  decode_case("f64.integer.token", R"(package app;
func f(x f64) i32 { switch x { case 1: return 0; default: return 1; } }
)", f64, types, cfg);
  decode_case("i32.float.token", R"(package app;
func f(x i32) i32 { switch x { case 1.0: return 0; default: return 1; } }
)", i32, types, cfg);
  decode_case("string.value", R"(package app;
func f(x string) i32 { switch x { case "a\u{0}😀": return 0; default: return 1; } }
)", string_type, types, cfg);
  decode_case("string.negative", R"(package app;
func f(x string) i32 { switch x { case -1: return 0; default: return 1; } }
)", string_type, types, cfg);
  decode_case("unsupported.target", int_source, bool_type, types, cfg);
}


void source_tests(TypeContext &types) {
  const TypeId u8 = scalar(types, TypeKind::UnsignedInt, 8);
  const TypeId i8 = scalar(types, TypeKind::SignedInt, 8);
  const TypeId i32 = scalar(types, TypeKind::SignedInt, 32);
  const TypeId u32 = scalar(types, TypeKind::UnsignedInt, 32);
  const TypeId i64 = scalar(types, TypeKind::SignedInt, 64);
  const TypeId u64 = scalar(types, TypeKind::UnsignedInt, 64);
  const TypeId isize = scalar(types, TypeKind::Isize);
  const TypeId usize = scalar(types, TypeKind::Usize);
  const TypeId f32 = scalar(types, TypeKind::Float, 32);
  const TypeId f64 = scalar(types, TypeKind::Float, 64);
  const TypeId bool_type = scalar(types, TypeKind::Bool);
  const TypeId string_type = scalar(types, TypeKind::String);
  const std::vector<std::string> names = {
      "i8", "u8", "i32", "u32", "i64", "u64", "isize", "usize",
      "f32", "f64", "bool", "string"};
  const std::vector<SymbolKind> kinds(names.size(), SymbolKind::builtin_type);
  const std::vector<TypeId> ids = {i8, u8, i32, u32, i64, u64, isize,
                                   usize, f32, f64, bool_type, string_type};
  const std::vector<std::optional<size_t>> defs;
  const std::vector<NominalDecl> no_nominals;
  auto function = [](NodeRef, TypeId) -> FunctionValue {
    throw LiteralError("test function callback unavailable");
  };
  auto run = [&](const std::string &label, const std::string &value,
                 AstKind kind, TypeId expected) {
    source_case(label, expression(value), kind, expected, types, names, kinds,
                ids, defs, no_nominals, function);
  };
  run("src.i8.127", "127", AstKind::Literal, i8);
  run("src.i8.min", "-128", AstKind::Unary, i8);
  run("src.i8.min.double", "-(-128)", AstKind::Unary, i8);
  run("src.i8.overflow", "128", AstKind::Literal, i8);
  run("src.u8.255", "255", AstKind::Literal, u8);
  run("src.u8.overflow", "256", AstKind::Literal, u8);
  run("src.i8.not", "~0", AstKind::Unary, i8);
  run("src.u8.not", "~0", AstKind::Unary, u8);
  run("src.i8.add.wrap", "i8(127)+i8(1)", AstKind::Binary, i8);
  run("src.u8.add.wrap", "u8(255)+u8(1)", AstKind::Binary, u8);
  run("src.i8.sub.wrap", "i8(-128)-i8(1)", AstKind::Binary, i8);
  run("src.i8.mul.wrap", "i8(100)*i8(3)", AstKind::Binary, i8);
  run("src.i8.div.trunc", "i8(-7)/i8(2)", AstKind::Binary, i8);
  run("src.i8.rem.trunc", "i8(-7)%i8(2)", AstKind::Binary, i8);
  run("src.i8.div.zero", "i8(1)/i8(0)", AstKind::Binary, i8);
  run("src.i8.shr.neg", "i8(-7)>>i8(1)", AstKind::Binary, i8);
  run("src.i8.shl.wrap", "i8(1)<<i8(7)", AstKind::Binary, i8);
  run("src.u8.shr", "u8(255)>>u8(1)", AstKind::Binary, u8);
  run("src.i8.and", "i8(-1)&i8(15)", AstKind::Binary, i8);
  run("src.i8.or", "i8(-2)|i8(1)", AstKind::Binary, i8);
  run("src.i8.xor", "i8(-1)^i8(127)", AstKind::Binary, i8);
  run("src.i8.shl.panic", "i8(1)<<i8(8)", AstKind::Binary, i8);
  run("src.bool.and", "true&&false", AstKind::Binary, bool_type);
  run("src.bool.short.and", "false&&(i32(1)/i32(0)==i32(0))",
      AstKind::Binary, bool_type);
  run("src.bool.short.or", "true||(i32(1)/i32(0)==i32(0))",
      AstKind::Binary, bool_type);
  run("src.f32.add", "f32(1.5)+f32(0.5)", AstKind::Binary, f32);
  run("src.f64.div.zero", "f64(1.0)/f64(0.0)", AstKind::Binary, f64);
  run("src.f64.nan", "f64(0.0)/f64(0.0)", AstKind::Binary, f64);
  run("src.f64.negzero.add", "f64(-0.0)+f64(-0.0)", AstKind::Binary, f64);
  run("src.f64.poszero.add", "f64(-0.0)+f64(0.0)", AstKind::Binary, f64);
  run("src.f64.nan.ne", "f64(0.0)/f64(0.0)!=f64(0.0)",
      AstKind::Binary, bool_type);
  run("src.f64.nan.eq", "f64(0.0)/f64(0.0)==f64(0.0)",
      AstKind::Binary, bool_type);
  run("src.f64.zeros.eq", "f64(-0.0)==f64(0.0)", AstKind::Binary, bool_type);
  run("src.i8.from.float", "i8(1.9)", AstKind::Call, i8);
  run("src.i8.from.negfloat", "i8(-1.9)", AstKind::Call, i8);
  run("src.i32.from.inf", "i32(f32(1e40))", AstKind::Call, i32);
  run("src.f32.from.int", "f32(16777217)", AstKind::Call, f32);
  run("src.f64.from.f32", "f64(f32(0.1))", AstKind::Call, f64);
  source_case("src.string", expression(R"("a\u{0}😀")"),
              AstKind::Literal, string_type, types, names, kinds, ids, defs,
              no_nominals, function);
}

void source_special_tests(TypeContext &types) {
  const TypeId i32 = scalar(types, TypeKind::SignedInt, 32);
  const TypeId string_type = scalar(types, TypeKind::String);
  const TypeId point = nominal(types, TypeKind::NominalStruct, SymbolId{300});
  const TypeId item = nominal(types, TypeKind::NominalEnum, SymbolId{310});
  const TypeId holder = nominal(types, TypeKind::NominalStruct, SymbolId{400});
  std::vector<NominalDecl> point_nominals;
  NominalDecl point_decl;
  point_decl.symbol = SymbolId{300};
  point_decl.kind = TypeKind::NominalStruct;
  point_decl.fields.push_back(field_decl(SymbolId{2}, i32));
  point_decl.fields.push_back(field_decl(SymbolId{3}, i32));
  point_nominals.push_back(point_decl);
  const std::vector<std::string> point_names = {"Point", "i32", "x", "y"};
  const std::vector<SymbolKind> point_kinds = {
      SymbolKind::type, SymbolKind::builtin_type, SymbolKind::field,
      SymbolKind::field};
  const std::vector<TypeId> point_ids = {point, i32, i32, i32};
  const std::vector<std::optional<size_t>> no_defs;
  auto function = [](NodeRef, TypeId) -> FunctionValue {
    throw LiteralError("test function callback unavailable");
  };
  source_case("agg.positional", R"(package app;
struct Point { x i32; y i32; }
func f() Point { return Point(1, 2); }
)", AstKind::Call, point, types, point_names, point_kinds, point_ids, no_defs,
              point_nominals, function);
  source_case("agg.named.reorder", R"(package app;
struct Point { x i32; y i32; }
func f() Point { return Point(y: 2, x: 1); }
)", AstKind::Call, point, types, point_names, point_kinds, point_ids, no_defs,
              point_nominals, function);
  source_case("agg.nested.field", R"(package app;
struct Point { x i32; y i32; }
func f() Point { return Point(y: Point(x: 3, y: 4).x, x: 5); }
)", AstKind::Call, point, types, point_names, point_kinds, point_ids, no_defs,
              point_nominals, function);
  const std::string alias_source = R"(package app;
struct Point { x i32; y i32; }
const Origin = Point(x: 1, y: 2);
func f() Point { return Origin; }
)";
  Parsed alias_parsed = parse_source(alias_source);
  const size_t alias_binding = find_kind(*alias_parsed.result.value,
                                         AstKind::Binding);
  const std::vector<std::string> alias_names = {"Point", "Origin", "x", "y",
                                               "i32"};
  const std::vector<SymbolKind> alias_kinds = {
      SymbolKind::type, SymbolKind::constant, SymbolKind::field,
      SymbolKind::field, SymbolKind::builtin_type};
  const std::vector<TypeId> alias_ids = {point, point, i32, i32, i32};
  const std::vector<std::optional<size_t>> alias_defs = {
      std::nullopt, alias_binding, std::nullopt, std::nullopt, std::nullopt};
  source_case("agg.constant.alias", alias_source, AstKind::Name, point, types,
              alias_names, alias_kinds, alias_ids, alias_defs, point_nominals,
              function);
  source_case("agg.constant.field", R"(package app;
struct Point { x i32; y i32; }
const Origin = Point(x: 1, y: 2);
func f() i32 { return Origin.y; }
)", AstKind::Member, i32, types, alias_names, alias_kinds, alias_ids,
              alias_defs, point_nominals, function);
  std::vector<NominalDecl> item_nominals;
  NominalDecl item_decl;
  item_decl.symbol = SymbolId{310};
  item_decl.kind = TypeKind::NominalEnum;
  VariantDecl variant_decl;
  variant_decl.symbol = SymbolId{2};
  variant_decl.payload.push_back(field_decl(SymbolId{3}, string_type));
  item_decl.variants.push_back(std::move(variant_decl));
  item_nominals.push_back(item_decl);
  source_case("enum.payload", R"(package app;
enum Item { Empty; Full(text string); }
func f() Item { return Item.Full(text: "x"); }
)", AstKind::Call, item, types, {"Item", "string", "Full", "text"},
              {SymbolKind::type, SymbolKind::builtin_type, SymbolKind::variant,
               SymbolKind::field},
              {item, string_type, item, string_type}, no_defs, item_nominals,
              function);
  const TypeId call_type = parameterized(types, i32, i32);
  std::vector<NominalDecl> holder_nominals;
  NominalDecl holder_decl;
  holder_decl.symbol = SymbolId{400};
  holder_decl.kind = TypeKind::NominalStruct;
  holder_decl.fields.push_back(field_decl(SymbolId{3}, call_type));
  holder_nominals.push_back(holder_decl);
  auto make_function = [](NodeRef, TypeId) -> FunctionValue {
    return FunctionValue{SymbolId{77}, {}, {}, false, {}};
  };
  source_case("function.field", R"(package app;
struct Holder { call func(i32) i32; }
func twice(x i32) i32 { return x * 2; }
func f() Holder { return Holder(call: twice); }
)", AstKind::Call, holder, types,
              {"Holder", "twice", "i32", "call"},
              {SymbolKind::type, SymbolKind::function, SymbolKind::builtin_type,
               SymbolKind::field},
              {holder, call_type, i32, call_type}, no_defs, holder_nominals,
              make_function);
  const std::string cycle_source = R"(package app;
const A = A;
func f() i32 { return A; }
)";
  Parsed cycle_parsed = parse_source(cycle_source);
  const size_t cycle_binding = find_kind(*cycle_parsed.result.value,
                                         AstKind::Binding);
  source_case("source.cycle", cycle_source, AstKind::Name, i32, types,
              {"A", "i32"},
              {SymbolKind::constant, SymbolKind::builtin_type}, {i32, i32},
              {cycle_binding, std::nullopt}, {}, function);
  std::string nested = "1";
  for (size_t index = 0; index < 130; ++index)
    nested = "(" + nested + ")";
  source_case("source.depth", expression(nested), AstKind::Parenthesized, i32,
              types, {}, {}, {}, {}, {}, function);
}

void hir_tests(TypeContext &types) {
  const TypeId i8 = scalar(types, TypeKind::SignedInt, 8);
  const TypeId u8 = scalar(types, TypeKind::UnsignedInt, 8);
  const TypeId i32 = scalar(types, TypeKind::SignedInt, 32);
  const TypeId f32 = scalar(types, TypeKind::Float, 32);
  const TypeId bool_type = scalar(types, TypeKind::Bool);
  const TypeId struct_type = nominal(types, TypeKind::NominalStruct,
                                     SymbolId{100});
  const TypeId enum_type = nominal(types, TypeKind::NominalEnum,
                                   SymbolId{101});
  {
    std::vector<HirNode> nodes;
    nodes.push_back(literal_node(i32, "42", 42));
    TypeContext display = types;
    hir_case("hir.integer", display, hir_module(types, nodes, {}), 0);
  }
  {
    std::vector<HirNode> nodes;
    nodes.push_back(float_node(f32, 2164260864));
    TypeContext display = types;
    hir_case("hir.float", display, hir_module(types, nodes, {}), 0);
  }
  {
    std::vector<HirNode> nodes;
    nodes.push_back(bool_node(bool_type, true));
    TypeContext display = types;
    hir_case("hir.bool", display, hir_module(types, nodes, {}), 0);
  }
  {
    std::vector<HirNode> nodes;
    nodes.push_back(literal_node(i8, "127", 127));
    nodes.push_back(literal_node(i8, "1", 1));
    nodes.push_back(make_node(i8, Binary{BinaryOp::add},
                              {HirNodeId{0}, HirNodeId{1}}));
    TypeContext display = types;
    hir_case("hir.binary.wrap", display, hir_module(types, nodes, {}), 2);
  }
  {
    std::vector<HirNode> nodes;
    nodes.push_back(literal_node(i8, "-7", 249));
    nodes.push_back(literal_node(i8, "1", 1));
    nodes.push_back(make_node(i8, Binary{BinaryOp::shift_right},
                              {HirNodeId{0}, HirNodeId{1}}));
    TypeContext display = types;
    hir_case("hir.binary.shr", display, hir_module(types, nodes, {}), 2);
  }
  {
    std::vector<HirNode> nodes;
    nodes.push_back(bool_node(bool_type, false));
    nodes.push_back(bool_node(bool_type, true));
    nodes.push_back(make_node(bool_type, ShortCircuit{LogicalOp::and_},
                              {HirNodeId{0}, HirNodeId{1}}));
    TypeContext display = types;
    hir_case("hir.short.and", display, hir_module(types, nodes, {}), 2);
  }
  {
    std::vector<HirNode> nodes;
    nodes.push_back(bool_node(bool_type, true));
    nodes.push_back(bool_node(bool_type, false));
    nodes.push_back(make_node(bool_type, ShortCircuit{static_cast<LogicalOp>(1)},
                              {HirNodeId{0}, HirNodeId{1}}));
    TypeContext display = types;
    hir_case("hir.short.or", display, hir_module(types, nodes, {}), 2);
  }
  {
    std::vector<HirNode> nodes;
    nodes.push_back(literal_node(i32, "200", 200));
    nodes.push_back(make_node(u8, IntegerConvert{}, {HirNodeId{0}}));
    TypeContext display = types;
    hir_case("hir.convert.u8", display, hir_module(types, nodes, {}), 1);
  }
  {
    std::vector<HirNode> nodes;
    nodes.push_back(literal_node(i32, "256", 256));
    nodes.push_back(make_node(u8, IntegerConvert{}, {HirNodeId{0}}));
    TypeContext display = types;
    hir_case("hir.convert.overflow", display, hir_module(types, nodes, {}), 1);
  }
  {
    std::vector<HirNode> nodes;
    nodes.push_back(literal_node(u8, "0", 0));
    nodes.push_back(make_node(u8, Unary{UnaryOp::bit_not}, {HirNodeId{0}}));
    TypeContext display = types;
    hir_case("hir.unary.not", display, hir_module(types, nodes, {}), 1);
  }
  {
    std::vector<HirNode> nodes;
    nodes.push_back(float_node(f32, 1069547520));
    nodes.push_back(make_node(f32, Unary{UnaryOp::negate}, {HirNodeId{0}}));
    TypeContext display = types;
    hir_case("hir.unary.float", display, hir_module(types, nodes, {}), 1);
  }
  {
    std::vector<HirNode> nodes;
    nodes.push_back(literal_node(i32, "1", 1));
    nodes.push_back(literal_node(i32, "2", 2));
    StructInit init;
    init.fields.push_back(call_arg(1, 0));
    init.fields.push_back(call_arg(0, 1));
    nodes.push_back(make_node(struct_type, std::move(init),
                              {HirNodeId{0}, HirNodeId{1}}));
    TypeContext display = types;
    hir_case("hir.struct.reorder", display, hir_module(types, nodes, {}), 2);
  }
  {
    std::vector<HirNode> nodes;
    nodes.push_back(literal_node(i32, "3", 3));
    nodes.push_back(literal_node(i32, "4", 4));
    EnumInit init;
    init.variant = SymbolId{9};
    init.payload.push_back(call_arg(0, 0));
    init.payload.push_back(call_arg(1, 1));
    nodes.push_back(make_node(enum_type, std::move(init),
                              {HirNodeId{0}, HirNodeId{1}}));
    TypeContext display = types;
    hir_case("hir.enum.reorder", display, hir_module(types, nodes, {}), 2);
  }
  std::vector<NominalDecl> nominals;
  NominalDecl nominal_decl;
  nominal_decl.symbol = SymbolId{100};
  nominal_decl.kind = TypeKind::NominalStruct;
  nominal_decl.fields.push_back(field_decl(SymbolId{200}, i32));
  nominal_decl.fields.push_back(field_decl(SymbolId{201}, i32));
  nominals.push_back(nominal_decl);
  {
    std::vector<HirNode> nodes;
    nodes.push_back(literal_node(i32, "5", 5));
    nodes.push_back(literal_node(i32, "6", 6));
    StructInit init;
    init.fields.push_back(call_arg(0, 0));
    init.fields.push_back(call_arg(1, 1));
    nodes.push_back(make_node(struct_type, std::move(init),
                              {HirNodeId{0}, HirNodeId{1}}));
    nodes.push_back(make_node(i32, FieldRead{SymbolId{201}},
                              {HirNodeId{2}}));
    TypeContext display = types;
    hir_case("hir.field", display, hir_module(types, nodes, {}, nominals), 3);
  }
  {
    std::vector<HirNode> nodes;
    nodes.push_back(literal_node(i32, "9", 9));
    nodes.push_back(make_node(i32, Binding{SymbolId{0}, false},
                              {HirNodeId{0}}));
    nodes.push_back(make_node(i32, Read{SymbolId{0}}));
    std::vector<Symbol> symbols;
    symbols.push_back(symbol_with(0, "c0", SymbolKind::constant));
    TypeContext display = types;
    hir_case("hir.read", display, hir_module(types, nodes, symbols), 2);
  }
  {
    std::vector<HirNode> nodes;
    nodes.push_back(make_node(i32, Read{SymbolId{1}}));
    nodes.push_back(make_node(i32, Binding{SymbolId{1}, false},
                              {HirNodeId{0}}));
    std::vector<Symbol> symbols;
    symbols.push_back(symbol_with(0, "c0", SymbolKind::constant));
    symbols.push_back(symbol_with(1, "c1", SymbolKind::constant));
    TypeContext display = types;
    hir_case("hir.cycle", display, hir_module(types, nodes, symbols), 0);
  }
  {
    std::vector<HirNode> nodes;
    ConstantValue value;
    value.type = i32;
    value.data = IntegerLiteral{"77", 77};
    nodes.push_back(make_node(i32, ConstantLiteral{value}));
    TypeContext display = types;
    hir_case("hir.constant_literal", display, hir_module(types, nodes, {}), 0);
  }
  {
    std::vector<HirNode> nodes;
    nodes.push_back(make_node(i32, Block{}));
    TypeContext display = types;
    hir_case("hir.unsupported", display, hir_module(types, nodes, {}), 0);
  }
  {
    std::vector<HirNode> nodes;
    nodes.push_back(literal_node(i32, "1", 1));
    for (size_t index = 0; index < 129; ++index) {
      HirNodeId previous{nodes.size() - 1};
      nodes.push_back(make_node(i32, Unary{UnaryOp::negate}, {previous}));
    }
    TypeContext display = types;
    hir_case("hir.depth", display, hir_module(types, nodes, {}),
             nodes.size() - 1);
  }
  {
    std::vector<HirNode> nodes;
    nodes.push_back(literal_node(i32, "1", 1));
    StructInit init;
    init.fields.push_back(call_arg(0, 0));
    nodes.push_back(make_node(struct_type, std::move(init), {HirNodeId{0}}));
    TypeContext display = types;
    HirModule module = hir_module(types, nodes, {});
    std::cout << "agg.struct="
              << (aggregate_constant_expression(module, HirNodeId{1})
                      ? "true"
                      : "false")
              << "\n";
    std::cout << "agg.literal="
              << (aggregate_constant_expression(module, HirNodeId{0})
                      ? "true"
                      : "false")
              << "\n";
  }
  {
    std::vector<HirNode> nodes;
    nodes.push_back(literal_node(i32, "1", 1));
    StructInit init;
    init.fields.push_back(call_arg(0, 0));
    nodes.push_back(make_node(struct_type, std::move(init), {HirNodeId{0}}));
    nodes.push_back(make_node(struct_type, Binding{SymbolId{2}, false},
                              {HirNodeId{1}}));
    nodes.push_back(make_node(struct_type, Read{SymbolId{2}}));
    std::vector<Symbol> symbols;
    symbols.push_back(symbol_with(0, "a0", SymbolKind::constant));
    symbols.push_back(symbol_with(1, "a1", SymbolKind::constant));
    symbols.push_back(symbol_with(2, "a2", SymbolKind::constant));
    TypeContext display = types;
    HirModule module = hir_module(types, nodes, symbols);
    std::cout << "agg.read="
              << (aggregate_constant_expression(module, HirNodeId{2})
                      ? "true"
                      : "false")
              << "\n";
  }
  {
    std::vector<HirNode> nodes;
    nodes.push_back(literal_node(i32, "1", 1));
    StructInit init;
    for (size_t index = 0; index < 99999; ++index)
      init.fields.push_back(call_arg(0, 0));
    nodes.push_back(make_node(struct_type, std::move(init), {HirNodeId{0}}));
    TypeContext display = types;
    aggregate_case("hir.work.limit.ok", display,
                   hir_module(types, nodes, {}), 1);
  }
  {
    std::vector<HirNode> nodes;
    nodes.push_back(literal_node(i32, "1", 1));
    StructInit init;
    for (size_t index = 0; index < 100000; ++index)
      init.fields.push_back(call_arg(0, 0));
    nodes.push_back(make_node(struct_type, std::move(init), {HirNodeId{0}}));
    TypeContext display = types;
    aggregate_case("hir.work.limit.over", display,
                   hir_module(types, nodes, {}), 1);
  }
}

void same_transform_tests(TypeContext &types) {
  const TypeId i32 = scalar(types, TypeKind::SignedInt, 32);
  const TypeId i8 = scalar(types, TypeKind::SignedInt, 8);
  const TypeId f32 = scalar(types, TypeKind::Float, 32);
  const TypeId bool_type = scalar(types, TypeKind::Bool);
  const TypeId string_type = scalar(types, TypeKind::String);
  const TypeId struct_type = nominal(types, TypeKind::NominalStruct,
                                     SymbolId{500});
  auto same = [](const std::string &label, const ConstantValue &left,
                 const ConstantValue &right) {
    std::cout << label << "="
              << (same_constant_value(left, right) ? "true" : "false") << "\n";
  };
  ConstantValue int_one{i32, IntegerLiteral{"1", 1}};
  ConstantValue int_one_other_bits{i32, IntegerLiteral{"1", 2}};
  ConstantValue int_one_i8{i8, IntegerLiteral{"1", 1}};
  ConstantValue float_nan{f32, FloatLiteral{0x7fc00000}};
  ConstantValue float_nan_same{f32, FloatLiteral{0x7fc00000}};
  ConstantValue bool_true{bool_type, BoolLiteral{true}};
  ConstantValue string_a{string_type, StringLiteral{"a\0😀"}};
  ConstantValue string_b{string_type, StringLiteral{"a\0😀"}};
  same("same.int.equal", int_one, int_one);
  same("same.int.bits", int_one, int_one_other_bits);
  same("same.int.type", int_one, int_one_i8);
  same("same.float.nan", float_nan, float_nan_same);
  same("same.bool", bool_true, bool_true);
  same("same.string", string_a, string_b);
  ConstantValue function_a{TypeId{0}, FunctionValue{SymbolId{7}, {i32}, {}, false, {}}};
  ConstantValue function_b{TypeId{0}, FunctionValue{SymbolId{7}, {i32}, {}, false, {}}};
  ConstantValue function_c{TypeId{0}, FunctionValue{SymbolId{8}, {i32}, {}, false, {}}};
  ConstantValue function_d{TypeId{0}, FunctionValue{SymbolId{7}, {i8}, {}, false, {}}};
  same("same.function", function_a, function_b);
  same("same.function.symbol", function_a, function_c);
  same("same.function.args", function_a, function_d);
  ConstantValue fields_a{struct_type, ConstantAggregate{std::nullopt,
                                                       {int_one, function_a}}};
  ConstantValue fields_b{struct_type, ConstantAggregate{std::nullopt,
                                                       {int_one, function_a}}};
  ConstantValue fields_c{struct_type, ConstantAggregate{std::nullopt,
                                                       {function_a, int_one}}};
  same("same.aggregate.equal", fields_a, fields_b);
  same("same.aggregate.order", fields_a, fields_c);
  same("same.aggregate.variant", fields_a,
       ConstantValue{struct_type, ConstantAggregate{SymbolId{3},
                                                    {int_one, function_a}}});
  const TypeId parameter_type = nominal(types, TypeKind::TypeParameter,
                                        SymbolId{42});
  const TypeId parameter_function = parameterized(types, parameter_type, i32);
  const TypeId concrete_function = parameterized(types, i32, i32);
  (void)concrete_function;
  const std::vector<Substitutions> dummy;
  (void)dummy;
  Substitutions substitutions{{SymbolId{42}, i32}};
  ConstantValue function_constant{parameter_function,
                                  FunctionValue{SymbolId{10}, {parameter_type}, {}, false, {}}};
  auto change = [i32](FunctionValue &value) {
    value.symbol = SymbolId{value.symbol.value + 1};
    value.captures.push_back(
        CaptureSource{SymbolId{3}, SymbolId{4}, TypeId{5}, false});
    value.move_capture = true;
    value.bound = BoundReceiver{i32, ParamMode::inout};
  };
  transform_constant(function_constant, substitutions, types, change);
  print_constant("transform.function", types, function_constant);
  ConstantValue nested_function{parameter_function,
                                FunctionValue{SymbolId{20}, {parameter_type}, {}, false, {}}};
  ConstantValue aggregate{struct_type,
                          ConstantAggregate{std::nullopt,
                                            {nested_function,
                                             ConstantValue{i32,
                                                           IntegerLiteral{"9",
                                                                          9}}}}};
  transform_constant(aggregate, substitutions, types, change);
  print_constant("transform.aggregate", types, aggregate);
}

} // namespace

int main() {
  TypeContext types;
  literal_tests(types);
  source_tests(types);
  source_special_tests(types);
  hir_tests(types);
  same_transform_tests(types);
  return 0;
}
