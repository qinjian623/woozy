#include "lang/hir/identity.hpp"
#include "lang/modules/declaration_key.hpp"
#include "lang/support/digest.hpp"
#include <filesystem>
#include <algorithm>
#include <map>
#include <mutex>
#include <tuple>

namespace lang::hir {
namespace {
using Ref = modules::NodeRef;
using NodeKey = std::tuple<uint64_t, size_t, uint64_t>;
NodeKey node_key(Ref value) { return {value.module.value, value.file, value.node.value}; }
using TokenKey = std::tuple<uint64_t, size_t, uint64_t, uint64_t>;
struct NodeInfo {
  std::optional<Ref> parent;
  size_t ordinal = 0, depth = 0;
  bool reachable = false;
  std::optional<SymbolId> named;
};
template<class K, class V, class F>
V memo(std::map<K, std::optional<V>> &values, K key, F compute) {
  auto [entry, inserted] = values.emplace(key, std::nullopt);
  if (!inserted) {
    if (!entry->second) throw std::invalid_argument("identity: cyclic logical key");
    return *entry->second;
  }
  try { entry->second = compute(); return *entry->second; }
  catch (...) { values.erase(entry); throw; }
}
}
struct IdentityKeys::Impl {
  std::optional<VerifiedHirModule> keep;
  const HirModule &module;
  std::shared_ptr<const modules::ResolvedProject> resolved;
  size_t symbol_count, instance_count;
  Config config;
  std::map<NodeKey, NodeInfo> nodes;
  std::map<TokenKey, ModuleId> module_uses;
  std::map<SymbolId, Ref> self;
  struct InstanceRef { const FunctionInstance *instance; std::optional<SymbolId> original; };
  std::map<SymbolId, InstanceRef> instances;
  std::map<NodeKey, std::optional<std::string>> sources;
  std::map<SymbolId, std::optional<std::string>> declarations, names;
  std::map<TypeId, std::optional<std::string>> types;
  std::map<SymbolId, std::optional<ModuleId>> owners;
  mutable std::mutex mutex;

