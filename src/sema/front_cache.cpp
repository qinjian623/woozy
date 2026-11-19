#include "lang/sema/front_cache.hpp"
#include "../hir/constant_value.hpp"
#include "../hir/literal_value.hpp"
#include "lang/hir/identity.hpp"
#include "lang/modules/declaration_key.hpp"
#include "lang/support/cache_io.hpp"
#include <algorithm>
#include <functional>
#include <map>
#include <tuple>

namespace lang::sema {
namespace {
using namespace hir;
using Ref = modules::NodeRef;
struct UnsupportedCache : std::exception {};
std::string interface_text(const modules::Module &package) {
  std::vector<std::string> records;
  for (const auto &file : package.files) {
    // Imports are file-local. Retain their semantic targets when a public
    // signature/template uses one, without depending on source byte offsets.
    std::string imports;
    for (const auto &import : package.imports) {
      if (import.file >= package.files.size() || package.files[import.file] != file) continue;
      modules::append_key_component(imports, import.alias);
      // Logical import spellings, unlike ModuleId, survive allocation changes.
      const auto &node = file->node(import.node);
      modules::append_key_component(imports, *file->tokens.at(node.token_begin + 1).string_value);
    }
    for (auto id : file->node(file->root).children) {
      const auto &node = file->node(id);
      bool exported = file->tokens.at(node.token_begin).spelling == "pub";
      bool generic = false;
      for (auto child : node.children)
        if (file->node(child).kind == AstKind::GenericParameter) generic = true;
      if (!exported && !generic && node.kind != AstKind::Impl && node.kind != AstKind::Trait) continue;
      std::string record;
      // Include complete type layouts/const values and checked template syntax.
      // Ordinary function implementation tokens do not enter the interface.
      size_t end = node.token_end;
      if (node.kind == AstKind::Function && !generic)
        for (auto child : node.children)
          if (file->node(child).kind == AstKind::Block) end = file->node(child).token_begin;
      for (size_t token = node.token_begin; token < end; ++token)
        modules::append_key_component(record, file->tokens.at(token).spelling);
      modules::append_key_component(record, imports);
      records.push_back(std::move(record));
    }
  }
  std::sort(records.begin(), records.end());
  cache::Writer out;
  for (const auto &record : records) out.string(record);
  return out.bytes;
}
std::string module_key(const FrontCache &cache, const HirModule &module, ModuleId id) {
  const auto &project = *module.resolved->project;
  const auto &package = project.module(id);
  lang::cache::Writer out;
  out.string("wz-front-scalar/1"); out.string(cache.fingerprint); out.string(package.identity);
  out.number(project.word_bits); out.number(project.hosted); out.string(project.backend);
  for (const auto &file : package.files) { out.string(file->source->path); out.string(sha256(file->source->original)); }
  std::vector<std::string> dependencies;
  for (const auto &import : package.imports) {
    const auto &dependency = project.module(import.target);
    dependencies.push_back(dependency.identity + interface_text(dependency));
  }
  // Impl visibility is project-wide in the current language. A change to the
  // impl set must be considered even outside a direct import edge.
  for (const auto &other : project.modules)
    for (const auto &file : other.files)
      for (auto node : file->node(file->root).children)
        if (file->node(node).kind == AstKind::Impl)
          dependencies.push_back(other.identity + interface_text(other));
  for (const auto &binding : project.bindings) dependencies.push_back(binding.sha256);
  for (const auto &binding : project.c_bindings) dependencies.push_back(binding.sha256);
  std::sort(dependencies.begin(), dependencies.end());
  dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());
  for (const auto &dependency : dependencies) out.string(dependency);
  return sha256(out.bytes);
}
bool same_ref(Ref left, Ref right) {
  return left.module == right.module && left.file == right.file && left.node == right.node;
}
Ref unwrap(const HirModule &module, Ref reference) {
  const auto &file = *module.resolved->project->module(reference.module).files.at(reference.file);
  for (;;) {
    const auto &node = file.node(reference.node);
    if ((node.kind == AstKind::Parenthesized || node.kind == AstKind::BracketItem || node.kind == AstKind::Argument) && node.children.size() == 1)
      reference.node = node.children.front();
    else return reference;
  }
}
using SourceKey = std::tuple<uint64_t, size_t, uint64_t>;
SourceKey source_key(Ref value) { return {value.module.value, value.file, value.node.value}; }
struct SourceOccurrences {
  std::map<SourceKey, std::vector<SymbolId>> values;
  explicit SourceOccurrences(const HirModule &module) {
    for (const auto &occurrence : module.resolved->occurrences)
      values[source_key(unwrap(module, occurrence.source))].push_back(occurrence.symbol);
  }
};
void validate_source(const HirModule &module, const HirNode &node, const SourceOccurrences &occurrences) {
  const auto reference = unwrap(module, node.source.node);
  const auto &file = *module.resolved->project->module(reference.module).files.at(reference.file);
  const auto &syntax = file.node(reference.node);
  auto need = [](bool value) { if (!value) throw UnsupportedCache{}; };
  auto children = [&](const std::vector<AstNodeId> &expected) {
    need(expected.size() == node.children.size());
    for (size_t i = 0; i < expected.size(); ++i)
      need(same_ref(unwrap(module, module.nodes.at(node.children[i].value).source.node),
          unwrap(module, {reference.module, reference.file, expected[i]})));
  };
  auto source_symbol = [&](SymbolId expected) {
    auto found = occurrences.values.find(source_key(reference));
    need(found != occurrences.values.end() && !found->second.empty());
    for (auto symbol : found->second) need(symbol == expected);
  };
  auto operator_spelling = [&]() {
    need(syntax.children.size() == 2);
    return file.tokens.at(file.node(syntax.children.front()).token_end).spelling;
  };
  constexpr std::string_view operators[] = {"+", "-", "*", "/", "%", "<<", ">>", "&", "|", "^", "==", "!=", "<", "<=", ">", ">="};
  switch (node.kind()) {
  case NodeKind::bool_literal: case NodeKind::integer_literal:
  case NodeKind::float_literal: case NodeKind::string_literal: {
    auto context = module.types;
    auto expected = source_constant_value(module, context, reference, node.type,
        [](Ref) -> TypeId { throw UnsupportedCache{}; },
        [](Ref, TypeId) -> FunctionValue { throw UnsupportedCache{}; });
    ConstantValue actual{node.type, UnitLiteral{}};
    std::visit([&](const auto &value) {
      using T = std::decay_t<decltype(value)>;
      if constexpr (std::is_same_v<T, BoolLiteral> || std::is_same_v<T, IntegerLiteral> ||
          std::is_same_v<T, FloatLiteral> || std::is_same_v<T, StringLiteral>) actual.data = value;
      else throw UnsupportedCache{};
    }, node.data);
    need(same_constant_value(actual, expected)); break;
  }
  case NodeKind::read: source_symbol(std::get<Read>(node.data).symbol); break;
  case NodeKind::place: source_symbol(std::get<Place>(node.data).symbol); break;
  case NodeKind::binding: {
    const auto value = std::get<Binding>(node.data);
    const auto &symbol = module.resolved->symbols.at(value.symbol.value);
    need(syntax.kind == AstKind::Binding && symbol.definition && same_ref(*symbol.definition, reference));
    need(value.writable == (file.tokens.at(syntax.token_begin).spelling == "var"));
    need(!syntax.children.empty()); children({syntax.children.back()}); break;
  }
  case NodeKind::assign: {
    need(syntax.kind == AstKind::Assign); children(syntax.children);
    const auto compound = std::get<Assign>(node.data).compound;
    need(operator_spelling() == (compound ? std::string(operators[static_cast<unsigned>(*compound)]) + "=" : "=")); break;
  }
  case NodeKind::binary:
    need(syntax.kind == AstKind::Binary); children(syntax.children);
    need(operator_spelling() == operators[static_cast<unsigned>(std::get<Binary>(node.data).op)]); break;
  case NodeKind::short_circuit:
    need(syntax.kind == AstKind::Binary); children(syntax.children);
    need(operator_spelling() == (std::get<ShortCircuit>(node.data).op == LogicalOp::and_ ? "&&" : "||")); break;
  case NodeKind::unary: {
    need(syntax.kind == AstKind::Unary); children(syntax.children);
    constexpr std::string_view unary[] = {"-", "!", "~"};
    need(file.tokens.at(syntax.token_begin).spelling == unary[static_cast<unsigned>(std::get<Unary>(node.data).op)]); break;
  }
  case NodeKind::block: need(syntax.kind == AstKind::Block); children(syntax.children); break;
  case NodeKind::expression_statement: need(syntax.kind == AstKind::ExpressionStatement); children(syntax.children); break;
  case NodeKind::if_: need(syntax.kind == AstKind::If); children(syntax.children); break;
  case NodeKind::return_: need(syntax.kind == AstKind::Return); children(syntax.children); break;
  // These already reconstruct the real source target/arguments in hir::verify.
  case NodeKind::call: case NodeKind::integer_convert: break;
  default: throw UnsupportedCache{};
  }
}
void validate_structure(const HirModule &module, size_t node_base, size_t scope_base,
                        size_t function_base, const std::set<uint64_t> &functions) {
  auto need = [](bool value) { if (!value) throw UnsupportedCache{}; };
  const auto node_count = module.nodes.size() - node_base;
  const auto scope_count = module.scopes.size() - scope_base;
  for (size_t i = scope_base; i < module.scopes.size(); ++i) {
    const auto &scope = module.scopes[i];
    need(functions.contains(scope.function.value));
    auto parent = scope.parent; size_t depth = 0;
    while (parent) {
      need(parent->value >= scope_base && parent->value < module.scopes.size() && ++depth <= scope_count);
      const auto &outer = module.scopes[parent->value];
      need(outer.function == scope.function); parent = outer.parent;
    }
  }
  for (size_t i = function_base; i < module.functions.size(); ++i) {
    const auto &function = module.functions[i];
    need(module.resolved->symbols.at(function.symbol.value).kind == modules::SymbolKind::function);
    const auto *signature = module.types.find(function.signature);
    need(signature && signature->kind == TypeKind::Function && !signature->throws_type &&
         signature->parameters.size() == function.parameters.size());
    need(function.scope.value >= scope_base && function.scope.value < module.scopes.size() &&
         module.scopes[function.scope.value].function == function.symbol);
    need(function.body.value >= node_base && function.body.value < module.nodes.size() &&
         module.nodes[function.body.value].scope == function.scope);
  }
  for (size_t i = node_base; i < module.nodes.size(); ++i) {
    const auto &node = module.nodes[i];
    need(node.scope.value >= scope_base && node.scope.value < module.scopes.size());
    if (const auto *call = std::get_if<Call>(&node.data)) {
      const auto *signature = module.types.find(call->signature);
      need(signature && signature->kind == TypeKind::Function && !signature->throws_type &&
           call->args.size() == signature->parameters.size() && node.type == signature->result);
      const bool indirect = call->kind == CalleeKind::function_value;
      need(call->callee.has_value() == indirect && call->direct_symbol.has_value() != indirect &&
           node.children.size() == call->args.size() + (indirect ? 1 : 0));
      if (indirect) need(node.children.front() == *call->callee);
      std::vector<bool> parameters(call->args.size());
      for (size_t a = 0; a < call->args.size(); ++a) {
        const auto &argument = call->args[a];
        need(argument.source_index == a && argument.parameter_index < parameters.size() &&
             !parameters[argument.parameter_index]);
        parameters[argument.parameter_index] = true;
        need(node.children[a + (indirect ? 1 : 0)] == argument.expression);
      }
    }
    if (const auto *returned = std::get_if<Return>(&node.data))
      need(returned->function == module.scopes[node.scope.value].function);
  }
  std::vector<uint8_t> colors(node_count);
  for (size_t root = 0; root < node_count; ++root) {
    if (colors[root]) continue;
    std::vector<std::pair<size_t, size_t>> stack{{root, 0}}; colors[root] = 1;
    while (!stack.empty()) {
      auto &[current, cursor] = stack.back();
      const auto &children = module.nodes[node_base + current].children;
      if (cursor == children.size()) { colors[current] = 2; stack.pop_back(); continue; }
      const auto child = children[cursor++].value;
      need(child >= node_base && child < module.nodes.size());
      const auto local = child - node_base; need(colors[local] != 1);
      if (!colors[local]) { colors[local] = 1; stack.emplace_back(local, 0); }
    }
  }
}
struct Encoder {
  const HirModule &module;
  ModuleId package;
  IdentityKeys &keys;
  const SourceOccurrences &occurrences;
  cache::Writer out;
  std::map<uint64_t, size_t> nodes, scopes;
  void symbol(SymbolId value) { out.string(keys.declaration_key(value)); }
  void type(TypeId id, size_t depth = 0) {
    const auto *value = module.types.find(id);
    if (!value || depth > 64 || static_cast<unsigned>(value->kind) > 8 || value->throws_type)
      throw UnsupportedCache{};
    out.number(static_cast<unsigned>(value->kind)); out.number(value->bits);
    if (value->kind == TypeKind::Function) {
      out.number(value->parameters.size());
      for (const auto &parameter : value->parameters) {
        out.number(static_cast<unsigned>(parameter.mode)); type(parameter.type, depth + 1);
      }
      type(value->result, depth + 1);
    }
  }
  void source(Source value) {
    if (value.node.module != package) throw UnsupportedCache{};
    const auto &file = *module.resolved->project->module(package).files.at(value.node.file);
    out.string(file.source->path);
    const auto path = keys.source_path(value.node);
    out.number(path.size()); for (auto index : path) out.number(index);
    out.number(value.span.begin_byte); out.number(value.span.end_byte);
  }
  void node(HirNodeId id) { out.number(nodes.at(id.value)); }
  void scope(ScopeId id) { out.number(scopes.at(id.value)); }
  void argument(const CallArg &value) {
    out.number(value.source_index); out.number(value.parameter_index);
    out.number(value.label.has_value()); if (value.label) out.string(*value.label);
    out.number(static_cast<unsigned>(value.mode)); node(value.expression);
  }
  void data(const HirNode &value) {
    if (!value.errors.empty()) throw UnsupportedCache{};
    out.number(value.data.index());
    std::visit([&](const auto &data) {
      using T = std::decay_t<decltype(data)>;
      if constexpr (std::is_same_v<T, UnitLiteral> || std::is_same_v<T, IntegerConvert> ||
          std::is_same_v<T, Block> || std::is_same_v<T, ExpressionStatement> ||
          std::is_same_v<T, If> || std::is_same_v<T, For> || std::is_same_v<T, Noop>) {}
      else if constexpr (std::is_same_v<T, BoolLiteral>) out.number(data.value);
      else if constexpr (std::is_same_v<T, IntegerLiteral>) { out.string(data.decimal); out.number(data.bits); }
      else if constexpr (std::is_same_v<T, FloatLiteral>) out.number(data.bits);
      else if constexpr (std::is_same_v<T, StringLiteral>) out.string(data.utf8);
      else if constexpr (std::is_same_v<T, Read> || std::is_same_v<T, Place> || std::is_same_v<T, Binding>) {
        symbol(data.symbol); if constexpr (std::is_same_v<T, Binding>) out.number(data.writable);
      } else if constexpr (std::is_same_v<T, FunctionValue>) {
        if (!data.type_args.empty() || !data.captures.empty() || data.bound || data.move_capture) throw UnsupportedCache{};
        symbol(data.symbol);
      } else if constexpr (std::is_same_v<T, Assign>) {
        out.number(data.compound.has_value()); if (data.compound) out.number(static_cast<unsigned>(*data.compound));
      } else if constexpr (std::is_same_v<T, Unary> || std::is_same_v<T, Binary> || std::is_same_v<T, ShortCircuit>)
        out.number(static_cast<unsigned>(data.op));
      else if constexpr (std::is_same_v<T, Return>) symbol(data.function);
      else if constexpr (std::is_same_v<T, Break> || std::is_same_v<T, Continue>) node(data.loop);
      else if constexpr (std::is_same_v<T, Call>) {
        if ((data.kind != CalleeKind::direct && data.kind != CalleeKind::function_value) ||
            data.binding || data.c_callable || data.system || !data.type_args.empty() || !data.omitted.empty()) throw UnsupportedCache{};
        out.number(static_cast<unsigned>(data.kind)); type(data.signature);
        out.number(data.direct_symbol.has_value()); if (data.direct_symbol) symbol(*data.direct_symbol);
        out.number(data.callee.has_value()); if (data.callee) node(*data.callee);
        out.number(data.args.size()); for (const auto &arg : data.args) argument(arg);
      } else throw UnsupportedCache{};
    }, value.data);
  }
};
struct Decoder {
  HirModule &module;
  ModuleId package;
  cache::Reader in;
  const std::map<std::string, SymbolId> &symbols;
  size_t node_base, scope_base, node_count = 0, scope_count = 0;
  Decoder(HirModule &value, ModuleId id, std::string_view bytes,
          const std::map<std::string, SymbolId> &names)
      : module(value), package(id), in{bytes}, symbols(names),
        node_base(value.nodes.size()), scope_base(value.scopes.size()) {}
  SymbolId symbol() {
    auto found = symbols.find(in.string());
    if (found == symbols.end()) throw UnsupportedCache{};
    return found->second;
  }
  TypeId type(size_t depth = 0) {
    if (depth > 64) throw UnsupportedCache{};
    auto kind = in.number();
    if (kind > 8) throw UnsupportedCache{};
    Type value; value.kind = static_cast<TypeKind>(kind);
    auto bits = in.number(); if (bits > 64) throw UnsupportedCache{}; value.bits = static_cast<unsigned>(bits);
    if (value.kind == TypeKind::Function) {
      auto count = in.count();
      for (size_t i = 0; i < count; ++i) {
        auto mode = in.number(); if (mode > 2) throw UnsupportedCache{};
        value.parameters.push_back({static_cast<ParamMode>(mode), type(depth + 1)});
      }
      value.result = type(depth + 1);
    }
    return module.types.intern(std::move(value));
  }
  Source source() {
    const auto path = in.string();
    const auto &files = module.resolved->project->module(package).files;
    size_t file_index = 0;
    while (file_index < files.size() && files[file_index]->source->path != path) ++file_index;
    if (file_index == files.size()) throw UnsupportedCache{};
    const auto &file = *files[file_index];
    auto id = file.root;
    auto count = in.count(); if (count > 1024) throw UnsupportedCache{};
    for (size_t i = 0; i < count; ++i) id = file.node(id).children.at(in.number());
    auto begin = in.number(), end = in.number();
    if (begin > end || end > file.source->original.size()) throw UnsupportedCache{};
    file.source->position(begin); file.source->position(end);
    return {{package, file_index, id}, {file.source->id, begin, end}};
  }
  HirNodeId node() {
    auto id = in.number(); if (id >= node_count) throw UnsupportedCache{};
    return HirNodeId{node_base + id};
  }
  ScopeId scope() {
    auto id = in.number(); if (id >= scope_count) throw UnsupportedCache{};
    return ScopeId{scope_base + id};
  }
  template<class E> E enumeration(unsigned limit) {
    auto value = in.number(); if (value > limit) throw UnsupportedCache{};
    return static_cast<E>(value);
  }
  CallArg argument() {
    auto source_index = in.number(), parameter_index = in.number();
    if (source_index > UINT32_MAX || parameter_index > UINT32_MAX) throw UnsupportedCache{};
    std::optional<std::string> label; if (in.flag()) label = in.string();
    auto mode = enumeration<ParamMode>(2); auto expression = node();
    return {static_cast<uint32_t>(source_index), static_cast<uint32_t>(parameter_index), label, mode, expression};
  }
  NodeData data() {
    auto tag = in.number();
    switch (static_cast<NodeKind>(tag)) {
    case NodeKind::unit_literal: return UnitLiteral{};
    case NodeKind::bool_literal: return BoolLiteral{in.flag()};
    case NodeKind::integer_literal: { auto decimal = in.string(); return IntegerLiteral{decimal, in.number()}; }
    case NodeKind::float_literal: return FloatLiteral{in.number()};
    case NodeKind::string_literal: return StringLiteral{in.string()};
    case NodeKind::read: return Read{symbol()};
    case NodeKind::place: return Place{symbol()};
    case NodeKind::function_value: return FunctionValue{symbol(), {}};
    case NodeKind::binding: { auto id = symbol(); return Binding{id, in.flag()}; }
    case NodeKind::assign: { Assign result; if (in.flag()) result.compound = enumeration<BinaryOp>(15); return result; }
    case NodeKind::unary: return Unary{enumeration<UnaryOp>(2)};
    case NodeKind::binary: return Binary{enumeration<BinaryOp>(15)};
    case NodeKind::short_circuit: return ShortCircuit{enumeration<LogicalOp>(1)};
    case NodeKind::integer_convert: return IntegerConvert{};
    case NodeKind::call: {
      Call result; result.kind = enumeration<CalleeKind>(1); result.signature = type();
      if (in.flag()) result.direct_symbol = symbol(); if (in.flag()) result.callee = node();
      auto count = in.count(); for (size_t i = 0; i < count; ++i) result.args.push_back(argument());
      return result;
    }
    case NodeKind::block: return Block{};
    case NodeKind::expression_statement: return ExpressionStatement{};
    case NodeKind::if_: return If{};
    case NodeKind::for_: return For{};
    case NodeKind::noop: return Noop{};
    case NodeKind::return_: return Return{symbol()};
    case NodeKind::break_: return Break{node()};
    case NodeKind::continue_: return Continue{node()};
    default: throw UnsupportedCache{};
    }
  }
};
}
std::string FrontCache::package_key(const HirModule &module, ModuleId id) const {
  return module_key(*this, module, id);
}
std::set<uint64_t> FrontCache::restore(HirModule &module) {
  std::set<uint64_t> restored;
  if (disabled) return restored;
  IdentityKeys keys(module);
  const SourceOccurrences occurrences(module);
  std::map<std::string, SymbolId> symbols;
  for (const auto &symbol : module.resolved->symbols) {
    try {
      if (!symbols.emplace(keys.declaration_key(symbol.id), symbol.id).second)
        throw UnsupportedCache{};
    } catch (const std::invalid_argument &) {}
  }
  for (const auto &package : module.resolved->project->modules) {
    if (package.files.empty()) continue;
    ++misses;
    const auto old_nodes = module.nodes.size(), old_scopes = module.scopes.size(), old_functions = module.functions.size();
    try {
      const auto key = module_key(*this, module, package.id);
      auto bytes = cache::load(directory / (key + ".hir"));
      if (!bytes) continue;
      Decoder decoder(module, package.id, *bytes, symbols);
      if (decoder.in.string() != "wz-front-scalar/1" || decoder.in.string() != key) continue;
      decoder.node_count = decoder.in.count(); decoder.scope_count = decoder.in.count();
      size_t source_nodes = 0;
      for (const auto &file : package.files) source_nodes += file->nodes.size();
      if (decoder.node_count > source_nodes * 8 + 16 || decoder.scope_count > source_nodes * 2 + 8) throw UnsupportedCache{};
      for (size_t i = 0; i < decoder.scope_count; ++i) {
        auto function = decoder.symbol();
        if (module.resolved->symbols.at(function.value).module != package.id) throw UnsupportedCache{};
        std::optional<ScopeId> parent; if (decoder.in.flag()) parent = decoder.scope();
        module.scopes.push_back({function, parent, decoder.source()});
      }
      for (size_t i = 0; i < decoder.node_count; ++i) {
        auto type = decoder.type(); auto source = decoder.source(); auto scope = decoder.scope();
        std::vector<HirNodeId> children;
        auto count = decoder.in.count(); for (size_t c = 0; c < count; ++c) children.push_back(decoder.node());
        module.nodes.push_back({type, source, scope, children, decoder.data(), {}, {}});
      }
      auto count = decoder.in.count(); if (!count) throw UnsupportedCache{};
      std::set<uint64_t> package_functions;
      for (size_t i = 0; i < count; ++i) {
        auto symbol = decoder.symbol(); auto signature = decoder.type();
        if (module.resolved->symbols.at(symbol.value).module != package.id || !package_functions.insert(symbol.value).second)
          throw UnsupportedCache{};
        std::vector<Parameter> parameters;
        auto params = decoder.in.count();
        for (size_t j = 0; j < params; ++j) { auto parameter = decoder.symbol(); parameters.push_back({parameter, decoder.in.string()}); }
        auto body = decoder.node(); auto scope = decoder.scope(); auto source = decoder.source();
        module.functions.push_back({symbol, signature, parameters, body, scope, source});
      }
      decoder.in.done();
      validate_structure(module, old_nodes, old_scopes, old_functions, package_functions);
      for (size_t i = old_nodes; i < module.nodes.size(); ++i) validate_source(module, module.nodes[i], occurrences);
      restored.insert(package_functions.begin(), package_functions.end()); --misses; ++hits;
    } catch (const std::exception &) {
      module.nodes.resize(old_nodes); module.scopes.resize(old_scopes); module.functions.resize(old_functions); ++rejected;
    }
  }
  return restored;
}
void FrontCache::store(const HirModule &module) {
  IdentityKeys keys(module);
  const SourceOccurrences occurrences(module);
  for (const auto &package : module.resolved->project->modules) {
    if (package.files.empty()) continue;
    try {
      Encoder encoder{module, package.id, keys, occurrences};
      std::vector<const HirFunction *> functions;
      for (const auto &item : module.templates)
        if (keys.function_owner(item.function.symbol) == package.id) throw UnsupportedCache{};
      for (const auto &function : module.functions)
        if (keys.function_owner(function.symbol) == package.id) {
          if (function.async || !function.captures.empty() || std::any_of(module.instances.begin(), module.instances.end(),
              [&](const auto &instance) { return instance.symbol == function.symbol; })) throw UnsupportedCache{};
          functions.push_back(&function);
        }
      if (functions.empty()) continue;
      for (size_t i = 0; i < module.scopes.size(); ++i)
        if (keys.function_owner(module.scopes[i].function) == package.id) encoder.scopes.emplace(i, encoder.scopes.size());
      for (size_t i = 0; i < module.nodes.size(); ++i)
        if (encoder.scopes.contains(module.nodes[i].scope.value)) encoder.nodes.emplace(i, encoder.nodes.size());
      const auto key = module_key(*this, module, package.id);
      encoder.out.string("wz-front-scalar/1"); encoder.out.string(key);
      encoder.out.number(encoder.nodes.size()); encoder.out.number(encoder.scopes.size());
      for (const auto &[index, local] : encoder.scopes) {
        const auto &scope = module.scopes.at(index); encoder.symbol(scope.function);
        encoder.out.number(scope.parent.has_value()); if (scope.parent) encoder.scope(*scope.parent);
        encoder.source(scope.source);
      }
      for (const auto &[index, local] : encoder.nodes) {
        const auto &node = module.nodes.at(index); validate_source(module, node, occurrences); encoder.type(node.type); encoder.source(node.source); encoder.scope(node.scope);
        encoder.out.number(node.children.size()); for (auto child : node.children) encoder.node(child);
        encoder.data(node);
      }
      encoder.out.number(functions.size());
      for (const auto *function : functions) {
        encoder.symbol(function->symbol); encoder.type(function->signature); encoder.out.number(function->parameters.size());
        for (const auto &parameter : function->parameters) { encoder.symbol(parameter.symbol); encoder.out.string(parameter.label); }
        encoder.node(function->body); encoder.scope(function->scope); encoder.source(function->source);
      }
      cache::store(directory, key + ".hir", encoder.out.bytes);
    } catch (const UnsupportedCache &) {} catch (const LiteralError &) {} catch (const std::out_of_range &) {} catch (const std::invalid_argument &) {}
  }
}
}
