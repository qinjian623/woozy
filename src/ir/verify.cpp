#include "lang/ir/native.hpp"
#include "../hir/foreign.hpp"
#include "layout.hpp"
#include "obligations.hpp"
#include <algorithm>
#include <functional>
#include <set>
#include <sstream>

namespace lang::ir {
namespace {
using Set = std::set<size_t>;
template <class T>
std::set<T> intersect(const std::set<T> &a, const std::set<T> &b) {
  std::set<T> r;
  std::set_intersection(a.begin(), a.end(), b.begin(), b.end(),
                        std::inserter(r, r.end()));
  return r;
}
bool integer(hir::TypeKind k) {
  return k == hir::TypeKind::SignedInt || k == hir::TypeKind::UnsignedInt ||
         k == hir::TypeKind::Isize || k == hir::TypeKind::Usize;
}
bool same_span(SourceSpan a, SourceSpan b) {
  return a.snapshot == b.snapshot && a.begin_byte == b.begin_byte &&
         a.end_byte == b.end_byte;
}
struct Bad {
  std::string message;
  SourceSpan span;
};
class Verifier {
  const Module &m;
  const hir::TypeContext &types;
  std::map<SymbolId, TypeId> functions;
  void need(bool ok, std::string message, SourceSpan s) const {
    if (!ok)
      throw Bad{std::move(message), s};
  }
  const hir::Type &type(TypeId id, SourceSpan s) const {
    auto t = types.find(id);
    need(t != nullptr, "invalid TypeId", s);
    return *t;
  }
  const hir::HirFunction *hir_function_for(SymbolId symbol) const {
    for (const auto &f : m.hir.module().functions)
      if (f.symbol == symbol)
        return &f;
    return nullptr;
  }
  void function(const Function &f) const {
    detail::AggregateTypes aggregates(m.hir.module());
    const auto s = f.source;
    const auto &sig = type(f.signature, s);
    need(functions.contains(f.symbol) && functions.at(f.symbol) == f.signature,
         "function identity/signature differs from verified HIR", s);
    need(!f.blocks.empty(), "function has no entry block", s);
    std::set<uint32_t> parameters;
    for (auto &slot : f.slots) {
      type(slot.type, s);
      if (slot.parameter) {
        auto p = *slot.parameter;
        need(p < sig.parameters.size() && parameters.insert(p).second,
             "invalid/duplicate parameter slot", s);
        need(slot.type == sig.parameters[p].type &&
                 slot.inout ==
                     (sig.parameters[p].mode == hir::ParamMode::inout),
             "parameter slot type/mode mismatch", s);
      } else {
        need(!slot.inout, "local slot cannot be an incoming pointer", s);
        if (slot.binding) {
          const auto *h = hir_function_for(f.symbol);
          bool capture = false;
          if (h)
            for (const auto &binding : h->captures)
              if (binding.local == *slot.binding) {
                capture = true;
                need(binding.type == slot.type && !slot.owner,
                     "capture local slot type/ownership", s);
              }
          (void)capture;
        }
      }
    }
    need(parameters.size() == sig.parameters.size(), "missing parameter slot",
         s);
    if (const auto *h = hir_function_for(f.symbol)) {
      for (const auto &capture : h->captures) {
        bool found = false;
        for (const auto &slot : f.slots)
          if (slot.binding == capture.local) {
            need(slot.type == capture.type && !slot.parameter && !slot.inout &&
                     !slot.owner,
                 "capture local slot identity", s);
            found = true;
          }
        need(found, "capture local has no entry slot", s);
      }
    }
    struct Def {
      size_t block, position;
      TypeId type;
      bool address;
      SourceSpan source;
    };
    std::map<ValueId, Def> defs;
    std::map<ValueId, const Instruction *> instructions;
    struct RequiredCheck {
      size_t block, position;
      HirNodeId origin;
      ValueId receiver, index;
      SourceSpan source;
    };
    std::vector<RequiredCheck> required_checks;
    using ConstantKey = std::tuple<uint64_t, uint64_t, uint64_t, uint64_t>;
    std::multimap<ConstantKey, size_t> constant_roots;
    for (size_t h = 0; h < m.hir.module().nodes.size(); ++h) {
      const auto &n = m.hir.module().nodes[h];
      if (n.kind() == hir::NodeKind::constant_literal &&
          m.hir.module().scopes.at(n.scope.value).function == f.symbol)
        constant_roots.emplace(
            ConstantKey{n.source.span.snapshot.value, n.source.span.begin_byte,
                        n.source.span.end_byte, n.type.value},
            h);
    }
    std::vector<Set> predecessors(f.blocks.size());
    for (size_t b = 0; b < f.blocks.size(); ++b) {
      const auto &bb = f.blocks[b];
      const auto &t = bb.terminator;
      bool shape = false;
      switch (t.kind) {
      case Exit::branch:
        shape = t.inputs.empty() && t.targets.size() == 1;
        break;
      case Exit::conditional:
        shape = t.inputs.size() == 1 && t.targets.size() == 2;
        break;
      case Exit::error_return:
      case Exit::return_:
        shape = t.inputs.size() == 1 && t.targets.empty();
        break;
      case Exit::unreachable:
        shape = t.inputs.empty() && t.targets.empty();
        break;
      case Exit::try_call:
        shape = !t.inputs.empty() && t.targets.size() == 2 &&
                t.targets[0] != t.targets[1];
        break;
      case Exit::await_:
        shape = true; // arity and distinctness checked with the Task type
        break;
      case Exit::cancel_return:
        shape = t.inputs.empty() && t.targets.empty();
        break;
      case Exit::missing:
        break;
      }
      need(shape, "missing or malformed terminator", t.source);
      for (auto dest : t.targets) {
        need(dest.value < f.blocks.size() && dest.value != 0,
             "invalid branch target or edge into entry", t.source);
        predecessors[dest.value].insert(b);
      }
      for (const auto &param : bb.parameters)
        need(defs.emplace(param.value,
                          Def{b, 0, param.type, false, f.source})
                 .second,
             "duplicate block parameter", t.source);
      for (size_t p = 0; p < bb.instructions.size(); ++p) {
        auto &i = bb.instructions[p];
        type(i.type, i.source);
        need(i.result.has_value() ==
                 (i.op != Op::store && i.op != Op::store_indirect &&
                  i.op != Op::exit_begin && i.op != Op::cleanup_begin &&
                  i.op != Op::cleanup_end && i.op != Op::take &&
                  i.op != Op::destroy && i.op != Op::end_borrow &&
                  i.op != Op::save_output && i.op != Op::vec_set &&
                  i.op != Op::iteration_end && i.op != Op::iteration_begin &&
                  i.op != Op::iteration_next && i.op != Op::ownership_begin &&
                  i.op != Op::ownership_end && i.op != Op::replace_indirect && i.op != Op::begin_borrow && i.op != Op::vec_check &&
                  i.op != Op::c_ptr_write && i.op != Op::c_buffer_write),
             "invalid instruction result", i.source);
        if (i.result)
          need(defs.emplace(*i.result, Def{b, p + 1, i.type,
                                           i.op == Op::address ||
                                               i.op == Op::field_address ||
                                               i.op == Op::vec_element_address,
                                           i.source})
                   .second,
               "duplicate value definition", i.source);
        if (i.result)
          instructions.emplace(*i.result, &i);
      }
    }
    // A block parameter is defined on its TryCall edge. Use that call's real
    // source span for the receiver/index location check rather than the
    // parameter block's outgoing terminator.
    for (size_t b = 0; b < f.blocks.size(); ++b)
      if (predecessors[b].size() == 1) {
        const auto predecessor = *predecessors[b].begin();
        for (const auto &param : f.blocks[b].parameters)
          defs[param.value].source = f.blocks[predecessor].terminator.source;
      }
    // A Vec element address is only an implementation detail of replacing a
    // field in the assignment that owns the indexed place. Track each address
    // through field_address nodes and reject any use that could store it, pass
    // it to a call, return it, or use it as an inout argument.
    using AddressInfo = std::pair<ValueId, size_t>; // root, field depth
    std::map<ValueId, AddressInfo> address_roots;
    auto resolve_address = [&](ValueId value) -> std::optional<AddressInfo> {
      std::set<ValueId> visited;
      size_t depth = 0;
      for (;;) {
        need(visited.insert(value).second,
             "Vec element address chain cycle", s);
        auto found = instructions.find(value);
        if (found == instructions.end())
          return {};
        const auto &definition = *found->second;
        if (definition.op == Op::vec_element_address)
          return AddressInfo{value, depth};
        if (definition.op != Op::field_address ||
            definition.inputs.size() != 1)
          return {};
        value = definition.inputs[0];
        ++depth;
      }
    };
    for (const auto &[value, definition] : instructions)
      if (definition->op == Op::vec_element_address ||
          definition->op == Op::field_address)
        if (auto root = resolve_address(value))
          address_roots.emplace(value, *root);
    std::map<ValueId, size_t> address_writes;
    std::set<ValueId> address_inout_roots;
    auto address_use = [&](const Instruction &consumer, ValueId value,
                           size_t operand, SourceSpan source) {
      auto found = address_roots.find(value);
      if (found == address_roots.end())
        return;
      if (consumer.op == Op::field_address ||
          consumer.op == Op::begin_borrow)
        return;
      if (consumer.op == Op::load_indirect ||
          consumer.op == Op::store_indirect ||
          consumer.op == Op::replace_indirect) {
        need(found->second.second > 0,
             "Vec element address used without a field replacement", source);
        if (consumer.op != Op::load_indirect)
          ++address_writes[found->second.first];
        return;
      }
      if (consumer.op == Op::call) {
        const auto &cs = type(consumer.signature, source);
        need(operand > 0 && operand - 1 < cs.parameters.size() &&
                 cs.parameters[operand - 1].mode == hir::ParamMode::inout,
             "Vec element address escapes through an ordinary call argument",
             source);
        address_inout_roots.insert(found->second.first);
        return;
      }
      need(false, "Vec element address escapes its field replacement or "
                  "inout argument",
           source);
    };
    for (const auto &bb : f.blocks) {
      for (const auto &i : bb.instructions)
        for (size_t operand = 0; operand < i.inputs.size(); ++operand)
          address_use(i, i.inputs[operand], operand, i.source);
      const auto &term = bb.terminator;
      for (size_t operand = 0; operand < term.inputs.size(); ++operand) {
        auto value = term.inputs[operand];
        auto found = address_roots.find(value);
        if (found == address_roots.end())
          continue;
        need(term.kind == Exit::try_call && operand > 0,
             "Vec element address escapes through a terminator",
             term.source);
        const auto &cs = type(term.signature, term.source);
        need(operand - 1 < cs.parameters.size() &&
                 cs.parameters[operand - 1].mode == hir::ParamMode::inout,
             "Vec element address escapes through an ordinary TryCall argument",
             term.source);
        address_inout_roots.insert(found->second.first);
      }
    }
    for (auto [root, info] : address_roots)
      if (info.second == 0)
        need(address_writes[root] == 1 ||
                 address_inout_roots.contains(root),
             "Vec element address does not perform exactly one field write "
             "or direct inout call",
             f.source);
    Set reachable;
    std::function<void(size_t)> visit = [&](size_t b) {
      if (!reachable.insert(b).second)
        return;
      for (auto d : f.blocks[b].terminator.targets)
        visit(d.value);
    };
    visit(0);
    need(reachable.size() == f.blocks.size(), "unreachable native block", s);
    std::vector<Set> dom(f.blocks.size(), reachable);
    dom[0] = {0};
    bool changed = true;
    while (changed) {
      changed = false;
      for (auto b : reachable)
        if (b != 0) {
          Set next = reachable;
          for (auto p : predecessors[b])
            if (reachable.contains(p))
              next = intersect(next, dom[p]);
          next.insert(b);
          if (next != dom[b]) {
            dom[b] = std::move(next);
            changed = true;
          }
        }
    }
    // A payload read requires the same SSA enum value's matching true edge on
    // every incoming path. Merely having a dominating tag comparison is not
    // enough.
    using Facts = std::set<std::tuple<ValueId, SymbolId, bool>>;
    Facts universe;
    for (auto [value, i] : instructions)
      if (i->op == Op::enum_test && i->inputs.size() == 1) {
        universe.emplace(i->inputs[0], i->variant, true);
        universe.emplace(i->inputs[0], i->variant, false);
      }
    std::vector<Facts> facts(f.blocks.size(), universe);
    facts[0].clear();
    changed = true;
    while (changed) {
      changed = false;
      for (auto b : reachable) {
        if (b == 0)
          continue;
        auto next = universe;
        for (auto p : predecessors[b]) {
          auto edge = facts[p];
          const auto &term = f.blocks[p].terminator;
          if (term.kind == Exit::conditional &&
              term.targets[0] != term.targets[1]) {
            auto found = instructions.find(term.inputs[0]);
            if (found != instructions.end()) {
              const auto &test = *found->second;
              if (test.op == Op::enum_test && test.inputs.size() == 1)
                edge.emplace(test.inputs[0], test.variant,
                             term.targets[0].value == b);
            }
          }
          next = intersect(next, edge);
        }
        if (next != facts[b]) {
          facts[b] = std::move(next);
          changed = true;
        }
      }
    }
    auto use = [&](ValueId id, size_t b, size_t pos,
                   SourceSpan src) -> const Def & {
      auto it = defs.find(id);
      need(it != defs.end(), "undefined value", src);
      const auto &d = it->second;
      need(d.block == b ? d.position < pos
                        : reachable.contains(b) && dom[b].contains(d.block),
           "value definition does not dominate use", src);
      return d;
    };
    Set all_slots, incoming;
    const auto *h = hir_function_for(f.symbol);
    auto capture_slot = [&](size_t index) {
      if (!h || index >= f.slots.size() || !f.slots[index].binding)
        return false;
      for (const auto &capture : h->captures)
        if (capture.local == *f.slots[index].binding)
          return true;
      return false;
    };
    for (size_t i = 0; i < f.slots.size(); ++i) {
      all_slots.insert(i);
      if (f.slots[i].parameter || capture_slot(i))
        incoming.insert(i);
    }
    std::vector<Set> in(f.blocks.size(), all_slots),
        out(f.blocks.size(), all_slots);
    in[0] = incoming;
    changed = true;
    while (changed) {
      changed = false;
      for (auto b : reachable) {
        Set next = b == 0 ? incoming : all_slots;
        if (b != 0)
          for (auto p : predecessors[b])
            if (reachable.contains(p))
              next = intersect(next, out[p]);
        auto written = next;
        for (const auto &i : f.blocks[b].instructions)
          if (i.op == Op::store)
            written.insert(i.slot.value);
        if (next != in[b] || written != out[b]) {
          in[b] = std::move(next);
          out[b] = std::move(written);
          changed = true;
        }
      }
    }
    // Fixed operations are effects: individually valid calls may not be
    // reordered. HIR children retain source evaluation order; use postorder
    // so nested arguments precede their enclosing operation.
    std::map<HirNodeId, size_t> system_order;
    std::vector<SourceSpan> system_loop_sources;
    auto hir_function = std::find_if(
        m.hir.module().functions.begin(), m.hir.module().functions.end(),
        [&](const auto &candidate) { return candidate.symbol == f.symbol; });
    need(hir_function != m.hir.module().functions.end(),
         "native function has no HIR body", s);
    std::vector<std::pair<HirNodeId, bool>> pending{{hir_function->body, false}};
    size_t sequence = 0;
    while (!pending.empty()) {
      auto [id, visited] = pending.back();
      pending.pop_back();
      const auto &node = m.hir.module().nodes.at(id.value);
      if (!visited) {
        pending.push_back({id, true});
        if (node.kind() == hir::NodeKind::for_) {
          system_loop_sources.push_back(node.source.span);
          // HIR stores step before body; execution performs body first.
          for (auto child : {node.children.at(2), node.children.at(3),
                             node.children.at(1), node.children.at(0)})
            pending.push_back({child, false});
        } else {
          if (node.kind() == hir::NodeKind::for_in)
            system_loop_sources.push_back(node.source.span);
          for (auto it = node.children.rbegin(); it != node.children.rend(); ++it)
            pending.push_back({*it, false});
        }
      } else if (const auto *call = std::get_if<hir::Call>(&node.data);
                 call && (call->kind == hir::CalleeKind::system ||
                          call->kind == hir::CalleeKind::c_foreign)) {
        system_order.emplace(id, sequence++);
      }
    }
    // Carry the order through ordinary CFG edges. A dominating loop header
    // starts another iteration, and defer markers start/end a cleanup context.
    // Splitting a block must not make reordered effects acceptable.
    auto backedge = [&](size_t from, size_t to) {
      if (!dom[from].contains(to)) return false;
      const auto source = f.blocks[from].terminator.source;
      need(std::any_of(system_loop_sources.begin(), system_loop_sources.end(),
                       [&](SourceSpan loop) { return same_span(loop, source); }),
           "system operation loop edge has no HIR loop", source);
      return true;
    };
    std::vector<std::optional<size_t>> system_in(f.blocks.size()),
        system_out(f.blocks.size());
    if (!system_order.empty()) {
      changed = true;
      while (changed) {
        changed = false;
        for (auto b : reachable) {
          std::optional<size_t> previous;
          for (auto pred : predecessors[b])
            if (!backedge(pred, b) && system_out[pred] &&
                (!previous || *system_out[pred] > *previous))
              previous = system_out[pred];
          if (system_in[b] != previous) {
            system_in[b] = previous;
            changed = true;
          }
          for (const auto &instruction : f.blocks[b].instructions) {
            if (instruction.op == Op::cleanup_begin || instruction.op == Op::cleanup_end)
              previous.reset();
            if (instruction.op == Op::system_call && instruction.origin) {
              auto position = system_order.find(*instruction.origin);
              if (position != system_order.end()) previous = position->second;
            }
          }
          if (system_out[b] != previous) {
            system_out[b] = previous;
            changed = true;
          }
        }
      }
    }
    for (size_t b = 0; b < f.blocks.size(); ++b) {
      std::optional<size_t> previous_system = system_in[b];
      Set initialized = in[b];
      const auto &bb = f.blocks[b];
      for (size_t p = 0; p < bb.instructions.size(); ++p) {
        const auto &i = bb.instructions[p];
        if (i.op == Op::cleanup_begin || i.op == Op::cleanup_end)
          previous_system.reset(); // Each defer body has its own order.
        const auto &t = type(i.type, i.source);
        need(i.op == Op::system_call || !i.system,
             "system metadata on a non-system instruction", i.source);
        need(i.op == Op::string_literal || i.utf8.empty(),
             "string bytes on a non-string instruction", i.source);
        std::vector<Def> args;
        for (auto v : i.inputs)
          args.push_back(use(v, b, p + 1, i.source));
        auto arity = [&](size_t n) {
          need(args.size() == n, "instruction operand count", i.source);
        };
        auto source_inputs = [&](hir::NodeKind expected) -> const hir::HirNode & {
          need(i.origin && i.origin->value < m.hir.module().nodes.size(),
               "source operation has no HIR origin", i.source);
          const auto &node = m.hir.module().nodes.at(i.origin->value);
          need(node.kind() == expected && node.type == i.type &&
                   same_span(node.source.span, i.source) &&
                   m.hir.module().scopes.at(node.scope.value).function == f.symbol &&
                   node.children.size() == args.size(),
               "source operation differs from verified HIR", i.source);
          for (size_t a = 0; a < args.size(); ++a)
            need(same_span(args[a].source,
                           m.hir.module().nodes.at(node.children[a].value).source.span),
                 "source operation operand differs from source", i.source);
          return node;
        };
        // Index read/place operations carry the receiver and index in source
        // order. Bind both operand definitions to the exact HIR children, not
        // merely to a type or a nearby instruction. A Vec/Slice bounds check
        // and write use the same IndexPlace origin.
        auto index_source = [&](hir::NodeKind expected,
                                bool source_is_origin) -> const hir::HirNode & {
          need(i.origin && i.origin->value < m.hir.module().nodes.size(),
               "index operation has no HIR origin", i.source);
          const auto &node = m.hir.module().nodes.at(i.origin->value);
          need(node.kind() == expected && node.children.size() == 2 &&
                   m.hir.module().scopes.at(node.scope.value).function == f.symbol &&
                   same_span(node.source.span, i.source) == source_is_origin,
               "index operation differs from verified HIR", i.source);
          for (size_t a = 0; a < 2; ++a)
            need(same_span(args[a].source,
                           m.hir.module().nodes.at(node.children[a].value).source.span),
                 "index receiver/index differs from source HIR", i.source);
          return node;
        };
        // A pure HIR constant expands to existing scalar/aggregate ops. Bind
        // its root by real Source and type as well as origin, so deleting the
        // origin cannot turn a damaged constant into an unchecked constructor.
        if (i.origin && i.origin->value < m.hir.module().nodes.size()) {
          const auto &root = m.hir.module().nodes[i.origin->value];
          if (root.kind() == hir::NodeKind::constant_literal) {
            // Cleanup markers name the full expression, not its value.
            const bool cleanup = i.op == Op::ownership_begin ||
                                 i.op == Op::ownership_end;
            need((cleanup ? t.kind == hir::TypeKind::Unit && !i.result
                          : root.type == i.type) &&
                     same_span(root.source.span, i.source),
                 "constant expansion changed its real source/type", i.source);
          }
        }
        auto [constant_begin, constant_end] = constant_roots.equal_range(
            ConstantKey{i.source.snapshot.value, i.source.begin_byte,
                        i.source.end_byte, i.type.value});
        for (auto entry = constant_begin; entry != constant_end; ++entry) {
          const size_t h = entry->second;
          const auto &root = m.hir.module().nodes[h];
          const auto *literal = std::get_if<hir::ConstantLiteral>(&root.data);
          if (!literal || !i.result || root.type != i.type ||
              root.source.span.snapshot != i.source.snapshot ||
              root.source.span.begin_byte != i.source.begin_byte ||
              root.source.span.end_byte != i.source.end_byte)
            continue;
          need(i.origin == HirNodeId{h},
               "constant expansion lost its real HIR origin", i.source);
          size_t work = 0;
          std::function<void(const hir::ConstantValue &, const Instruction &,
                             size_t)>
              check = [&](const auto &v, const auto &actual, size_t depth) {
                need(depth <= 128 && ++work <= 100000,
                     "constant expansion exceeds budget", i.source);
                need(v.type == actual.type, "constant expansion type",
                     i.source);
                std::visit(
                    [&](const auto &data) {
                      using T = std::decay_t<decltype(data)>;
                      if constexpr (std::is_same_v<T, hir::ConstantAggregate>) {
                        need(actual.op == (data.variant ? Op::enum_init
                                                        : Op::struct_init) &&
                                 (!data.variant ||
                                  actual.variant == *data.variant) &&
                                 actual.inputs.size() == data.fields.size(),
                             "constant expansion aggregate/variant", i.source);
                        for (size_t c = 0; c < data.fields.size(); ++c) {
                          auto d = instructions.find(actual.inputs[c]);
                          need(d != instructions.end(),
                               "constant field is not a real definition",
                               i.source);
                          check(data.fields[c], *d->second, depth + 1);
                        }
                      } else if constexpr (std::is_same_v<T,
                                                          hir::StringLiteral>)
                        need(actual.op == Op::string_literal &&
                                 actual.utf8 == data.utf8,
                             "constant expansion bytes", i.source);
                      else if constexpr (std::is_same_v<T, hir::FunctionValue>)
                        need(actual.op == Op::function &&
                                 actual.function == data.symbol,
                             "constant expansion function identity", i.source);
                      else {
                        uint64_t bits = 0;
                        if constexpr (std::is_same_v<T, hir::BoolLiteral>)
                          bits = data.value;
                        else if constexpr (!std::is_same_v<T, hir::UnitLiteral>)
                          bits = data.bits;
                        need(actual.op == Op::constant && actual.bits == bits,
                             "constant expansion scalar value", i.source);
                      }
                    },
                    v.data);
              };
          check(literal->value, i, 0);
        }
        auto value = [&](size_t n) -> const hir::Type & {
          need(!args.at(n).address, "address used as scalar value", i.source);
          return type(args[n].type, i.source);
        };
        need(i.exit.has_value() == (i.op == Op::exit_begin) &&
                 i.cleanup.has_value() ==
                     (i.op == Op::cleanup_begin || i.op == Op::cleanup_end),
             "invalid cleanup metadata", i.source);
        switch (i.op) {
        case Op::begin_borrow:
          arity(1);
          need(i.owner && i.slot.value < f.slots.size() &&
               ((i.owner->kind == hir::OwnerKind::binding && f.slots[i.slot.value].binding == SymbolId{i.owner->identity}) ||
                f.slots[i.slot.value].owner == i.owner), "borrow actual storage differs from owner", i.source);
          break;
        case Op::ownership_begin:
        case Op::ownership_end:
          arity(0);
          need(i.origin && i.origin->value < m.hir.module().nodes.size() &&
                   i.bits <= 1 && t.kind == hir::TypeKind::Unit,
               "ownership batch shape", i.source);
          {
            const auto &origin = m.hir.module().nodes.at(i.origin->value);
            need(same_span(origin.source.span, i.source) &&
                     m.hir.module().scopes.at(origin.scope.value).function == f.symbol,
                 "ownership batch source differs from HIR", i.source);
          }
          break;
        case Op::copy:
        case Op::borrow:
          arity(1); value(0);
          need(args[0].type == i.type, "copy/borrow type", i.source);
          if (i.origin) {
            const auto &source = m.hir.module().nodes.at(i.origin->value);
            if (source.kind() == hir::NodeKind::read) {
              bool matches = false;
              for (const auto &u : m.hir.module().ownership.uses)
                if (u.consumer == *i.origin && !u.destination && u.operand == 0)
                  matches = (i.op == Op::copy ? hir::UseKind::copy : hir::UseKind::borrow) == u.kind;
              need(matches, "copy/borrow differs from HIR use", i.source);
              auto definition = instructions.find(i.inputs[0]);
              need(definition != instructions.end() && definition->second->op == Op::load,
                   "copy/borrow does not read binding", i.source);
              const auto &slot = f.slots.at(definition->second->slot.value);
              if (slot.owner)
                need(slot.owner == hir::OwnerRef{hir::OwnerKind::binding, std::get<hir::Read>(source.data).symbol.value},
                     "copy/borrow from wrong binding", i.source);
            }
          }
          break;
        case Op::move:
          arity(0);
          need(i.slot.value < f.slots.size() && f.slots[i.slot.value].type == i.type && i.origin,
               "move slot/type/origin", i.source);
          { const auto &source = m.hir.module().nodes.at(i.origin->value);
            const auto *move = std::get_if<hir::Move>(&source.data);
            need(move && move->binding && (!f.slots[i.slot.value].owner ||
                 f.slots[i.slot.value].owner == hir::OwnerRef{hir::OwnerKind::binding, move->binding->value}),
                 "move differs from HIR", i.source);
          }
          break;
        case Op::take:
        case Op::destroy:
        case Op::iteration_end:
          arity(0);
          need(t.kind == hir::TypeKind::Unit && i.slot.value < f.slots.size() &&
                   f.slots[i.slot.value].owner,
               "owning action without live slot", i.source);
          if (i.owner) need(i.owner == f.slots[i.slot.value].owner,
                            "owning action uses another slot", i.source);
          break;
        case Op::end_borrow:
          arity(0); need(i.owner.has_value(), "end borrow without identity", i.source); break;
        case Op::save_output:
          arity(1); value(0);
          need(i.owner && hir::classify_value(m.hir.module(), args[0].type).needs_drop,
               "save output without owned value", i.source); break;
        case Op::vec_init:
          need(t.kind == hir::TypeKind::Vec, "Vec constructor type", i.source);
          for (size_t a = 0; a < args.size(); ++a) {
            value(a); need(args[a].type == t.type_args[0], "Vec element type", i.source);
          }
          break;
        case Op::vec_read:
        case Op::vec_check:
        case Op::vec_set: {
          arity(i.op == Op::vec_set ? 3 : 2);
          const bool text = value(0).kind == hir::TypeKind::String;
          need((value(0).kind == hir::TypeKind::Vec ||
                value(0).kind == hir::TypeKind::Slice ||
                (text && i.op == Op::vec_read)) &&
                   integer(value(1).kind), "index receiver/type", i.source);
          if (text) {
            source_inputs(hir::NodeKind::index_read);
          } else if (i.op == Op::vec_read) {
            const bool compound =
                i.origin && i.origin->value < m.hir.module().nodes.size() &&
                m.hir.module().nodes[i.origin->value].kind() ==
                    hir::NodeKind::index_place;
            const auto &node = index_source(
                compound ? hir::NodeKind::index_place
                         : hir::NodeKind::index_read,
                !compound);
            need(node.type == i.type, "indexed read source type", i.source);
          } else {
            index_source(hir::NodeKind::index_place, false);
          }
          if (i.op == Op::vec_read)
            need(text ? t.kind == hir::TypeKind::UnsignedInt && t.bits == 32
                      : type(args[0].type, i.source).type_args[0] == i.type,
                 "indexed result", i.source);
          else if (i.op == Op::vec_check) need(t.kind == hir::TypeKind::Unit, "Vec check type", i.source);
          else {
            value(2); need(type(args[0].type, i.source).type_args[0] == args[2].type &&
                          t.kind == hir::TypeKind::Unit, "Vec write type", i.source);
            required_checks.push_back(
                {b, p, *i.origin, i.inputs[0], i.inputs[1], i.source});
          }
          break;
        }
        case Op::vec_element_address: {
          arity(2);
          need((value(0).kind == hir::TypeKind::Vec ||
                value(0).kind == hir::TypeKind::Slice) &&
                   integer(value(1).kind) &&
                   type(args[0].type, i.source).type_args[0] == i.type,
               "Vec/Slice element address type", i.source);
          need(i.origin && i.origin->value < m.hir.module().nodes.size(),
               "Vec element address without HIR place", i.source);
          const auto &place = m.hir.module().nodes[i.origin->value];
          need(place.kind() == hir::NodeKind::index_place &&
                   place.type == i.type && place.children.size() == 2,
               "Vec element address origin is not the indexed place", i.source);
          required_checks.push_back(
              {b, p, *i.origin, i.inputs[0], i.inputs[1], i.source});
          auto from_place = [&](ValueId value, HirNodeId source) {
            const auto &definition = defs.at(value);
            const auto &span = m.hir.module().nodes[source.value].source.span;
            need(definition.source.snapshot == span.snapshot &&
                     definition.source.begin_byte >= span.begin_byte &&
                     definition.source.end_byte <= span.end_byte,
                 "Vec element address receiver/index differs from the HIR "
                 "indexed place",
                 i.source);
          };
          from_place(i.inputs[0], place.children[0]);
          from_place(i.inputs[1], place.children[1]);
          break;
        }
        case Op::vec_slice:
          need((value(0).kind == hir::TypeKind::Vec ||
                value(0).kind == hir::TypeKind::Slice ||
                value(0).kind == hir::TypeKind::String) &&
                   args[0].type == i.type && i.bits <= 3,
               "Vec/Slice slice type", i.source);
          arity(1 + bool(i.bits & 1) + bool(i.bits & 2));
          for (size_t a = 1; a < args.size(); ++a) need(integer(value(a).kind), "slice endpoint type", i.source);
          {
            const auto &origin = source_inputs(hir::NodeKind::vec_slice);
            const auto &slice = std::get<hir::VecSlice>(origin.data);
            need(i.bits == (bool(slice.start) | (bool(slice.end) << 1)),
                 "slice endpoint presence differs from source", i.source);
          }
          break;
        case Op::iteration_begin:
        case Op::iteration_next:
        case Op::iteration_read:
        case Op::iteration_test:
          arity(0);
          need(i.slot.value < f.slots.size() && f.slots[i.slot.value].owner &&
               f.slots[i.slot.value].owner->kind == hir::OwnerKind::iteration &&
               (type(f.slots[i.slot.value].type, i.source).kind == hir::TypeKind::Vec ||
                type(f.slots[i.slot.value].type, i.source).kind == hir::TypeKind::Slice ||
                type(f.slots[i.slot.value].type, i.source).kind == hir::TypeKind::String),
               "iteration slot", i.source);
          if (i.op == Op::iteration_read)
            need(type(f.slots[i.slot.value].type, i.source).kind == hir::TypeKind::String
                     ? t.kind == hir::TypeKind::UnsignedInt && t.bits == 32
                     : i.type == type(f.slots[i.slot.value].type, i.source).type_args[0],
                 "iteration element type", i.source);
          else need(t.kind == (i.op == Op::iteration_test ? hir::TypeKind::Bool : hir::TypeKind::Unit),
                    "iteration operation type", i.source);
          break;
        case Op::vec_length:
          arity(1); need((value(0).kind == hir::TypeKind::Vec ||
                         value(0).kind == hir::TypeKind::Slice ||
                         value(0).kind == hir::TypeKind::String) &&
                         t.kind == hir::TypeKind::Usize,
                         "iteration/length type", i.source); break;
        case Op::exit_begin:
          need(t.kind == hir::TypeKind::Unit, "exit marker type", i.source);
          for (size_t a = 0; a < args.size(); ++a)
            value(a);
          break;
        case Op::cleanup_begin:
        case Op::cleanup_end:
          arity(0);
          need(t.kind == hir::TypeKind::Unit, "cleanup marker type", i.source);
          break;
        case Op::string_literal:
          arity(0);
          need(t.kind == hir::TypeKind::String && i.bits == 0,
               "invalid string literal type/bits", i.source);
          try {
            validate_utf8(i.utf8);
          } catch (const SourceError &) {
            need(false, "invalid UTF-8 string literal", i.source);
          }
          break;
        case Op::constant:
          arity(0);
          need(integer(t.kind) || t.kind == hir::TypeKind::Unit ||
                   t.kind == hir::TypeKind::Bool ||
                   t.kind == hir::TypeKind::Float,
               "invalid constant type", i.source);
          if (t.kind == hir::TypeKind::Unit)
            need(i.bits == 0, "nonzero Unit", i.source);
          else if (t.kind == hir::TypeKind::Bool)
            need(i.bits <= 1, "invalid bool bits", i.source);
          else {
            auto w = hir::bit_width(t, m.hir.config());
            need(w == 64 || (i.bits >> w) == 0, "constant exceeds width",
                 i.source);
          }
          break;
        case Op::function: {
          need(functions.contains(i.function),
               "unknown function identity", i.source);
          if (!i.origin) {
            need(functions.at(i.function) == i.type,
                 "unknown function identity/signature", i.source);
            arity(0);
            const auto *h = hir_function_for(i.function);
            need(!h || h->captures.empty(),
                 "capturing function used as a direct callee", i.source);
            break;
          }
          need(i.origin->value < m.hir.module().nodes.size(),
               "function instruction origin", i.source);
          const auto &origin = m.hir.module().nodes[i.origin->value];
          if (const auto *v = std::get_if<hir::FunctionValue>(&origin.data)) {
            need(v->symbol == i.function && origin.type == i.type,
                 "function instruction differs from its HIR value", i.source);
            if (v->bound) {
              const auto &full = type(functions.at(i.function), i.source);
              need(!full.parameters.empty(), "bound method has no Self",
                   i.source);
              auto trimmed = full;
              trimmed.parameters.erase(trimmed.parameters.begin());
              need(type(i.type, i.source) == trimmed,
                   "bound function instruction signature", i.source);
            } else
              need(functions.at(i.function) == i.type,
                   "unknown function identity/signature", i.source);
            const size_t expected = v->bound ? 1 : v->captures.size();
            arity(expected);
            for (size_t a = 0; a < expected; ++a) {
              value(a);
              const auto expected_type =
                  v->bound ? v->bound->type : v->captures[a].type;
              need(args[a].type == expected_type,
                   "function environment input type/order", i.source);
            }
          } else if (const auto *c =
                         std::get_if<hir::ConstantLiteral>(&origin.data)) {
            const auto *v = std::get_if<hir::FunctionValue>(&c->value.data);
            need(v && v->symbol == i.function && v->captures.empty() &&
                     !v->bound && !v->move_capture && origin.type == i.type,
                 "constant function instruction differs from HIR", i.source);
            arity(0);
          } else
            need(false, "function instruction origin is not a function value",
                 i.source);
          break;
        }
        case Op::load:
        case Op::address:
        case Op::store: {
          need(i.slot.value < f.slots.size(), "unknown slot", i.source);
          auto st = f.slots[i.slot.value].type;
          if (i.op == Op::store) {
            arity(1);
            value(0);
            need(args[0].type == st && t.kind == hir::TypeKind::Unit,
                 "store type mismatch", i.source);
            initialized.insert(i.slot.value);
          } else {
            arity(0);
            need(st == i.type, "load/address type mismatch", i.source);
            if (reachable.contains(b))
              need(initialized.contains(i.slot.value),
                   "slot used before initialization", i.source);
          }
          break;
        }
        case Op::unary: {
          arity(1);
          auto k = value(0).kind;
          need(args[0].type == i.type &&
                   (i.unary == hir::UnaryOp::logical_not
                        ? k == hir::TypeKind::Bool
                        : i.unary == hir::UnaryOp::negate
                              ? integer(k) || k == hir::TypeKind::Float
                              : i.unary == hir::UnaryOp::bit_not && integer(k)),
               "invalid unary types/op", i.source);
          break;
        }
        case Op::binary: {
          arity(2);
          auto l = value(0).kind, r = value(1).kind;
          bool comparison = i.binary >= hir::BinaryOp::equal &&
                            i.binary <= hir::BinaryOp::greater_equal;
          bool shift = i.binary == hir::BinaryOp::shift_left ||
                       i.binary == hir::BinaryOp::shift_right;
          bool eq = i.binary == hir::BinaryOp::equal ||
                    i.binary == hir::BinaryOp::not_equal;
          bool floating = l == hir::TypeKind::Float && r == l &&
                          (comparison || i.binary == hir::BinaryOp::add ||
                           i.binary == hir::BinaryOp::subtract ||
                           i.binary == hir::BinaryOp::multiply ||
                           i.binary == hir::BinaryOp::divide);
          // String patterns and source operators use the same binary ops.
          bool string_equal = (comparison || i.binary == hir::BinaryOp::add) &&
                              l == hir::TypeKind::String && r == l;
          need(i.binary >= hir::BinaryOp::add &&
                   i.binary <= hir::BinaryOp::greater_equal &&
                   ((integer(l) && integer(r)) || floating || string_equal ||
                    (eq && l == hir::TypeKind::Bool && r == l)) &&
                   (shift || args[0].type == args[1].type) &&
                   (comparison ? t.kind == hir::TypeKind::Bool
                               : i.type == args[0].type),
               "invalid binary types/op", i.source);
          if (string_equal) {
            if (i.origin && m.hir.module().nodes.at(i.origin->value).kind() == hir::NodeKind::binary) {
              const auto &origin = source_inputs(hir::NodeKind::binary);
              need(std::get<hir::Binary>(origin.data).op == i.binary,
                   "text binary operator differs from source", i.source);
            } else if (i.origin) {
              const auto &origin = m.hir.module().nodes.at(i.origin->value);
              const auto *assign = std::get_if<hir::Assign>(&origin.data);
              need(assign && assign->compound == i.binary && i.binary == hir::BinaryOp::add &&
                       origin.children.size() == 2 &&
                       m.hir.module().nodes.at(origin.children[0].value).type == i.type &&
                       same_span(origin.source.span, i.source) &&
                       same_span(args[1].source, m.hir.module().nodes.at(origin.children[1].value).source.span),
                   "text compound operator differs from source", i.source);
            } else {
              bool pattern = false;
              auto expected = instructions.find(i.inputs[1]);
              std::function<void(const hir::Pattern &)> find = [&](const auto &p) {
                if (p.literal && std::holds_alternative<hir::StringLiteral>(*p.literal) &&
                    same_span(p.source.span, i.source) && expected != instructions.end() &&
                    expected->second->op == Op::string_literal &&
                    expected->second->utf8 == std::get<hir::StringLiteral>(*p.literal).utf8)
                  pattern = true;
                for (const auto &child : p.payload) find(child);
              };
              for (const auto &node : m.hir.module().nodes)
                if (const auto *sw = std::get_if<hir::Switch>(&node.data))
                  for (const auto &arm : sw->arms) find(arm.pattern);
              need(pattern && i.binary == hir::BinaryOp::equal,
                   "text binary without source or real pattern", i.source);
            }
          }
          break;
        }
        case Op::convert: {
          arity(1);
          const auto &source = value(0);
          const auto source_kind = source.kind, destination = t.kind;
          // Every f32/f64 source-target pair is a real conversion. In
          // particular f64->f32 narrowing is IEEE RNE and overflows to a
          // signed infinity; the HIR origin check below still binds it to the
          // exact checked T(x) expression and operand.
          const bool float_pair =
              source_kind == hir::TypeKind::Float &&
              destination == hir::TypeKind::Float &&
              (source.bits == 32 || source.bits == 64) &&
              (t.bits == 32 || t.bits == 64);
          need((integer(source_kind) && integer(destination)) ||
                   (source_kind == hir::TypeKind::Float &&
                    integer(destination)) ||
                   (integer(source_kind) &&
                    destination == hir::TypeKind::Float) ||
                   float_pair,
               "invalid numeric conversion", i.source);
          // The instruction is an implementation of one real HIR conversion,
          // not merely any pair of same-shaped SSA values.
          need(i.origin && i.origin->value < m.hir.module().nodes.size(),
               "conversion without HIR origin", i.source);
          const auto &origin = m.hir.module().nodes[i.origin->value];
          need(std::get_if<hir::IntegerConvert>(&origin.data) != nullptr &&
                   origin.type == i.type && origin.children.size() == 1,
               "conversion origin is not the matching HIR conversion",
               i.source);
          need(m.hir.module().scopes.at(origin.scope.value).function ==
                   f.symbol,
               "conversion origin belongs to another function", i.source);
          need(i.source.snapshot == origin.source.span.snapshot &&
                   i.source.begin_byte >= origin.source.span.begin_byte &&
                   i.source.end_byte <= origin.source.span.end_byte,
               "conversion source span differs from HIR origin", i.source);
          const auto &operand =
              m.hir.module().nodes[origin.children[0].value];
          need(operand.type == args[0].type,
               "conversion operand type differs from HIR", i.source);
          // The SSA input must be the actual value produced for this HIR
          // operand, not merely any same-typed value defined somewhere inside
          // its SourceSpan. Follow only the existing transparent copy/borrow
          // wrappers, keyed by the real ValueId chain; do not use block/slot
          // depth as a proxy for value identity.
          ValueId operand_value = i.inputs[0];
          std::set<ValueId> visited;
          for (;;) {
            need(visited.insert(operand_value).second,
                 "conversion operand value cycle", i.source);
            auto instruction = instructions.find(operand_value);
            if (instruction != instructions.end()) {
              const auto &transparent = *instruction->second;
              if ((transparent.op == Op::copy ||
                   transparent.op == Op::borrow) &&
                  transparent.inputs.size() == 1) {
                operand_value = transparent.inputs[0];
                continue;
              }
            }
            const auto &definition = defs.at(operand_value);
            need(same_span(definition.source, operand.source.span),
                 "conversion operand value differs from the real HIR operand",
                 i.source);
            break;
          }
          break;
        }
        case Op::system_call: {
          need(i.origin && i.origin->value < m.hir.module().nodes.size(),
               "system call without HIR origin", i.source);
          const auto &source = m.hir.module().nodes[i.origin->value];
          const auto *call = std::get_if<hir::Call>(&source.data);
          need(call && call->kind == hir::CalleeKind::system && call->system &&
                   i.system == call->system && call->signature == i.signature &&
                   m.hir.module().scopes.at(source.scope.value).function == f.symbol,
               "system operation/signature differs from verified HIR", i.source);
          need(same_span(i.source, source.source.span),
               "system operation source differs from HIR", i.source);
          auto position = system_order.find(*i.origin);
          need(position != system_order.end() &&
                   (!previous_system || position->second > *previous_system),
               "system operation order differs from HIR evaluation", i.source);
          previous_system = position->second;
          const auto &cs = type(i.signature, i.source);
          need(cs.kind == hir::TypeKind::Function && !cs.throws_type &&
                   i.type == cs.result,
               "invalid system call signature/result", i.source);
          arity(cs.parameters.size());
          for (size_t a = 0; a < cs.parameters.size(); ++a) {
            value(a);
            need(cs.parameters[a].mode == hir::ParamMode::ordinary &&
                     args[a].type == cs.parameters[a].type && !args[a].address,
                 "system argument type/mode mismatch", i.source);
            const auto &argument = call->args.at(a);
            need(argument.parameter_index == a,
                 "system argument order differs from HIR", i.source);
            ValueId operand = i.inputs[a];
            std::set<ValueId> visited;
            for (;;) {
              need(visited.insert(operand).second,
                   "system argument value cycle", i.source);
              auto definition = instructions.find(operand);
              if (definition != instructions.end() &&
                  (definition->second->op == Op::copy ||
                   definition->second->op == Op::borrow)) {
                need(definition->second->inputs.size() == 1,
                     "invalid system argument copy/borrow", i.source);
                operand = definition->second->inputs.front();
                continue;
              }
              break;
            }
            need(same_span(defs.at(operand).source,
                           m.hir.module().nodes.at(argument.expression.value).source.span),
                 "system argument value differs from HIR operand", i.source);
          }
          break;
        }
        case Op::foreign_call: {
          need(!i.system && i.c_callable.has_value(),
               "C call missing frozen metadata", i.source);
          need(i.origin && i.origin->value < m.hir.module().nodes.size(),
               "C call without HIR origin", i.source);
          const auto &source = m.hir.module().nodes[i.origin->value];
          const auto *call = std::get_if<hir::Call>(&source.data);
          need(call && call->kind == hir::CalleeKind::c_foreign &&
                   call->c_callable == i.c_callable &&
                   call->signature == i.signature &&
                   m.hir.module().scopes.at(source.scope.value).function == f.symbol,
               "C call metadata differs from HIR", i.source);
          const auto &cs = type(i.signature, i.source);
          need(cs.kind == hir::TypeKind::Function && !cs.throws_type &&
                   i.type == cs.result,
               "C call signature/result", i.source);
          need(*i.c_callable >= 1 &&
                   *i.c_callable <= m.hir.module().resolved->project->c_callables.size(),
               "C callable index", i.source);
          auto position = system_order.find(*i.origin);
          need(position != system_order.end() &&
                   (!previous_system || position->second > *previous_system),
               "C call order differs from HIR evaluation", i.source);
          previous_system = position->second;
          arity(cs.parameters.size());
          for (size_t a = 0; a < cs.parameters.size(); ++a) {
            value(a);
            const auto &argument = call->args.at(a);
            need(argument.parameter_index == a && argument.source_index == a &&
                     !argument.label &&
                     argument.mode == hir::ParamMode::ordinary,
                 "C argument order/mode differs from HIR", i.source);
            if (type(cs.parameters[a].type, i.source).kind ==
                hir::TypeKind::CForeignCallback) {
              need(m.hir.module().nodes.at(argument.expression.value).kind() ==
                       hir::NodeKind::function_value &&
                       args[a].type ==
                           m.hir.module().nodes.at(argument.expression.value).type &&
                       !args[a].address,
                   "C callback argument type/identity", i.source);
            } else {
              bool compatible = args[a].type == cs.parameters[a].type;
              if (!compatible) {
                const auto &actual = type(args[a].type, i.source);
                const auto &wanted = type(cs.parameters[a].type, i.source);
                compatible = (wanted.kind == hir::TypeKind::CForeignPointer ||
                              wanted.kind == hir::TypeKind::CForeignBuffer) &&
                             actual.kind == wanted.kind && wanted.bits == 0 &&
                             actual.bits == 1 &&
                             actual.type_args == wanted.type_args;
              }
              need(compatible && !args[a].address,
                   "C argument type/mode mismatch", i.source);
            }
          }
          break;
        }
        case Op::c_field_read: {
          arity(1);
          need(i.c_type, "C field metadata", i.source);
          need(i.origin && i.origin->value < m.hir.module().nodes.size(),
               "C field without HIR origin", i.source);
          const auto &source = m.hir.module().nodes.at(i.origin->value);
          const auto *field = std::get_if<hir::CFieldRead>(&source.data);
          need(field && field->c_type == i.c_type && field->field == i.c_field &&
                   source.type == i.type && source.children.size() == 1,
               "C field differs from HIR", i.source);
          const auto &receiver = type(args[0].type, i.source);
          need(receiver.kind == hir::TypeKind::CForeignRecord &&
                   receiver.c_type == i.c_type && !args[0].address,
               "C field receiver type", i.source);
          const auto &decl =
              m.hir.module().resolved->project->c_type(i.c_type);
          need(decl.kind == modules::CDeclKind::record &&
                   i.c_field < decl.fields.size(),
               "C field declaration", i.source);
          hir::TypeContext scratch = m.hir.module().types;
          auto expected = hir::detail::c_type_id(
              *m.hir.module().resolved->project, scratch,
              decl.fields[i.c_field].type);
          need(expected == i.type, "C field result type", i.source);
          break;
        }
        case Op::c_ptr_read:
        case Op::c_buffer_read: {
          arity(2);
          const auto &receiver = type(args[0].type, i.source);
          need(receiver.kind == (i.op == Op::c_ptr_read
                                     ? hir::TypeKind::CForeignPointer
                                     : hir::TypeKind::CForeignBuffer) &&
                   receiver.type_args.size() == 1 &&
                   i.type == receiver.type_args[0] && !args[0].address,
               "C indexed read receiver/result", i.source);
          need(integer(type(args[1].type, i.source).kind) && !args[1].address,
               "C indexed read index type", i.source);
          need(i.origin && i.origin->value < m.hir.module().nodes.size(),
               "C indexed read without HIR origin", i.source);
          const auto &source = m.hir.module().nodes.at(i.origin->value);
          need((source.kind() == hir::NodeKind::index_read ||
                source.kind() == hir::NodeKind::index_place) &&
                   source.children.size() == 2 && source.type == i.type,
               "C indexed read differs from HIR", i.source);
          if (source.kind() == hir::NodeKind::index_place)
            need(receiver.bits == 1,
                 "C compound read of read-only pointer", i.source);
          break;
        }
        case Op::c_ptr_write:
        case Op::c_buffer_write: {
          arity(3);
          const auto &receiver = type(args[0].type, i.source);
          need(receiver.kind == (i.op == Op::c_ptr_write
                                     ? hir::TypeKind::CForeignPointer
                                     : hir::TypeKind::CForeignBuffer) &&
                   receiver.type_args.size() == 1 && receiver.bits == 1 &&
                   args[2].type == receiver.type_args[0] && !args[0].address &&
                   !args[2].address,
               "C indexed write receiver/result", i.source);
          need(integer(type(args[1].type, i.source).kind) && !args[1].address,
               "C indexed write index type", i.source);
          need(i.origin && i.origin->value < m.hir.module().nodes.size(),
               "C indexed write without HIR origin", i.source);
          const auto &source = m.hir.module().nodes.at(i.origin->value);
          need(source.kind() == hir::NodeKind::index_place &&
                   source.children.size() == 2,
               "C indexed write differs from HIR", i.source);
          break;
        }
        case Op::trait_object: {
          arity(1);
          value(0);
          need(t.kind == hir::TypeKind::TraitObject,
               "trait object constructor type", i.source);
          need(i.origin && i.origin->value < m.hir.module().nodes.size(),
               "trait object constructor without HIR origin", i.source);
          const auto &source = m.hir.module().nodes[i.origin->value];
          const auto *init = std::get_if<hir::TraitObjectInit>(&source.data);
          need(init && source.kind() == hir::NodeKind::trait_object_init &&
                   m.hir.module().scopes.at(source.scope.value).function ==
                       f.symbol &&
                   init->trait.declaration == i.variant &&
                   init->concrete == args[0].type &&
                   init->methods == i.methods &&
                   m.hir.module().types.find(i.type)->type_args ==
                       init->trait.args &&
                   same_span(source.source.span, i.source),
               "trait object constructor differs from HIR", i.source);
          break;
        }
        case Op::trait_call: {
          need(i.origin && i.origin->value < m.hir.module().nodes.size(),
               "trait call without HIR origin", i.source);
          const auto &source = m.hir.module().nodes[i.origin->value];
          const auto *dispatch = std::get_if<hir::TraitDispatch>(&source.data);
          need(dispatch && source.kind() == hir::NodeKind::trait_dispatch &&
                   m.hir.module().scopes.at(source.scope.value).function ==
                       f.symbol,
               "trait call origin differs from HIR", i.source);
          const auto &cs = type(i.signature, i.source);
          need(cs.kind == hir::TypeKind::Function && !cs.throws_type &&
                   dispatch->signature == i.signature &&
                   dispatch->index == i.bits &&
                   dispatch->method == i.field &&
                   i.type == cs.result,
               "trait call ABI signature differs from HIR", i.source);
          need(value(0).kind == hir::TypeKind::TraitObject,
               "trait call receiver is not dyn", i.source);
          arity(cs.parameters.size() + 1);
          for (size_t a = 0; a < cs.parameters.size(); ++a)
            need(args[a + 1].type == cs.parameters[a].type &&
                     !args[a + 1].address &&
                     cs.parameters[a].mode == hir::ParamMode::ordinary,
                 "trait call argument type/mode", i.source);
          need(same_span(source.source.span, i.source),
               "trait call source span differs from HIR", i.source);
          break;
        }
        case Op::call: {
          need(!i.system, "system metadata on ordinary call", i.source);
          need(i.origin && i.origin->value < m.hir.module().nodes.size(),
               "call without HIR origin", i.source);
          const auto &source = m.hir.module().nodes[i.origin->value];
          const auto *call = std::get_if<hir::Call>(&source.data);
          need(call && call->signature == i.signature &&
                   m.hir.module().scopes.at(source.scope.value).function ==
                       f.symbol,
               "call origin/signature differs from HIR", i.source);
          const auto &cs = type(i.signature, i.source);
          need(cs.kind == hir::TypeKind::Function && !cs.throws_type,
               "call signature is not function", i.source);
          arity(cs.parameters.size() + 1);
          value(0);
          need(args[0].type == i.signature && i.type == cs.result,
               "callee/result type mismatch", i.source);
          for (size_t a = 0; a < cs.parameters.size(); ++a)
            need(args[a + 1].type == cs.parameters[a].type &&
                     args[a + 1].address ==
                         (cs.parameters[a].mode == hir::ParamMode::inout),
                 "call argument type/mode mismatch", i.source);
          break;
        }
        case Op::struct_init:
        case Op::enum_init: {
          bool structure = i.op == Op::struct_init;
          need(t.kind == (structure ? hir::TypeKind::NominalStruct
                                    : hir::TypeKind::NominalEnum),
               "aggregate constructor kind", i.source);
          auto fields = aggregates.fields(
              i.type, structure ? std::nullopt : std::optional{i.variant});
          arity(fields.size());
          for (size_t a = 0; a < fields.size(); ++a) {
            value(a);
            need(args[a].type == fields[a].type, "constructor field type",
                 i.source);
          }
          break;
        }
        case Op::field_read:
        case Op::field_address: {
          arity(1);
          need(args[0].address == (i.op == Op::field_address) &&
                   type(args[0].type, i.source).kind ==
                       hir::TypeKind::NominalStruct,
               "field receiver type/mode", i.source);
          auto fields = aggregates.fields(args[0].type);
          auto index = aggregates.field_index(args[0].type, i.field);
          need(i.type == fields[index].type, "field result type", i.source);
          if (i.op == Op::field_address) {
            bool found = false;
            const auto &h = m.hir.module();
            for (const auto &source : h.nodes) {
              const auto &span = source.source.span;
              if (source.kind() != hir::NodeKind::field_place ||
                  span.snapshot != i.source.snapshot ||
                  span.begin_byte != i.source.begin_byte ||
                  span.end_byte != i.source.end_byte ||
                  h.scopes.at(source.scope.value).function != f.symbol)
                continue;
              need(std::get<hir::FieldPlace>(source.data).field == i.field &&
                       source.type == i.type,
                   "field address differs from the HIR source place", i.source);
              found = true;
              break;
            }
            need(found, "field address has no HIR source place", i.source);
          }
          break;
        }
        case Op::load_indirect:
          arity(1);
          need(args[0].address && args[0].type == i.type,
               "indirect load type/mode", i.source);
          break;
        case Op::replace_indirect:
        case Op::store_indirect:
          arity(2);
          value(1);
          need(args[0].address && args[0].type == args[1].type &&
                   t.kind == hir::TypeKind::Unit,
               "indirect store type/mode", i.source);
          need(hir::classify_value(m.hir.module(), args[1].type).needs_drop ==
               (i.op == Op::replace_indirect), "owned field replacement lacks actual destroy", i.source);
          break;
        case Op::enum_test:
        case Op::enum_payload: {
          arity(1);
          need(value(0).kind == hir::TypeKind::NominalEnum,
               "enum operation receiver", i.source);
          aggregates.variant_index(args[0].type, i.variant);
          if (i.op == Op::enum_test)
            need(t.kind == hir::TypeKind::Bool, "enum test result", i.source);
          else {
            auto fields = aggregates.fields(args[0].type, i.variant);
            auto index =
                aggregates.field_index(args[0].type, i.field, i.variant);
            need(fields[index].type == i.type, "enum payload result type",
                 i.source);
            need(facts[b].contains({i.inputs[0], i.variant, true}),
                 "enum payload without matching variant edge", i.source);
          }
          break;
        }
        default:
          need(false, "unknown native operation", i.source);
        }
      }
      const auto &t = bb.terminator;
      TypeId async_return = TypeId{0}, async_error = TypeId{0};
      if (f.async) {
        const auto &task = type(sig.result, t.source);
        need(task.kind == hir::TypeKind::Task && task.type_args.size() == 2,
             "async function signature result", t.source);
        async_return = task.type_args.at(0);
        async_error = task.type_args.at(1);
      }
      if (t.kind == Exit::error_return)
        need(t.origin && t.exit && t.exit->kind == hir::ExitKind::error &&
                 t.exit->error_type ==
                     (f.async ? std::optional<TypeId>(async_error)
                              : sig.throws_type),
             "ErrorReturn without verified error exit", t.source);
      if (t.kind == Exit::return_ && !t.origin)
        need(type(f.async ? async_return : sig.result, t.source).kind ==
                 hir::TypeKind::Unit &&
                 t.source.snapshot == f.source.snapshot &&
                 t.source.begin_byte == f.source.begin_byte &&
                 t.source.end_byte == f.source.end_byte,
             "explicit return without source exit identity", t.source);
      std::vector<Def> operands;
      for (auto v : t.inputs)
        operands.push_back(use(v, b, bb.instructions.size() + 1, t.source));
      if (t.kind == Exit::try_call) {
        const auto &cs = type(t.signature, t.source);
        need(cs.kind == hir::TypeKind::Function && cs.throws_type && t.origin,
             "TryCall requires throwing signature and source call", t.source);
        need(t.origin->value < m.hir.module().nodes.size(),
             "unknown TryCall origin", t.source);
        const auto *call =
            std::get_if<hir::Call>(&m.hir.module().nodes[t.origin->value].data);
        need(call && call->signature == t.signature,
             "TryCall differs from source signature", t.source);
        need(operands.size() == cs.parameters.size() + 1 &&
                 !operands[0].address && operands[0].type == t.signature,
             "TryCall callee/arity", t.source);
        for (size_t a = 0; a < cs.parameters.size(); ++a)
          need(operands[a + 1].type == cs.parameters[a].type &&
                   operands[a + 1].address ==
                       (cs.parameters[a].mode == hir::ParamMode::inout),
               "TryCall argument type/mode", t.source);
        for (unsigned side = 0; side < 2; ++side) {
          auto target = t.targets[side].value;
          const auto &params = f.blocks[target].parameters;
          need(
              predecessors[target] == Set{b} && params.size() == 1 &&
                  params[0].type == (side == 0 ? cs.result : *cs.throws_type) &&
                  params[0].call == *t.origin && params[0].error == (side == 1),
              "TryCall side parameter/predecessor", t.source);
        }
      } else if (t.kind == Exit::await_) {
        need(t.targets.size() == 3 && t.origin, "await arity/origin", t.source);
        const auto &task = type(t.signature, t.source);
        need(task.kind == hir::TypeKind::Task && task.type_args.size() == 2,
             "await task signature", t.source);
        need(operands.size() == 1 && !operands[0].address &&
                 operands[0].type == t.signature,
             "await future operand", t.source);
        const auto error_type = task.type_args[1];
        const bool unit_error =
            type(error_type, t.source).kind == hir::TypeKind::Unit;
        for (unsigned side = 0; side < 2; ++side) {
          auto target = t.targets[side].value;
          const auto &params = f.blocks[target].parameters;
          const auto expected = task.type_args[side];
          const bool unit =
              type(expected, t.source).kind == hir::TypeKind::Unit;
          need(predecessors[target] == Set{b} &&
                   params.size() == (unit ? 0u : 1u) &&
                   (unit || (params[0].type == expected &&
                             params[0].call == *t.origin &&
                             params[0].error == (side == 1))),
               "await side parameter/predecessor", t.source);
        }
        need(f.blocks[t.targets[2].value].parameters.empty(),
             "await cancel parameter", t.source);
        (void)unit_error;
      } else {
        need(t.signature.value == 0, "signature on non-TryCall", t.source);
        for (const auto &d : operands)
          need(!d.address &&
                   (t.kind == Exit::return_
                        ? d.type == (f.async ? async_return : sig.result)
                    : t.kind == Exit::error_return
                        ? d.type == (f.async ? async_error : *sig.throws_type)
                        : type(d.type, t.source).kind == hir::TypeKind::Bool),
               "terminator type mismatch", t.source);
      }
      if (!bb.parameters.empty()) {
        need(b != 0 && predecessors[b].size() == 1, "orphan block parameter",
             t.source);
        const auto &pred = f.blocks[*predecessors[b].begin()].terminator;
        need(pred.kind == Exit::try_call || pred.kind == Exit::await_,
             "parameter without TryCall/await edge", t.source);
      }
    }
    Set impossible;
    for (size_t b = 0; b < f.blocks.size(); ++b)
      for (const auto &[value, variant, matches] : facts[b]) {
        if (matches)
          continue;
        const auto &variants = aggregates.declaration(defs.at(value).type).variants;
        if (!variants.empty() &&
            std::all_of(variants.begin(), variants.end(), [&](const auto &v) {
              return facts[b].contains({value, v.symbol, false});
            }))
          impossible.insert(b);
      }
    // A Vec/Slice write and an indexed-field element address must be dominated
    // by a bounds check with the same IndexPlace origin and the same receiver
    // and index values. The check may be in a predecessor block when the RHS
    // has an error edge (for example a throwing call), so dominance and real
    // source identity are used instead of instruction adjacency.
    for (const auto &required : required_checks) {
      bool found = false;
      for (size_t candidate_block : reachable) {
        if (!dom[required.block].contains(candidate_block))
          continue;
        const auto &candidate_bb = f.blocks[candidate_block];
        for (size_t c = 0; c < candidate_bb.instructions.size(); ++c) {
          if (candidate_block == required.block && c >= required.position)
            continue;
          const auto &candidate = candidate_bb.instructions[c];
          if (candidate.op == Op::vec_check && candidate.origin &&
              *candidate.origin == required.origin &&
              candidate.inputs.size() == 2 &&
              candidate.inputs[0] == required.receiver &&
              candidate.inputs[1] == required.index) {
            found = true;
            break;
          }
        }
        if (found)
          break;
      }
      need(found, "Vec/Slice write lacks matching source bounds check",
           required.source);
    }
    cleanup(f, dom, predecessors, impossible);
    ownership(f);
  }

#include "ownership_verify.inc"