  explicit Impl(const HirModule &value)
      : module(value), resolved(value.resolved), symbol_count(resolved ? resolved->symbols.size() : 0),
        instance_count(value.instances.size()), config(value.config) {
    if (!resolved || !resolved->project) throw std::invalid_argument("identity: missing resolved project");
    for (const auto &package : resolved->project->modules)
      for (size_t file_index = 0; file_index < package.files.size(); ++file_index) {
        const auto &file = *package.files[file_index];
        for (size_t i = 0; i < file.nodes.size(); ++i)
          nodes.emplace(NodeKey{package.id.value, file_index, i}, NodeInfo{});
        Ref root{package.id, file_index, file.root};
        nodes.at(node_key(root)).reachable = true;
        std::vector<Ref> stack{root};
        while (!stack.empty()) {
          const auto parent = stack.back(); stack.pop_back();
          const auto depth = nodes.at(node_key(parent)).depth;
          const auto &children = file.node(parent.node).children;
          for (size_t i = 0; i < children.size(); ++i) {
            Ref child{package.id, file_index, children[i]};
            auto &info = nodes.at(node_key(child));
            if (info.reachable) continue;
            info.parent = parent; info.ordinal = i; info.depth = depth + 1; info.reachable = true;
            stack.push_back(child);
          }
        }
      }
    for (const auto &symbol : resolved->symbols)
      if (symbol.definition && !symbol.name.empty()) {
        auto found = nodes.find(node_key(*symbol.definition));
        if (found != nodes.end() && !found->second.named) found->second.named = symbol.id;
      }
    for (const auto &occurrence : resolved->occurrences) {
      const auto &symbol = resolved->symbols.at(occurrence.symbol.value);
      if (symbol.kind == modules::SymbolKind::module && symbol.target)
        module_uses.emplace(TokenKey{occurrence.source.module.value, occurrence.source.file,
            occurrence.span.begin_byte, occurrence.span.end_byte}, *symbol.target);
    }
    for (const auto &[source, symbol] : resolved->self_parameters) self.emplace(symbol, source);
    for (const auto &instance : value.instances) instances.emplace(instance.symbol, InstanceRef{&instance, {}});
    for (const auto &instance : value.instances)
      for (const auto &[original, replacement] : instance.renamed)
        instances.try_emplace(replacement, InstanceRef{&instance, original});
  }
  void current() const {
    if (module.resolved != resolved || module.resolved->symbols.size() != symbol_count ||
        module.instances.size() != instance_count || module.config != config)
      throw std::invalid_argument("identity: frontend changed after index construction");
  }
  std::string source(Ref reference) {
    return memo(sources, node_key(reference), [&] {
      const auto &info = nodes.at(node_key(reference));
      if (!info.reachable) throw std::invalid_argument("identity: detached AST node");
      const auto &package = resolved->project->module(reference.module);
      const auto &file = *package.files.at(reference.file);
      const auto &node = file.node(reference.node);
      if (!info.parent) return std::string{};
      if (node.kind == AstKind::Impl) {
        std::string header;
        for (size_t i = node.token_begin; i < node.token_end; ++i) {
          const auto &token = file.tokens.at(i);
          if (token.spelling == "{") break;
          modules::append_key_component(header, token.spelling);
          auto target = module_uses.find(TokenKey{reference.module.value, reference.file,
              token.span.begin_byte, token.span.end_byte});
          if (target != module_uses.end()) {
            modules::append_key_component(header, "@module");
            modules::append_key_component(header, resolved->project->module(target->second).identity);
          }
        }
        return modules::declaration_key(package.identity, "impl", "", header);
      }
      auto owner = source(*info.parent);
      if (info.named) {
        const auto &symbol = resolved->symbols.at(info.named->value);
        const bool file_local = symbol.kind == modules::SymbolKind::module ||
            (symbol.kind == modules::SymbolKind::constant && info.depth == 1 && symbol.name == "_");
        if (file_local)
          owner = modules::declaration_key(package.identity, "file", "",
              std::filesystem::path(file.source->path).filename().string());
        if (symbol.kind == modules::SymbolKind::value ||
            (symbol.kind == modules::SymbolKind::constant && (info.depth > 1 || symbol.name == "_")))
          owner = modules::declaration_key(package.identity, "local", owner, std::to_string(info.ordinal));
        return modules::declaration_key(package.identity,
            std::to_string(static_cast<unsigned>(symbol.kind)), owner, symbol.name);
      }
      return modules::declaration_key(package.identity, "child", owner, std::to_string(info.ordinal));
    });
  }
  std::string original(SymbolId id) {
    const auto &symbol = resolved->symbols.at(id.value);
    if (symbol.definition) return source(*symbol.definition);
    if (auto found = self.find(id); found != self.end())
      return modules::declaration_key(resolved->project->module(symbol.module).identity,
          "Self", source(found->second), "Self");
    if (symbol.kind == modules::SymbolKind::builtin_type || symbol.kind == modules::SymbolKind::builtin_function)
      return modules::declaration_key("prelude", "builtin", "", symbol.name);
    throw std::invalid_argument("identity: declaration has no stable source: " + symbol.name);
  }
  ModuleId owner(SymbolId id) {
    return memo(owners, id, [&] {
      auto found = instances.find(id);
      return found == instances.end() ? resolved->symbols.at(id.value).module : owner(found->second.instance->origin);
    });
  }
  std::string declaration(SymbolId id) {
    return memo(declarations, id, [&] {
      auto found = instances.find(id);
      if (found == instances.end()) return original(id);
      const auto &instance = *found->second.instance;
      const auto &package = resolved->project->module(owner(instance.origin));
      if (found->second.original)
        return modules::declaration_key(package.identity, "instance-member", declaration(instance.symbol), original(*found->second.original));
      std::string arguments;
      for (auto argument : instance.type_args) modules::append_key_component(arguments, type(argument));
      return modules::declaration_key(package.identity, "instance", original(instance.origin), arguments);
    });
  }
  std::string type(TypeId id) {
    return memo(types, id, [&] {
      const auto *value = module.types.find(id);
      if (!value) throw std::invalid_argument("identity: invalid TypeId");
      std::string key = "wz-type/1:";
      auto add = [&](std::string_view part) { modules::append_key_component(key, part); };
      add(std::to_string(static_cast<unsigned>(value->kind))); add(std::to_string(value->bits));
      switch (value->kind) {
      case TypeKind::NominalStruct: case TypeKind::NominalEnum: case TypeKind::TypeParameter: case TypeKind::TraitObject:
        add(declaration(value->declaration)); break;
      case TypeKind::ForeignOpaque: case TypeKind::ForeignCallable:
        add(resolved->project->foreign_binding(value->binding).stable_identity); break;
      default: break;
      }
      if (value->c_type) {
        const auto &decl = resolved->project->c_types.at(value->c_type - 1);
        const auto &bundle = resolved->project->c_bindings.at(decl.bundle);
        add(bundle.contract); add(bundle.sha256); add(decl.module); add(decl.identity);
      }
      add(std::to_string(value->parameters.size()));
      for (const auto &parameter : value->parameters) {
        add(std::to_string(static_cast<unsigned>(parameter.mode))); add(type(parameter.type));
      }
      add(value->kind == TypeKind::Function ? type(value->result) : "");
      add(value->throws_type ? type(*value->throws_type) : "");
      add(std::to_string(value->type_args.size()));
      for (auto argument : value->type_args) add(type(argument));
      return key;
    });
  }
};
IdentityKeys::IdentityKeys(const HirModule &module) : impl_(std::make_unique<Impl>(module)) {}
IdentityKeys::IdentityKeys(const VerifiedHirModule &module) : IdentityKeys(module.module()) { impl_->keep = module; }
IdentityKeys::~IdentityKeys() = default;
bool IdentityKeys::matches(const HirModule &module) const {
  return impl_->keep && &impl_->module == &module && module.resolved == impl_->resolved &&
      module.resolved->symbols.size() == impl_->symbol_count && module.instances.size() == impl_->instance_count;
}
std::vector<size_t> IdentityKeys::source_path(modules::NodeRef ref) const {
  std::lock_guard lock(impl_->mutex); impl_->current(); std::vector<size_t> result;
  auto current = ref;
  for (;;) {
    const auto &node = impl_->nodes.at(node_key(current));
    if (!node.reachable) throw std::invalid_argument("identity: detached AST node");
    if (!node.parent) break;
    result.push_back(node.ordinal); current = *node.parent;
  }
  std::reverse(result.begin(), result.end()); return result;
}
std::string IdentityKeys::declaration_key(SymbolId id) const {
  std::lock_guard lock(impl_->mutex); impl_->current(); return impl_->declaration(id);
}
std::string IdentityKeys::type_key(TypeId id) const {
  std::lock_guard lock(impl_->mutex); impl_->current(); return impl_->type(id);
}
std::string IdentityKeys::function_name(SymbolId id) const {
  std::lock_guard lock(impl_->mutex); impl_->current();
  return memo(impl_->names, id, [&] { return "lang.fn." + sha256(impl_->declaration(id)); });
}
ModuleId IdentityKeys::function_owner(SymbolId id) const {
  std::lock_guard lock(impl_->mutex); impl_->current(); return impl_->owner(id);
}
std::string declaration_key(const HirModule &module, SymbolId id) { return IdentityKeys(module).declaration_key(id); }
std::string type_key(const HirModule &module, TypeId id) { return IdentityKeys(module).type_key(id); }
std::string native_function_name(const HirModule &module, SymbolId id) { return IdentityKeys(module).function_name(id); }
ModuleId function_owner(const HirModule &module, SymbolId id) { return IdentityKeys(module).function_owner(id); }
}
