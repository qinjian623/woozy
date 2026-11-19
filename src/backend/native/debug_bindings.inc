  // Pattern, iteration and catch bindings do not have hir::Binding nodes.
  // Their HIR records name the exact source scope in concrete instances too.
  std::map<SymbolId, ScopeId> extra_binding_scopes;

  void index_bindings() {
    for (const auto &n : hir.nodes) {
      if (const auto *b = std::get_if<hir::Binding>(&n.data)) {
        function_bindings[hir.scopes.at(n.scope.value).function]
            .emplace(b->symbol, &n);
        declarations[n.scope].push_back(&n);
      } else if (const auto *loop = std::get_if<hir::ForIn>(&n.data)) {
        extra_binding_scopes.emplace(loop->element, loop->body_scope);
      } else if (const auto *sw = std::get_if<hir::Switch>(&n.data)) {
        auto pattern = [&](const auto &self, const hir::Pattern &p,
                           ScopeId scope) -> void {
          if (p.binding) extra_binding_scopes.emplace(*p.binding, scope);
          for (const auto &child : p.payload) self(self, child, scope);
        };
        for (const auto &arm : sw->arms) pattern(pattern, arm.pattern, arm.scope);
      } else if (const auto *attempt = std::get_if<hir::Try>(&n.data)) {
        for (const auto &handler : attempt->catches)
          extra_binding_scopes.emplace(handler.binding, handler.scope);
      }
    }
    for (auto &[id, nodes] : declarations)
      std::stable_sort(nodes.begin(), nodes.end(), [](const auto *a, const auto *b) {
        return a->source.span.end_byte < b->source.span.end_byte;
      });
  }

  const modules::Symbol *capture_symbol(SymbolId local) const {
    if (function)
      for (const auto &capture : function->captures)
        if (capture.local == local) return symbol(capture.origin);
    return nullptr;
  }
