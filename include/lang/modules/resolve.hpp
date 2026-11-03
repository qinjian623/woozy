#pragma once
#include "lang/modules/project.hpp"

namespace lang::modules {
enum class SymbolKind {
  value, constant, function, type, trait, type_parameter, module,
  field, variant, builtin_type, builtin_function
};
struct NodeRef {
  ModuleId module;
  size_t file;
  AstNodeId node;
};
struct Symbol {
  SymbolId id;
  std::string name;
  SymbolKind kind;
  ModuleId module;
  std::optional<NodeRef> definition;
  std::optional<SourceSpan> declaration;
  bool exported = false;
  std::optional<ModuleId> target;
};
struct Occurrence {
  NodeRef source;
  SymbolId symbol;
  SourceSpan span;
};
struct ResolvedProject {
  std::shared_ptr<const Project> project;
  std::vector<Symbol> symbols;
  std::vector<Occurrence> occurrences;
  // Instance/type members require T04's types; no symbol is guessed here.
  std::vector<NodeRef> deferred_members;
};
PhaseResult<ResolvedProject> resolve_project(std::shared_ptr<const Project>,
                                           std::stop_token = {});
} // namespace lang::modules