  // Independent CFG dataflow over exit obligations. It reads immutable HIR
  // plans, never calls Lower and never trusts copied cleanup order in NativeIR.
  void cleanup(const Function &f, const std::vector<Set> &dom,
               const std::vector<Set> &predecessors,
               const Set &impossible) const {
    const auto &h = m.hir.module();
    auto node = [&](HirNodeId id) -> const hir::HirNode & {
      need(id.value < h.nodes.size() &&
               h.scopes.at(h.nodes[id.value].scope.value).function == f.symbol,
           "exit/cleanup from another function", f.source);
      return h.nodes.at(id.value);
    };
    auto plan = [&](const ExitSite &site) -> const hir::ExitPlan & {
      for (const auto &p : node(site.origin).exits)
        if (p.kind == site.kind && p.error_type == site.error_type)
          return p;
      need(false, "exit marker without HIR plan", f.source);
      throw std::logic_error("unreachable");
    };
    // Same HIR Try can have multiple inline instances. Identity includes its
    // actual protected entry and handler destination; bindings are never
    // shared.
    std::map<std::tuple<HirNodeId, uint32_t, BlockId>, const Handler *>
        handlers;
    std::set<BlockId> handler_blocks;
    std::set<LocalSlotId> handler_slots;
    std::set<std::tuple<HirNodeId, uint32_t, BlockId>> instances;
    for (const auto &handler : f.handlers) {
      const auto &n = node(handler.target.try_node);
      auto t = std::get_if<hir::Try>(&n.data);
      need(t && handler.target.index < t->catches.size() &&
               handler.block.value < f.blocks.size() &&
               handler.protected_entry.value < f.blocks.size() &&
               handler.slot.value < f.slots.size(),
           "invalid native handler", f.source);
      const auto &incoming = predecessors[handler.protected_entry.value];
      need(incoming.size() == 1,
           "handler protected entry is not a concrete Try entry", f.source);
      const auto &entry = f.blocks[*incoming.begin()].terminator;
      need(entry.kind == Exit::branch &&
               entry.origin == handler.target.try_node &&
               entry.source.snapshot == n.source.span.snapshot &&
               entry.source.begin_byte == n.source.span.begin_byte &&
               entry.source.end_byte == n.source.span.end_byte,
           "handler protected entry differs from source Try", f.source);
      need(f.slots[handler.slot.value].type ==
                   t->catches[handler.target.index].error_type &&
               !f.slots[handler.slot.value].parameter &&
               handler.block != handler.protected_entry &&
               handler_blocks.insert(handler.block).second &&
               handler_slots.insert(handler.slot).second &&
               instances
                   .emplace(handler.target.try_node, handler.target.index,
                            handler.protected_entry)
                   .second &&
               handlers
                   .emplace(std::tuple{handler.target.try_node,
                                       handler.target.index, handler.block},
                            &handler)
                   .second,
           "handler binding type/identity", f.source);
    }
    struct Frame {
      ExitSite site;
      size_t next = 0;
      bool active = false;
      std::vector<ValueId> saved;
      size_t action = 0;
      bool operator==(const Frame &) const = default;
    };
    using State = std::vector<Frame>;
    detail::Obligations actual;
    auto context = [&](const State &state) {
      detail::Obligations::Context path;
      for (const auto &frame : state)
        if (frame.active) {
          auto d = plan(frame.site).defers.at(frame.next);
          path.emplace_back(frame.site.origin, frame.site.kind,
                            frame.site.error_type, d);
        }
      return path;
    };
    std::vector<std::optional<State>> states(f.blocks.size());
    states[0] = State{};
    std::vector<size_t> work{0};
    while (!work.empty()) {
      auto b = work.back();
      work.pop_back();
      auto state = *states[b];
      const auto &bb = f.blocks[b];
      bool ownership_batch = false;
      for (const auto &i : bb.instructions) {
        if (i.op == Op::ownership_begin || i.op == Op::ownership_end)
          ownership_batch = i.op == Op::ownership_begin;
        if (i.op == Op::destroy && f.slots.at(i.slot.value).owner &&
            f.slots.at(i.slot.value).owner->kind == hir::OwnerKind::temporary)
          need(ownership_batch || (!state.empty() && !state.back().active),
               "temporary destroy outside a verified cleanup plan", i.source);
        if (i.op != Op::cleanup_begin && i.op != Op::cleanup_end) {
          for (const auto &frame : state)
            if (frame.active) {
              const auto &p = plan(frame.site);
              const auto &body =
                  node(node(p.defers.at(frame.next)).children[0]);
              const auto &range = body.source.span;
              need(i.source.snapshot == range.snapshot &&
                       i.source.begin_byte >= range.begin_byte &&
                       i.source.end_byte <= range.end_byte,
                   "instruction moved across cleanup bodies", i.source);
            }
        }
        if ((i.op == Op::call || i.op == Op::system_call ||
             i.op == Op::foreign_call) && !impossible.contains(b))
          ++actual.calls[{context(state), *i.origin}];
        if (i.op == Op::exit_begin) {
          const auto &site = *i.exit;
          if (!impossible.contains(b))
            ++actual.exits[{context(state), site.origin, site.kind,
                            site.error_type}];
          const auto &p = plan(site);
          need(state.empty() || state.back().active,
               "exit begins during pending cleanup", i.source);
          need(i.inputs.size() == (p.saves_value ? 1u : 0u),
               "exit value not saved before cleanup", i.source);
          state.push_back({*i.exit, 0, false, i.inputs});
        } else if (i.op == Op::cleanup_begin) {
          need(!state.empty(), "cleanup without exit obligation", i.source);
          auto &top = state.back();
          const auto &p = plan(top.site);
          need(!top.active && top.next < p.defers.size() &&
                   p.defers[top.next] == *i.cleanup &&
                   node(*i.cleanup).kind() == hir::NodeKind::defer_,
               "cleanup registration/order differs from HIR", i.source);
          if (!p.actions.empty()) {
            need(top.action < p.actions.size() && p.actions[top.action].kind == hir::CleanupKind::defer_ &&
                 p.actions[top.action].owner.identity == i.cleanup->value,
                 "defer moved across destroy/output actions", i.source);
          }
          top.active = true;
        } else if (i.op == Op::cleanup_end) {
          need(!state.empty(), "cleanup end without begin", i.source);
          auto &top = state.back();
          const auto &p = plan(top.site);
          need(top.active && p.defers.at(top.next) == *i.cleanup,
               "cleanup end mismatch", i.source);
          top.active = false;
          ++top.next;
          if (!p.actions.empty()) ++top.action;
        } else if (!state.empty() && !state.back().active &&
                   (i.op == Op::destroy || i.op == Op::end_borrow ||
                    i.op == Op::save_output || i.op == Op::iteration_end)) {
          auto &top = state.back(); const auto &p = plan(top.site);
          need(top.action < p.actions.size(), "extra owning cleanup", i.source);
          const auto &expected = p.actions[top.action++];
          auto kind = i.op == Op::destroy ? hir::CleanupKind::destroy_if_live :
                      i.op == Op::end_borrow ? hir::CleanupKind::end_borrow :
                      i.op == Op::save_output ? hir::CleanupKind::save_output : hir::CleanupKind::end_iteration;
          need(kind == expected.kind && i.owner == expected.owner,
               "actual owning operation/order differs from HIR exit", i.source);
          if (i.op == Op::save_output) need(i.inputs == top.saved, "saved wrong owning output", i.source);
          if (i.op == Op::destroy) need(f.slots.at(i.slot.value).type == expected.type,
                                      "destroy type differs from HIR", i.source);
        }
      }
      const auto &t = bb.terminator;
      if (t.kind == Exit::try_call && !impossible.contains(b))
        ++actual.calls[{context(state), *t.origin}];
      if (t.origin && (t.kind == Exit::return_ || t.kind == Exit::error_return)) {
        // A function-level edge must apply its matching HIR plan when one is
        // recorded. Trivial exits have no plan. Await's cancel plan belongs to
        // its separate cancel_return edge, and a caught error plan is applied
        // on a branch whose t.exit is validated below.
        const auto want = t.kind == Exit::return_ ? hir::ExitKind::return_
                                                  : hir::ExitKind::error;
        for (const auto &p : node(*t.origin).exits)
          if (p.kind == want)
            need(t.exit && t.exit->origin == *t.origin &&
                     t.exit->kind == p.kind &&
                     t.exit->error_type == p.error_type,
                 "exit omitted required cleanup plan", t.source);
      }
      if (t.kind == Exit::await_) {
        // The await node owns exactly one error plan (when E is not Unit) and
        // one cancel plan; both must be present at the matching exit blocks.
        const auto &n = node(*t.origin);
        const auto &task = type(t.signature, t.source);
        const auto error_type = task.type_args[1];
        size_t errors = 0, cancels = 0;
        for (const auto &p : n.exits) {
          errors += p.kind == hir::ExitKind::error;
          cancels += p.kind == hir::ExitKind::cancel;
        }
        need(errors == (type(error_type, t.source).kind == hir::TypeKind::Unit
                            ? 0u
                            : 1u) &&
                 cancels == 1,
             "await cleanup plans", t.source);
      }
      if (t.exit) {
        const auto &p = plan(*t.exit);
        need(t.origin == t.exit->origin && !state.empty(),
             "exit end without begin", t.source);
        auto top = state.back();
        need(top.site == *t.exit && !top.active && top.next == p.defers.size() && top.action == p.actions.size(),
             "exit before all cleanups completed", t.source);
        if (p.kind == hir::ExitKind::return_ ||
            p.kind == hir::ExitKind::error) {
          if (auto target = std::get_if<hir::CatchTarget>(&p.target)) {
            need(t.kind == Exit::branch, "catch exit is not a branch",
                 t.source);
            auto found =
                handlers.find({target->try_node, target->index, t.targets[0]});
            need(found != handlers.end() && t.kind == Exit::branch &&
                     t.targets[0] == found->second->block &&
                     !top.saved.empty() &&
                     dom[b].contains(found->second->protected_entry.value),
                 "error delivered to wrong handler", t.source);
            need(!bb.instructions.empty(), "missing error binding store",
                 t.source);
            const auto &store = bb.instructions.back();
            need(store.op == Op::store && store.slot == found->second->slot &&
                     store.inputs == top.saved,
                 "handler received different error value", t.source);
          } else {
            need(t.kind == (p.kind == hir::ExitKind::error ? Exit::error_return
                                                           : Exit::return_),
                 "wrong function exit kind", t.source);
            if (p.saves_value)
              need(t.inputs == top.saved, "exit value changed after cleanup",
                   t.source);
          }
        } else if (p.kind == hir::ExitKind::cancel)
          need(t.kind == Exit::cancel_return, "cancel exit kind", t.source);
        else
          need(t.kind == Exit::branch, "normal/jump exit is not branch",
               t.source);
        state.pop_back();
      }
      // An exhaustive enum switch's default edge cannot execute, even when
      // generated inside a cleanup. Other unfinished exits remain invalid.
      if (t.kind == Exit::return_ || t.kind == Exit::error_return ||
          t.kind == Exit::cancel_return ||
          (t.kind == Exit::unreachable && !impossible.contains(b)))
        need(state.empty(), "function exits with unfinished cleanup", t.source);
      for (auto target : t.targets) {
        if (!states[target.value]) {
          states[target.value] = state;
          work.push_back(target.value);
        } else
          need(*states[target.value] == state,
               "inconsistent cleanup state at CFG join", t.source);
      }
    }
    obligations(f, actual);
  }

