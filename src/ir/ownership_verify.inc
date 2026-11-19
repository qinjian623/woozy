  // Dataflow checks the flags that LLVM actually reads and writes. It does not
  // infer ownership from marker counts. A removed take or drop leaves a live
  // slot at a return, a backedge, or the next initialization of that slot.
  void ownership(const Function &f) const {
    const auto &h = m.hir.module();
    std::vector<unsigned> entry(f.slots.size(), 1); // bit 1 dead, bit 2 live
    const auto &sig = *types.find(f.signature);
    auto capture_slot = [&](const Slot &slot) {
      if (!slot.binding) return false;
      if (const auto *hf = hir_function_for(f.symbol))
        for (const auto &capture : hf->captures)
          if (capture.local == *slot.binding)
            return true;
      return false;
    };
    for (size_t n = 0; n < f.slots.size(); ++n) {
      const auto &s = f.slots[n];
      bool own = hir::classify_value(h, s.type).needs_drop;
      if (capture_slot(s)) {
        need(!s.owner && !s.parameter && !s.inout && !s.owning_parameter,
             "capture slot cannot own or be an incoming parameter", f.source);
        continue;
      }
      need(own == s.owner.has_value(), "owning slot missing identity/live flag", f.source);
      if (s.parameter) {
        need(s.owning_parameter == (own && sig.parameters[*s.parameter].mode == hir::ParamMode::move),
             "parameter ownership differs from signature", f.source);
      } else need(!s.owning_parameter, "local has incoming owner", f.source);
      if (s.owning_parameter) entry[n] = 2;
      if (!s.owner) continue;
      auto ref = *s.owner;
      bool match = false;
      if (ref.kind == hir::OwnerKind::binding) {
        if (auto it = locals_by_symbol.find(ref.identity);
            it != locals_by_symbol.end())
          for (const auto *l : it->second)
            if (l->type == s.type &&
                h.scopes.at(l->scope.value).function == f.symbol)
              match = true;
      } else if (ref.kind == hir::OwnerKind::temporary) {
        if (auto it = temporaries_by_producer.find(ref.identity);
            it != temporaries_by_producer.end())
          for (const auto *t : it->second)
            if (t->type == s.type &&
                h.scopes.at(h.nodes[t->producer.value].scope.value).function ==
                    f.symbol)
              match = true;
      } else if (ref.kind == hir::OwnerKind::iteration && ref.identity < h.nodes.size()) {
        const auto &n = h.nodes[ref.identity];
        match = n.kind() == hir::NodeKind::for_in && h.nodes[n.children[0].value].type == s.type;
      }
      need(match, "owning slot identity/type differs from frozen HIR", f.source);
      if (!s.parameter) {
        bool initialized = false;
        for (const auto &b : f.blocks) for (const auto &i : b.instructions)
          if (i.op == Op::store && i.slot.value == n) initialized = true;
        need(initialized, "owning slot never actually initialized", f.source);
      }
    }
    std::map<ValueId, const Instruction *> definitions;
    std::map<ValueId, const BlockParameter *> parameters;
    for (const auto &b : f.blocks) {
      for (const auto &p : b.parameters) parameters.emplace(p.value, &p);
      for (const auto &i : b.instructions) if (i.result) definitions.emplace(*i.result, &i);
    }
    std::function<std::optional<LocalSlotId>(ValueId)> root = [&](ValueId v) -> std::optional<LocalSlotId> {
      auto it = definitions.find(v); if (it == definitions.end()) return {};
      const auto &i = *it->second;
      if (i.op == Op::load || i.op == Op::address || i.op == Op::move) return i.slot;
      if ((i.op == Op::field_read || i.op == Op::field_address ||
           i.op == Op::borrow || i.op == Op::copy ||
           i.op == Op::vec_element_address) && !i.inputs.empty())
        return root(i.inputs[0]);
      if (i.op == Op::vec_read && !i.inputs.empty()) {
        const auto kind = types.find(i.type)->kind;
        if (kind == hir::TypeKind::Vec || kind == hir::TypeKind::Slice)
          return root(i.inputs[0]);
      }
      return {};
    };
    // A move argument is transferred at actual callee entry. A take moved
    // before a later throwing argument must not silence that failure prefix's
    // live flag. Each take therefore needs its real consuming use in the same
    // native block, with the actual saved value, not just matching metadata.
    for (const auto &b : f.blocks) for (size_t p = 0; p < b.instructions.size(); ++p) {
      const auto &take = b.instructions[p]; if (take.op != Op::take) continue;
      std::set<ValueId> saved;
      for (const auto &bb : f.blocks) for (const auto &store : bb.instructions)
        if (store.op == Op::store && store.slot == take.slot) saved.insert(store.inputs[0]);
      bool consumed = false;
      auto move_call = [&](const auto &call) {
        const auto &sig = *types.find(call.signature);
        for (size_t a = 0; a < sig.parameters.size(); ++a)
          if (sig.parameters[a].mode == hir::ParamMode::move && saved.contains(call.inputs[a+1])) return true;
        return false;
      };
      for (size_t n = p + 1; n < b.instructions.size(); ++n) {
        const auto &i = b.instructions[n];
        if (i.op == Op::store && i.slot != take.slot && f.slots[i.slot.value].owner && saved.contains(i.inputs[0])) consumed = true;
        if (i.op == Op::struct_init || i.op == Op::enum_init ||
            i.op == Op::vec_init || i.op == Op::function)
          for (auto v : i.inputs) if (saved.contains(v)) consumed = true;
        if (i.op == Op::vec_set && saved.contains(i.inputs[2])) consumed = true;
        if (i.op == Op::replace_indirect && saved.contains(i.inputs[1])) consumed = true;
        if (i.op == Op::call && move_call(i)) consumed = true;
        if (i.op == Op::exit_begin && !i.inputs.empty() && saved.contains(i.inputs[0])) consumed = true;
      }
      if (b.terminator.kind == Exit::try_call && move_call(b.terminator)) consumed = true;
      if ((b.terminator.kind == Exit::return_ || b.terminator.kind == Exit::error_return) &&
          saved.contains(b.terminator.inputs[0])) consumed = true;
      need(consumed, "actual take is not followed by its consuming operation/callee entry", take.source);
    }
    for (const auto &b : f.blocks) {
      const std::vector<hir::CleanupStep> *batch = nullptr;
      size_t next = 0; std::optional<HirNodeId> origin; uint64_t kind = 0;
      for (const auto &i : b.instructions) {
        if (i.op == Op::begin_borrow) {
          if (i.owner->kind == hir::OwnerKind::binding)
            need(root(i.inputs[0]) == i.slot, "borrow does not use actual binding storage", i.source);
          else {
            bool stored = false;
            for (const auto &block : f.blocks) for (const auto &store : block.instructions)
              if (store.op == Op::store && store.slot == i.slot && store.inputs == i.inputs) stored = true;
            need(stored, "borrow does not use actual owned temporary", i.source);
          }
        }
        if (i.op == Op::ownership_begin) {
          need(!batch, "nested ownership batch", i.source); origin = i.origin; kind = i.bits;
          if (!kind) {
            if (auto it = full_expressions.find(origin->value);
                it != full_expressions.end())
              batch = &it->second->actions;
          } else {
            if (auto it = calls.find(origin->value); it != calls.end())
              batch = &it->second->on_return;
          }
          need(batch != nullptr, "batch not present in HIR", i.source); next = 0;
        } else if (i.op == Op::ownership_end) {
          need(batch && next == batch->size() && i.origin == origin && i.bits == kind,
               "actual full-expression/call cleanup incomplete", i.source); batch = nullptr;
        } else if (batch) {
          need(next < batch->size(), "extra full-expression action", i.source);
          const auto &a = (*batch)[next++];
          need(i.owner == a.owner &&
               ((i.op == Op::destroy && a.kind == hir::CleanupKind::destroy_if_live &&
                 f.slots.at(i.slot.value).type == a.type) ||
                (i.op == Op::end_borrow && a.kind == hir::CleanupKind::end_borrow)),
               "actual full-expression/call owning operation differs from HIR", i.source);
        }
        if (i.op != Op::store || !f.slots.at(i.slot.value).owner ||
            f.slots.at(i.slot.value).owner->kind != hir::OwnerKind::temporary) continue;
        const auto &n = h.nodes.at(f.slots[i.slot.value].owner->identity);
        const Instruction *producer = definitions.count(i.inputs[0]) ? definitions.at(i.inputs[0]) : nullptr;
        if (n.kind() == hir::NodeKind::read) {
          need(producer && producer->op == Op::copy && producer->inputs.size() == 1,
               "owned Read lacks actual logical copy", i.source);
          const auto *load = definitions.at(producer->inputs[0]);
          const auto &load_slot = f.slots.at(load->slot.value);
          const auto symbol = std::get<hir::Read>(n.data).symbol;
          need(load->op == Op::load &&
                   (load_slot.owner ==
                        hir::OwnerRef{hir::OwnerKind::binding, symbol.value} ||
                    (capture_slot(load_slot) && load_slot.binding == symbol)),
               "owned Read copies wrong source", i.source);
        } else if (n.kind() == hir::NodeKind::field_read) {
          need(producer && producer->op == Op::copy && producer->inputs.size() == 1 &&
               definitions.at(producer->inputs[0])->op == Op::field_read,
               "owned field projection lacks actual logical copy", i.source);
        } else if (n.kind() == hir::NodeKind::move && std::get<hir::Move>(n.data).binding) {
          need(producer && producer->op == Op::move && f.slots.at(producer->slot.value).owner ==
               hir::OwnerRef{hir::OwnerKind::binding, std::get<hir::Move>(n.data).binding->value},
               "Move replaced by copy/borrow", i.source);
        } else if (n.kind() == hir::NodeKind::call && !producer) {
          need(parameters.count(i.inputs[0]) && parameters.at(i.inputs[0])->call.value ==
               f.slots[i.slot.value].owner->identity && !parameters.at(i.inputs[0])->error,
               "owning call result from inactive/wrong channel", i.source);
        }
      }
      need(!batch, "ownership batch crosses block", b.terminator.source);
    }
    // Slot flags do not prove that two slots received distinct logical copies.
    // Track the unbound owned value between creation, store and take as well.
    using Values = std::map<ValueId, unsigned>; // bit 1 transferred, bit 2 available
    Values empty_values;
    std::map<LocalSlotId, ValueId> temporary_values;
    auto inert_function = [&](const Instruction &i) {
      if (i.op != Op::function)
        return false;
      // No-origin function instructions are direct-call metadata or constant
      // noncapturing values, never a captured environment temporary.
      if (!i.origin || i.origin->value >= h.nodes.size())
        return true;
      const auto *v = std::get_if<hir::FunctionValue>(
          &h.nodes[i.origin->value].data);
      return v && v->captures.empty() && !v->bound;
    };
    auto owned_value = [&](ValueId value) {
      auto definition = definitions.find(value);
      if (definition != definitions.end() &&
          (definition->second->op == Op::string_literal ||
           inert_function(*definition->second)))
        return false; // Static bytes / null-environment Fn have no owner.
      auto type = definition != definitions.end() ? definition->second->type
                                                 : parameters.at(value)->type;
      return hir::classify_value(h, type).needs_drop;
    };
    auto produces_owner = [](Op op) {
      return op == Op::copy || op == Op::move || op == Op::struct_init ||
             op == Op::enum_init || op == Op::vec_init || op == Op::vec_read ||
             op == Op::vec_slice || op == Op::iteration_read ||
             op == Op::call || op == Op::system_call ||
             op == Op::foreign_call || op == Op::binary ||
             op == Op::function || op == Op::trait_object ||
             op == Op::trait_call;
    };
    for (const auto &block : f.blocks) {
      for (const auto &p : block.parameters)
        if (owned_value(p.value)) empty_values.emplace(p.value, 1);
      for (const auto &i : block.instructions) {
        if (i.result &&
            produces_owner(i.op) &&
            !inert_function(i) && owned_value(*i.result))
          empty_values.emplace(*i.result, 1);
        if (i.op == Op::store && f.slots[i.slot.value].owner &&
            f.slots[i.slot.value].owner->kind == hir::OwnerKind::temporary)
          need(temporary_values.emplace(i.slot, i.inputs[0]).second,
               "owning temporary has multiple initializing instructions", i.source);
      }
    }
    // Every use reached through a vec_element_address must still have an
    // owning handle for that container. Trace the real SSA field chain to the
    // indexed address, then map its receiver value to the owning slot(s) that
    // actually stored that value. Liveness is checked path-sensitively below.
    std::map<ValueId, ValueId> element_address_roots;
    auto address_root = [&](ValueId value) -> std::optional<ValueId> {
      std::set<ValueId> visited;
      for (;;) {
        need(visited.insert(value).second,
             "Vec element address chain cycle", f.source);
        auto found = definitions.find(value);
        if (found == definitions.end())
          return {};
        const auto &definition = *found->second;
        if (definition.op == Op::vec_element_address)
          return value;
        if (definition.op != Op::field_address ||
            definition.inputs.size() != 1)
          return {};
        value = definition.inputs[0];
      }
    };
    for (const auto &[value, definition] : definitions)
      if (definition->op == Op::vec_element_address ||
          definition->op == Op::field_address)
        if (auto root = address_root(value))
          element_address_roots.emplace(value, *root);
    std::map<ValueId, std::set<LocalSlotId>> stored_owners;
    for (const auto &block : f.blocks)
      for (const auto &i : block.instructions)
        if (i.op == Op::store && i.inputs.size() == 1 &&
            i.slot.value < f.slots.size() && f.slots[i.slot.value].owner)
          stored_owners[i.inputs[0]].insert(i.slot);
    auto receiver_owners = [&](ValueId value) -> std::set<LocalSlotId> {
      std::set<ValueId> visited;
      for (;;) {
        need(visited.insert(value).second,
             "Vec element receiver ownership cycle", f.source);
        auto stored = stored_owners.find(value);
        if (stored != stored_owners.end() && !stored->second.empty())
          return stored->second;
        auto definition = definitions.find(value);
        if (definition == definitions.end())
          return {};
        const auto &i = *definition->second;
        if ((i.op == Op::load || i.op == Op::move) &&
            i.slot.value < f.slots.size() && f.slots[i.slot.value].owner)
          return {i.slot};
        if ((i.op != Op::copy && i.op != Op::borrow) || i.inputs.empty())
          return {};
        value = i.inputs[0];
      }
    };
    using Borrows = std::vector<LocalSlotId>;
    std::vector<std::optional<std::set<LocalSlotId>>> iterating(f.blocks.size()); iterating[0] = std::set<LocalSlotId>{};
    std::vector<std::optional<Borrows>> borrowed(f.blocks.size()); borrowed[0] = Borrows{};
    std::vector<std::optional<std::vector<unsigned>>> incoming(f.blocks.size());
    std::vector<std::optional<Values>> incoming_values(f.blocks.size());
    incoming[0] = entry;
    incoming_values[0] = empty_values;
    std::vector<size_t> work{0};
    while (!work.empty()) {
      auto b = work.back(); work.pop_back(); auto flags = *incoming[b]; auto borrows = *borrowed[b]; auto iterations = *iterating[b];
      auto values = *incoming_values[b];
      auto consume = [&](ValueId value, SourceSpan source) {
        if (!owned_value(value)) return;
        need(values.contains(value) && values.at(value) == 2,
             "owned value reused or transferred without a logical copy/take", source);
        values.at(value) = 1;
      };
      auto call_inputs = [&](const auto &call) {
        const auto &signature = *types.find(call.signature);
        for (size_t n = 0; n < signature.parameters.size(); ++n)
          if (signature.parameters[n].mode == hir::ParamMode::move)
            consume(call.inputs[n + 1], call.source);
      };
      for (const auto &p : f.blocks[b].parameters) {
        if (!owned_value(p.value)) continue;
        need(values.at(p.value) == 1, "call overwrites an untransferred owned result", f.source);
        values.at(p.value) = 2;
      }
      for (const auto &i : f.blocks[b].instructions) {
        auto require_element_owner = [&](ValueId address, SourceSpan source) {
          auto found = element_address_roots.find(address);
          if (found == element_address_roots.end())
            return;
          const auto &root_instruction = *definitions.at(found->second);
          need(!root_instruction.inputs.empty(),
               "Vec element address has no receiver", source);
          auto owners = receiver_owners(root_instruction.inputs[0]);
          need(!owners.empty(),
               "Vec element address receiver has no tracked owning storage",
               source);
          bool live = false;
          for (auto slot : owners)
            live |= slot.value < flags.size() && flags[slot.value] == 2;
          need(live, "Vec element address owner is not live at field use",
               source);
        };
        if (i.op == Op::vec_element_address && i.result)
          require_element_owner(*i.result, i.source);
        if (i.op == Op::load_indirect || i.op == Op::store_indirect ||
            i.op == Op::replace_indirect)
          for (auto value : i.inputs)
            require_element_owner(value, i.source);
        if (i.op == Op::store && f.slots[i.slot.value].owner)
          consume(i.inputs[0], i.source);
        if (i.op == Op::struct_init || i.op == Op::enum_init ||
            i.op == Op::vec_init || i.op == Op::function)
          for (auto value : i.inputs) consume(value, i.source);
        if (i.op == Op::vec_set) consume(i.inputs[2], i.source);
        if (i.op == Op::replace_indirect) consume(i.inputs[1], i.source);
        if (i.op == Op::call) call_inputs(i);
        if (i.op == Op::take) {
          auto value = temporary_values.at(i.slot);
          if (owned_value(value)) {
            need(values.contains(value) && values.at(value) == 1,
                 "temporary take duplicates an available owned value", i.source);
            values.at(value) = 2;
          }
        }
        if (i.result && produces_owner(i.op) &&
            !inert_function(i) && owned_value(*i.result)) {
          need(values.at(*i.result) == 1, "owned result overwritten before transfer", i.source);
          values.at(*i.result) = 2;
        }
        if (i.op == Op::iteration_begin)
          need(iterations.insert(i.slot).second, "duplicate actual iteration registration", i.source);
        if (i.op == Op::iteration_end)
          need(iterations.erase(i.slot) == 1, "end_iteration without actual registration", i.source);
        if (i.op == Op::iteration_test || i.op == Op::iteration_next || i.op == Op::iteration_read)
          need(iterations.contains(i.slot), "iteration operation without actual registration", i.source);
        if (i.op == Op::destroy)
          need(!iterations.contains(i.slot), "destroy before actual end_iteration", i.source);
        if (i.op == Op::load && f.slots.at(i.slot.value).owner &&
            (!f.slots.at(i.slot.value).parameter || f.slots.at(i.slot.value).owning_parameter))
          need(flags[i.slot.value] == 2, "read of inactive owning binding", i.source);
        if (i.op == Op::begin_borrow) {
          borrows.push_back(i.slot);
        } else if (i.op == Op::end_borrow) {
          need(!borrows.empty(), "end_borrow without actual active borrow", i.source);
          const auto &slot = f.slots[borrows.back().value];
          need((i.owner->kind == hir::OwnerKind::binding && slot.binding == SymbolId{i.owner->identity}) ||
               slot.owner == i.owner, "end_borrow ends different actual storage", i.source);
          borrows.pop_back();
        }
        if (i.op == Op::move || i.op == Op::destroy)
          need(std::find(borrows.begin(), borrows.end(), i.slot) == borrows.end(),
               "move/destroy while actual borrow remains active", i.source);
        if (i.op == Op::store && f.slots.at(i.slot.value).owner) {
          need(flags[i.slot.value] == 1, "owning store overwrites a live value", i.source);
          flags[i.slot.value] = 2;
        } else if (i.op == Op::move && f.slots.at(i.slot.value).owner) {
          need(flags[i.slot.value] == 2, "move of inactive owning slot", i.source);
          flags[i.slot.value] = 1;
        } else if (i.op == Op::take) {
          need(flags.at(i.slot.value) == 2, "take of inactive owning temporary", i.source);
          need(f.slots.at(i.slot.value).owner->kind == hir::OwnerKind::temporary,
               "take must transfer temporary storage", i.source);
          flags[i.slot.value] = 1;
        } else if (i.op == Op::destroy) {
          need(!f.slots.at(i.slot.value).parameter || f.slots.at(i.slot.value).owning_parameter,
               "destroy of ordinary borrowed parameter", i.source);
          flags[i.slot.value] = 1;
        }
      }
      const auto &term = f.blocks[b].terminator;
      if (term.kind == Exit::try_call) call_inputs(term);
      if (term.kind == Exit::return_ || term.kind == Exit::error_return) {
        consume(term.inputs[0], term.source);
        need(borrows.empty() && iterations.empty(), "function leaves active borrow/iteration", term.source);
        for (size_t n = 0; n < flags.size(); ++n)
          need(!f.slots[n].owner || flags[n] == 1,
               "function leaves a live owning slot (missing actual take/destroy)", term.source);
        for (auto [value, state] : values)
          need(state == 1, "function leaves an untransferred owned result", term.source);
      }
      for (auto dest : term.targets) {
        if (term.kind == Exit::conditional) {
          auto condition = definitions.find(term.inputs[0]);
          if (condition != definitions.end() && condition->second->op == Op::constant &&
              types.find(condition->second->type)->kind == hir::TypeKind::Bool &&
              dest != term.targets[condition->second->bits ? 0 : 1])
            continue;
        }
        if (!incoming[dest.value]) { incoming[dest.value] = flags; incoming_values[dest.value] = values; borrowed[dest.value] = borrows; iterating[dest.value] = iterations; work.push_back(dest.value); }
        else {
          need(*borrowed[dest.value] == borrows && *iterating[dest.value] == iterations,
               "actual borrow/iteration state differs at CFG join", term.source);
          auto next = *incoming[dest.value];
          auto next_values = *incoming_values[dest.value];
          for (size_t n = 0; n < flags.size(); ++n) next[n] |= flags[n];
          for (auto [value, state] : values) next_values.at(value) |= state;
          if (next != *incoming[dest.value] || next_values != *incoming_values[dest.value]) {
            incoming[dest.value] = std::move(next);
            incoming_values[dest.value] = std::move(next_values);
            work.push_back(dest.value);
          }
        }
      }
    }
  }