  void obligations(const Function &f, const detail::Obligations &actual) const {
    const hir::HirFunction *source = nullptr;
    for (const auto &candidate : m.hir.module().functions)
      if (candidate.symbol == f.symbol)
        source = &candidate;
    need(source != nullptr, "missing HIR function for exit obligations",
         f.source);
    const auto expected = detail::RequiredExits(m.hir.module()).run(*source);
    need(actual.exits == expected.exits,
         "required HIR exit/cleanup occurrence missing or duplicated",
         f.source);
    need(actual.calls == expected.calls,
         "required HIR call occurrence missing or duplicated", f.source);
  }

public:
  explicit Verifier(const Module &input)
      : m(input), types(input.hir.module().types) {}
  void run() {
    auto s = m.hir.module().functions.empty()
                 ? SourceSpan{SourceSnapshotId{0}, 0, 0}
                 : m.hir.module().functions[0].source.span;
    need(m.hir.config().backend == hir::Backend::native &&
             m.hir.config().word_bits == 64,
         "native IR requires native64", s);
    for (auto &f : m.hir.module().functions)
      functions.emplace(f.symbol, f.signature);
    need(functions.size() == m.functions.size(),
         "native function set differs from HIR", s);
    std::set<SymbolId> seen;
    for (const auto &f : m.functions) {
      need(seen.insert(f.symbol).second, "duplicate native function", f.source);
      function(f);
    }
  }
};
} // namespace
PhaseResult<VerifiedModule> verify(const Module &m) {
  PhaseResult<VerifiedModule> result;
  try {
    auto owned = std::make_shared<const Module>(m);
    Verifier(*owned).run();
    result.value = VerifiedModule(owned);
    result.state = PhaseState::complete;
  } catch (const Bad &b) {
    result.diagnostics.push_back({"E_IR_INVALID",
                                  "lowering",
                                  "error",
                                  b.message,
                                  b.span,
                                  {},
                                  {},
                                  {},
                                  {},
                                  {}});
  } catch (const std::exception &e) {
    result.diagnostics.push_back({"E_IR_INVALID",
                                  "lowering",
                                  "error",
                                  e.what(),
                                  {},
                                  {},
                                  {},
                                  {},
                                  {},
                                  {}});
  }
  return result;
}
std::string dump(const Module &m) {
  std::ostringstream out;
  out << "native-scalars compiler-dev/1 native64\n";
  for (const auto &f : m.functions) {
    out << "function @" << f.symbol.value << " type=" << f.signature.value
        << '\n';
    for (size_t s = 0; s < f.slots.size(); ++s) {
      const auto &slot = f.slots[s];
      out << " slot $" << s << " type=" << slot.type.value;
      if (slot.parameter)
        out << " parameter=" << *slot.parameter << " inout=" << slot.inout;
      out << '\n';
    }
    for (const auto &handler : f.handlers)
      out << " handler try=" << handler.target.try_node.value << ':'
          << handler.target.index << " entry=b" << handler.protected_entry.value
          << " block=b" << handler.block.value << " slot=$"
          << handler.slot.value << '\n';
    for (size_t b = 0; b < f.blocks.size(); ++b) {
      out << " b" << b << ":";
      for (auto p : f.blocks[b].parameters)
        out << " %" << p.value.value << ":" << p.type.value;
      out << '\n';
      for (auto &i : f.blocks[b].instructions) {
        out << "  ";
        if (i.result)
          out << '%' << i.result->value << " = ";
        static constexpr const char *names[] = {
            "constant",      "function",      "load",
            "address",       "store",         "unary",
            "binary",        "convert",       "call",
            "struct_init",   "enum_init",     "field_read",
            "field_address", "load_indirect", "store_indirect",
            "enum_test",     "enum_payload",  "string_literal",
            "exit_begin",    "cleanup_begin", "cleanup_end",
            "copy", "move", "take", "destroy", "borrow", "end_borrow", "save_output",
            "vec_init", "vec_read", "vec_set", "vec_slice", "iteration_begin", "iteration_end", "vec_length", "iteration_test", "iteration_read", "iteration_next", "ownership_begin", "ownership_end", "replace_indirect", "begin_borrow", "vec_check", "vec_element_address", "system_call",
            "foreign_call", "c_field_read", "c_ptr_read", "c_ptr_write", "c_buffer_read", "c_buffer_write",
            "trait_object", "trait_call"};
        static_assert(sizeof(names) / sizeof(names[0]) ==
                      static_cast<unsigned>(Op::trait_call) + 1);
        out << names[static_cast<unsigned>(i.op)] << " type=" << i.type.value;
        if (i.op == Op::constant)
          out << " bits=" << i.bits;
        if (i.op == Op::string_literal) {
          static constexpr char hex[] = "0123456789abcdef";
          out << " bytes=" << i.utf8.size() << " utf8=";
          for (unsigned char c : i.utf8)
            out << hex[c >> 4] << hex[c & 15];
        }
        if (i.op == Op::function)
          out << " @" << i.function.value;
        if (i.op == Op::load || i.op == Op::store || i.op == Op::address)
          out << " $" << i.slot.value;
        if (i.op == Op::unary)
          out << " op=" << static_cast<unsigned>(i.unary);
        if (i.op == Op::binary)
          out << " op=" << static_cast<unsigned>(i.binary);
        if (i.op == Op::call || i.op == Op::system_call ||
            i.op == Op::foreign_call)
          out << " signature=" << i.signature.value;
        if (i.c_callable) out << " c_callable=" << *i.c_callable;
        if (i.c_type) out << " c_type=" << i.c_type;
        if (i.op == Op::c_field_read) out << " c_field=" << i.c_field;
        if (i.system)
          out << " system=" << static_cast<unsigned>(*i.system);
        if (i.op == Op::field_read || i.op == Op::field_address ||
            i.op == Op::enum_payload)
          out << " field=@" << i.field.value;
        if (i.op == Op::enum_init || i.op == Op::enum_test ||
            i.op == Op::enum_payload)
          out << " variant=@" << i.variant.value;
        if (i.origin)
          out << " origin=" << i.origin->value;
        if (i.exit)
          out << " origin=" << i.exit->origin.value
              << " exit=" << int(i.exit->kind);
        if (i.cleanup)
          out << " defer=" << i.cleanup->value;
        for (auto a : i.inputs)
          out << " %" << a.value;
        out << " source=" << i.source.snapshot.value << ':'
            << i.source.begin_byte << '-' << i.source.end_byte << '\n';
      }
      const auto &t = f.blocks[b].terminator;
      static constexpr const char *exits[] = {
          "missing",     "branch",   "conditional", "return",
          "unreachable", "try_call", "error_return", "await", "cancel_return"};
      out << "  " << exits[static_cast<unsigned>(t.kind)];
      if (t.kind == Exit::try_call)
        out << " signature=" << t.signature.value;
      if (t.origin)
        out << " origin=" << t.origin->value;
      for (auto v : t.inputs)
        out << " %" << v.value;
      for (auto d : t.targets)
        out << " b" << d.value;
      out << '\n';
    }
  }
  return out.str();
}
} // namespace lang::ir
