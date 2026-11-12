#include "lang/ir/native.hpp"
#include "layout.hpp"
#include <map>
#include <stdexcept>
#include <type_traits>

namespace lang::ir {
namespace {
bool uses_slot(Op op) {
  switch (op) {
  case Op::load:
  case Op::address:
  case Op::store:
  case Op::move:
  case Op::take:
  case Op::destroy:
  case Op::begin_borrow:
  case Op::iteration_begin:
  case Op::iteration_end:
  case Op::iteration_test:
  case Op::iteration_read:
  case Op::iteration_next:
    return true;
  default:
    return false;
  }
}
class Lower {
  const hir::HirModule &h;
  detail::AggregateTypes aggregates;
  Function f;
  BlockId current;
  uint64_t next_value = 0;
  std::map<SymbolId, LocalSlotId> slots;
  std::map<HirNodeId, std::pair<BlockId, BlockId>> loops;
  TypeId unit, boolean;
  std::map<HirNodeId, LocalSlotId> temporaries, iterations;
  bool owning(TypeId t) const { return hir::classify_value(h, t).needs_drop; }
  void effect(Op op, LocalSlotId slot, SourceSpan source,
              std::vector<ValueId> inputs = {},
              std::optional<hir::OwnerRef> owner = {},
              std::optional<HirNodeId> origin = {}) {
    Instruction i; i.op = op; i.type = unit; i.slot = slot;
    i.inputs = std::move(inputs); i.source = source; i.owner = owner;
    i.origin = origin;
    f.blocks[current.value].instructions.push_back(std::move(i));
  }
  LocalSlotId owner_slot(hir::OwnerRef ref) const {
    if (ref.kind == hir::OwnerKind::binding) return slots.at(SymbolId{ref.identity});
    if (ref.kind == hir::OwnerKind::iteration) return iterations.at(HirNodeId{ref.identity});
    return temporaries.at(HirNodeId{ref.identity});
  }
  void begin_borrow(hir::OwnerRef owner, ValueId value, SourceSpan source) {
    effect(Op::begin_borrow, owner_slot(owner), source, {value}, owner);
  }
  void take(HirNodeId id) {
    if (auto it = temporaries.find(id); it != temporaries.end())
      effect(Op::take, it->second, node(id).source.span);
  }
  void actions(const std::vector<hir::CleanupStep> &steps, SourceSpan source,
               std::optional<ValueId> saved = {}) {
    for (const auto &a : steps) {
      if (!live()) break;
      if (a.kind == hir::CleanupKind::defer_) {
        HirNodeId d{a.owner.identity};
        marker(Op::cleanup_begin, {}, d);
        auto outer_slots = slots; auto outer_loops = loops;
        auto outer_handlers = handlers; auto outer_temps = temporaries;
        auto outer_iterations = iterations;
        stmt(node(d).children[0]);
        slots = std::move(outer_slots); loops = std::move(outer_loops);
        handlers = std::move(outer_handlers); temporaries = std::move(outer_temps);
        iterations = std::move(outer_iterations);
        if (live()) marker(Op::cleanup_end, {}, d);
      } else if (a.kind == hir::CleanupKind::destroy_if_live) {
        effect(Op::destroy, owner_slot(a.owner), source, {}, a.owner);
      } else if (a.kind == hir::CleanupKind::end_iteration) {
        effect(Op::iteration_end, owner_slot(a.owner), source, {}, a.owner);
      } else if (a.kind == hir::CleanupKind::end_borrow) {
        effect(Op::end_borrow, LocalSlotId{0}, source, {}, a.owner);
      } else if (a.kind == hir::CleanupKind::save_output) {
        effect(Op::save_output, LocalSlotId{0}, source, {*saved}, a.owner);
      }
    }
  }
  void batch(HirNodeId id, uint64_t kind, const std::vector<hir::CleanupStep> &steps) {
    Instruction i; i.op = Op::ownership_begin; i.type = unit; i.origin = id;
    i.bits = kind; i.source = node(id).source.span;
    f.blocks[current.value].instructions.push_back(i);
    actions(steps, i.source);
    i.op = Op::ownership_end; f.blocks[current.value].instructions.push_back(i);
  }
  void full(HirNodeId id) {
    for (const auto &fe : h.ownership.full_expressions)
      if (fe.node == id) { batch(id, 0, fe.actions); return; }
  }
  void call_end(HirNodeId id) {
    for (const auto &c : h.ownership.calls)
      if (c.node == id) { batch(id, 1, c.on_return); return; }
  }
  std::map<std::pair<HirNodeId, uint32_t>, Handler> handlers;
  const hir::ExitPlan *plan(const ExitSite &site) const {
    for (const auto &p : node(site.origin).exits)
      if (p.kind == site.kind && p.error_type == site.error_type)
        return &p;
    return nullptr;
  }
  void marker(Op op, std::optional<ExitSite> site,
              std::optional<HirNodeId> cleanup,
              std::vector<ValueId> inputs = {}) {
    Instruction i;
    i.op = op;
    i.type = unit;
    i.exit = site;
    i.cleanup = cleanup;
    i.inputs = std::move(inputs);
    i.source = node(site ? site->origin : *cleanup).source.span;
    f.blocks[current.value].instructions.push_back(std::move(i));
  }
  void clean(const ExitSite &site, std::optional<ValueId> saved = {}) {
    if (const auto *p = plan(site)) {
      marker(Op::exit_begin, site, {},
             saved ? std::vector{*saved} : std::vector<ValueId>{});
      if (!p->actions.empty()) actions(p->actions, node(site.origin).source.span, saved);
      else {
        std::vector<hir::CleanupStep> defers;
        for (auto d : p->defers) defers.push_back({hir::CleanupKind::defer_,
          {hir::OwnerKind::temporary, d.value}, TypeId{0}});
        actions(defers, node(site.origin).source.span, saved);
      }
    }
  }
  void finish(const ExitSite &site) {
    auto &t = f.blocks[current.value].terminator;
    t.origin = site.origin;
    if (plan(site))
      t.exit = site;
  }
  void error(HirNodeId id, TypeId type, ValueId value) {
    ExitSite site{id, hir::ExitKind::error, type};
    const auto *p = plan(site);
    if (!p)
      throw std::logic_error("missing verified error exit");
    clean(site, value);
    if (auto c = std::get_if<hir::CatchTarget>(&p->target)) {
      const auto &handler = handlers.at({c->try_node, c->index});
      store(handler.slot, value, node(id).source.span);
      jump(handler.block, node(id).source.span);
    } else {
      end(Exit::error_return, {value}, {}, node(id).source.span);
    }
    finish(site);
  }
  const hir::HirNode &node(HirNodeId id) const { return h.nodes.at(id.value); }
  const hir::Type &ty(HirNodeId id) const { return *h.types.find(node(id).type); }
  bool live() const {
    return f.blocks[current.value].terminator.kind == Exit::missing;
  }
  BlockId block() {
    BlockId id{f.blocks.size()};
    f.blocks.emplace_back();
    return id;
  }
  void end(Exit k, std::vector<ValueId> values, std::vector<BlockId> targets,
           SourceSpan s) {
    f.blocks[current.value].terminator = {k, std::move(values),
                                          std::move(targets), s};
  }
  void jump(BlockId target, SourceSpan s) {
    end(Exit::branch, {}, {target}, s);
  }
  ValueId emit(Instruction i) {
    ValueId id{next_value++};
    i.result = id;
    f.blocks[current.value].instructions.push_back(std::move(i));
    return id;
  }
  ValueId constant(TypeId t, uint64_t bits, SourceSpan s) {
    Instruction i;
    i.type = t;
    i.bits = bits;
    i.source = s;
    return emit(i);
  }
  LocalSlotId slot(TypeId type) {
    LocalSlotId id{f.slots.size()};
    f.slots.push_back({type, {}, false});
    return id;
  }
  void store(LocalSlotId to, ValueId value, SourceSpan s) {
    Instruction i;
    i.op = Op::store;
    i.type = unit;
    i.slot = to;
    i.inputs = {value};
    i.source = s;
    f.blocks[current.value].instructions.push_back(i);
  }
  ValueId access(Op op, LocalSlotId slot_id, SourceSpan s) {
    Instruction i;
    i.op = op;
    i.type = f.slots[slot_id.value].type;
    i.slot = slot_id;
    i.source = s;
    return emit(i);
  }
  ValueId indirect(ValueId address, TypeId type, SourceSpan s) {
    Instruction i;
    i.op = Op::load_indirect;
    i.type = type;
    i.inputs = {address};
    i.source = s;
    return emit(i);
  }
  using Extracted = std::map<const hir::Pattern *, ValueId>;
  void match_pattern(const hir::Pattern &p, ValueId value, BlockId failure,
                     Extracted &extracted) {
    if (p.literal) {
      ValueId expected;
      std::visit(
          [&](const auto &literal) {
            using T = std::decay_t<decltype(literal)>;
            if constexpr (std::is_same_v<T, hir::StringLiteral>) {
              Instruction i;
              i.op = Op::string_literal;
              i.type = p.type;
              i.utf8 = literal.utf8;
              i.source = p.source.span;
              expected = emit(std::move(i));
            } else if constexpr (std::is_same_v<T, hir::BoolLiteral>) {
              expected = constant(p.type, literal.value, p.source.span);
            } else if constexpr (std::is_same_v<T, hir::IntegerLiteral>) {
              expected = constant(p.type, literal.bits, p.source.span);
            } else {
              expected = constant(p.type, literal.bits, p.source.span);
            }
          },
          *p.literal);
      auto matched = block();
      Instruction test;
      test.op = Op::binary;
      test.binary = hir::BinaryOp::equal;
      test.type = boolean;
      test.inputs = {value, expected};
      test.source = p.source.span;
      end(Exit::conditional, {emit(test)}, {matched, failure}, p.source.span);
      current = matched;
      return;
    }
    if (!p.variant)
      return;
    auto matched = block();
    Instruction test;
    test.op = Op::enum_test;
    test.type = boolean;
    test.variant = *p.variant;
    test.inputs = {value};
    test.source = p.source.span;
    end(Exit::conditional, {emit(test)}, {matched, failure}, p.source.span);
    current = matched;
    auto fields = aggregates.fields(p.type, *p.variant);
    for (size_t a = 0; a < p.payload.size(); ++a) {
      const auto &child = p.payload[a];
      // A wildcard payload with no descendant test or binding needs no
      // active read. A literal must be tested, so it is not skipped.
      if (!child.variant && !child.binding && !child.literal)
        continue;
      Instruction payload;
      payload.op = Op::enum_payload;
      payload.type = child.type;
      payload.variant = *p.variant;
      payload.field = fields[a].symbol;
      payload.inputs = {value};
      payload.source = child.source.span;
      const auto child_value = emit(payload);
      extracted[&child] = child_value;
      match_pattern(child, child_value, failure, extracted);
    }
  }
  void bind_pattern(const hir::Pattern &p, ValueId value,
                    const Extracted &extracted) {
    if (p.binding) {
      auto local = slot(p.type);
      slots.emplace(*p.binding, local);
      f.slots[local.value].binding = p.binding;
      if (owning(p.type)) {
        f.slots[local.value].owner = hir::OwnerRef{hir::OwnerKind::binding, p.binding->value};
        Instruction copy; copy.op = Op::copy; copy.type = p.type;
        copy.inputs = {value}; copy.source = p.source.span;
        value = emit(copy);
      }
      store(local, value, p.source.span);
    }
    if (!p.variant)
      return;
    for (const auto &child : p.payload) {
      // A wildcard payload needs no deeper read. A binding, nested variant,
      // or literal uses the exact active-payload value already tested by
      // match_pattern; re-reading here would create a different SSA value
      // without the matching-variant fact required by the verifier.
      if (!child.variant && !child.binding && !child.literal)
        continue;
      auto found = extracted.find(&child);
      if (found == extracted.end())
        throw std::logic_error("pattern payload was not extracted");
      bind_pattern(child, found->second, extracted);
    }
  }
  ValueId expr(HirNodeId id) {
    auto value = raw_expr(id);
    for (const auto &t : h.ownership.temporaries)
      if (t.producer == id) {
        auto storage = slot(t.type);
        f.slots[storage.value].owner = hir::OwnerRef{hir::OwnerKind::temporary, id.value};
        temporaries[id] = storage;
        store(storage, value, node(id).source.span);
        break;
      }
    return value;
  }
  ValueId constant_value(const hir::ConstantValue &value, SourceSpan source,
                         std::optional<HirNodeId> origin = {}) {
    Instruction i;
    i.type = value.type;
    i.source = source;
    i.origin = origin;
    std::visit(
        [&](const auto &v) {
          using T = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<T, hir::StringLiteral>) {
            i.op = Op::string_literal;
            i.utf8 = v.utf8;
          } else if constexpr (std::is_same_v<T, hir::FunctionValue>) {
            i.op = Op::function;
            i.function = v.symbol;
          } else if constexpr (std::is_same_v<T, hir::ConstantAggregate>) {
            i.op = v.variant ? Op::enum_init : Op::struct_init;
            if (v.variant)
              i.variant = *v.variant;
            for (const auto &field : v.fields)
              i.inputs.push_back(constant_value(field, source));
          } else {
            i.op = Op::constant;
            if constexpr (std::is_same_v<T, hir::BoolLiteral>)
              i.bits = v.value;
            else if constexpr (!std::is_same_v<T, hir::UnitLiteral>)
              i.bits = v.bits;
          }
        },
        value.data);
    return emit(i);
  }
  ValueId raw_expr(HirNodeId id) {
    const auto &n = node(id);
    Instruction i;
    i.type = n.type;
    i.source = n.source.span;
    switch (n.kind()) {
    case hir::NodeKind::constant_literal:
      return constant_value(std::get<hir::ConstantLiteral>(n.data).value,
                            i.source, id);
    case hir::NodeKind::unit_literal:
      return constant(n.type, 0, i.source);
    case hir::NodeKind::bool_literal:
      return constant(n.type, std::get<hir::BoolLiteral>(n.data).value,
                      i.source);
    case hir::NodeKind::integer_literal:
      return constant(n.type, std::get<hir::IntegerLiteral>(n.data).bits,
                      i.source);
    case hir::NodeKind::float_literal:
      return constant(n.type, std::get<hir::FloatLiteral>(n.data).bits,
                      i.source);
    case hir::NodeKind::string_literal:
      i.op = Op::string_literal;
      i.utf8 = std::get<hir::StringLiteral>(n.data).utf8;
      return emit(i);
    case hir::NodeKind::read: {
      auto value = access(Op::load, slots.at(std::get<hir::Read>(n.data).symbol), i.source);
      for (const auto &u : h.ownership.uses)
        if (u.consumer == id && u.operand == 0 && !u.destination) {
          i.op = u.kind == hir::UseKind::copy ? Op::copy : Op::borrow;
          i.inputs = {value}; i.origin = id;
          return emit(i);
        }
      return value;
    }
    case hir::NodeKind::move: {
      i.op = Op::move; i.origin = id;
      if (auto binding = std::get<hir::Move>(n.data).binding) {
        i.slot = slots.at(*binding);
        return emit(i);
      }
      auto value = expr(n.children[0]); take(n.children[0]);
      return value;
    }
    case hir::NodeKind::vec_init:
      i.op = Op::vec_init;
      for (auto child : n.children) i.inputs.push_back(expr(child));
      for (auto child : n.children) take(child);
      return emit(i);
    case hir::NodeKind::index_read: {
      i.origin = id;
      const auto receiver = ty(n.children[0]).kind;
      i.op = receiver == hir::TypeKind::CForeignPointer ? Op::c_ptr_read
              : receiver == hir::TypeKind::CForeignBuffer ? Op::c_buffer_read
                                                          : Op::vec_read;
      i.inputs = {expr(n.children[0]), expr(n.children[1])};
      return emit(i);
    }
    case hir::NodeKind::c_field_read: {
      i.origin = id;
      i.op = Op::c_field_read;
      const auto &f = std::get<hir::CFieldRead>(n.data);
      i.c_type = f.c_type;
      i.c_field = f.field;
      i.inputs = {expr(n.children[0])};
      return emit(i);
    }
    case hir::NodeKind::vec_slice:
      i.origin = id;
      i.op = Op::vec_slice;
      for (auto child : n.children) i.inputs.push_back(expr(child));
      i.bits = (std::get<hir::VecSlice>(n.data).start ? 1 : 0) |
               (std::get<hir::VecSlice>(n.data).end ? 2 : 0);
      return emit(i);
    case hir::NodeKind::place:
      return access(Op::address, slots.at(std::get<hir::Place>(n.data).symbol),
                    i.source);
    case hir::NodeKind::function_value:
      i.op = Op::function;
      i.function = std::get<hir::FunctionValue>(n.data).symbol;
      return emit(i);
    case hir::NodeKind::struct_init:
    case hir::NodeKind::enum_init: {
      const bool structure = n.kind() == hir::NodeKind::struct_init;
      i.op = structure ? Op::struct_init : Op::enum_init;
      const auto &args = structure ? std::get<hir::StructInit>(n.data).fields
                                   : std::get<hir::EnumInit>(n.data).payload;
      if (!structure)
        i.variant = std::get<hir::EnumInit>(n.data).variant;
      i.inputs.resize(args.size());
      for (const auto &a : args)
        i.inputs[a.parameter_index] = expr(a.expression);
      for (const auto &a : args) take(a.expression);
      return emit(i);
    }
    case hir::NodeKind::field_read:
    case hir::NodeKind::field_place:
      i.op = n.kind() == hir::NodeKind::field_read ? Op::field_read
                                                   : Op::field_address;
      i.field = n.kind() == hir::NodeKind::field_read
                    ? std::get<hir::FieldRead>(n.data).field
                    : std::get<hir::FieldPlace>(n.data).field;
      i.inputs = {expr(n.children[0])};
      { auto value = emit(i);
        bool own = false;
        for (const auto &t : h.ownership.temporaries) if (t.producer == id) own = true;
        if (own) { i.op = Op::copy; i.inputs = {value}; return emit(i); }
        return value;
      }
    case hir::NodeKind::unary:
      i.op = Op::unary;
      i.unary = std::get<hir::Unary>(n.data).op;
      i.inputs = {expr(n.children[0])};
      return emit(i);
    case hir::NodeKind::binary: {
      i.origin = id;
      i.op = Op::binary;
      i.binary = std::get<hir::Binary>(n.data).op;
      auto lhs = expr(n.children[0]);
      auto rhs = expr(n.children[1]);
      i.inputs = {lhs, rhs};
      return emit(i);
    }
    case hir::NodeKind::integer_convert:
      i.op = Op::convert;
      i.origin = id;
      i.inputs = {expr(n.children[0])};
      return emit(i);
    case hir::NodeKind::short_circuit: {
      auto lhs = expr(n.children[0]);
      auto result = slot(boolean);
      store(result, lhs, i.source);
      auto rhs = block(), join = block();
      bool and_ =
          std::get<hir::ShortCircuit>(n.data).op == hir::LogicalOp::and_;
      end(Exit::conditional, {lhs},
          and_ ? std::vector{rhs, join} : std::vector{join, rhs}, i.source);
      current = rhs;
      store(result, expr(n.children[1]), i.source);
      jump(join, i.source);
      current = join;
      return access(Op::load, result, i.source);
    }
    case hir::NodeKind::call: {
      const auto &c = std::get<hir::Call>(n.data);
      const bool system = c.kind == hir::CalleeKind::system;
      const bool c_foreign = c.kind == hir::CalleeKind::c_foreign;
      i.op = system ? Op::system_call : c_foreign ? Op::foreign_call : Op::call;
      i.system = c.system;
      i.c_callable = c.c_callable;
      i.origin = id;
      i.signature = c.signature;
      ValueId callee;
      if (system) {
        // Fixed typed operation; there is no source-level function pointer.
      } else if (c_foreign) {
        // C calls have no source-level callee value. The adapter identity is
        // an immutable binding index checked by both verifiers.
      } else if (c.callee)
        callee = expr(*c.callee);
      else {
        Instruction fn;
        fn.op = Op::function;
        fn.type = c.signature;
        fn.function = *c.direct_symbol;
        fn.source = i.source;
        callee = emit(fn);
      }
      const size_t argument_offset = system || c_foreign ? 0 : 1;
      i.inputs.resize(c.args.size() + argument_offset);
      if (!system && !c_foreign) i.inputs[0] = callee;
      // Evaluation follows source order. Only the saved values are reordered.
      for (const auto &arg : c.args) {
        auto value = expr(arg.expression);
        i.inputs[arg.parameter_index + argument_offset] = value;
        bool lifetime = false;
        for (const auto &call : h.ownership.calls) if (call.node == id) lifetime = true;
        if (lifetime && arg.mode != hir::ParamMode::move) {
          size_t offset = c.callee ? 1 : 0;
          for (const auto &u : h.ownership.uses)
            if (u.consumer == id && !u.destination && u.operand == arg.source_index + offset &&
                (u.kind == hir::UseKind::borrow || u.kind == hir::UseKind::inout)) {
              if (u.root) begin_borrow({hir::OwnerKind::binding, u.root->value}, value, i.source);
              else if (owning(u.type)) begin_borrow({hir::OwnerKind::temporary, arg.expression.value}, value, i.source);
              break;
            }
        }
      }
      const auto &signature = *h.types.find(c.signature);
      for (const auto &a : c.args) if (a.mode == hir::ParamMode::move) take(a.expression);
      if (!signature.throws_type) {
        auto result = emit(i); call_end(id); return result;
      }
      auto ok = block(), fail = block();
      ValueId value{next_value++}, error_value{next_value++};
      f.blocks[ok.value].parameters.push_back({value, n.type, id, false});
      f.blocks[fail.value].parameters.push_back(
          {error_value, *signature.throws_type, id, true});
      end(Exit::try_call, i.inputs, {ok, fail}, i.source);
      f.blocks[current.value].terminator.signature = c.signature;
      f.blocks[current.value].terminator.origin = id;
      current = fail;
      error(id, *signature.throws_type, error_value);
      current = ok;
      call_end(id);
      return value;
    }
    default:
      throw std::logic_error("non-expression in native expression lowering");
    }
  }
  void stmt(HirNodeId id) {
    if (!live())
      return;
    const auto &n = node(id);
    auto s = n.source.span;
    switch (n.kind()) {
    case hir::NodeKind::block: {
      for (auto child : n.children)
        stmt(child);
      ExitSite site{id, hir::ExitKind::normal, {}};
      if (live() && plan(site)) {
        clean(site);
        auto next = block();
        jump(next, s);
        finish(site);
        current = next;
      }
      break;
    }
    case hir::NodeKind::defer_:
      // Registration is represented by the VerifiedHIR plan at each exit.
      break;
    case hir::NodeKind::throw_: {
      auto value = expr(n.children[0]);
      take(n.children[0]);
      error(id, node(n.children[0]).type, value);
      break;
    }
    case hir::NodeKind::try_: {
      const auto &t = std::get<hir::Try>(n.data);
      auto protected_entry = block(), after = block();
      jump(protected_entry, s);
      f.blocks[current.value].terminator.origin = id;
      current = protected_entry;
      for (uint32_t c = 0; c < t.catches.size(); ++c) {
        const auto &catch_ = t.catches[c];
        Handler handler{
            {id, c}, block(), slot(catch_.error_type), protected_entry};
        handlers.emplace(std::pair{id, c}, handler);
        f.handlers.push_back(handler);
        slots.emplace(catch_.binding, handler.slot);
        f.slots[handler.slot.value].binding = catch_.binding;
        if (owning(catch_.error_type)) f.slots[handler.slot.value].owner =
          hir::OwnerRef{hir::OwnerKind::binding, catch_.binding.value};
      }
      stmt(n.children[0]);
      if (live())
        jump(after, s);
      for (uint32_t c = 0; c < t.catches.size(); ++c) {
        current = handlers.at({id, c}).block;
        stmt(t.catches[c].body);
        if (live())
          jump(after, s);
      }
      current = after;
      break;
    }
    case hir::NodeKind::noop:
      break;
    case hir::NodeKind::expression_statement:
      (void)expr(n.children[0]);
      full(id);
      break;
    case hir::NodeKind::binding: {
      auto initial = expr(n.children[0]);
      auto target = slot(node(n.children[0]).type);
      auto symbol = std::get<hir::Binding>(n.data).symbol;
      slots.emplace(symbol, target);
      f.slots[target.value].binding = symbol;
      if (owning(node(n.children[0]).type)) f.slots[target.value].owner =
          hir::OwnerRef{hir::OwnerKind::binding, symbol.value};
      take(n.children[0]);
      store(target, initial, s);
      full(id);
      break;
    }
    case hir::NodeKind::assign: {
      const auto &place = node(n.children[0]);
      auto indexed_field = [&]() -> std::optional<HirNodeId> {
        auto base = n.children[0];
        while (node(base).kind() == hir::NodeKind::field_place)
          base = node(base).children[0];
        if (node(base).kind() == hir::NodeKind::index_place)
          return base;
        return {};
      };
      auto indexed_place = indexed_field();
      if (place.kind() == hir::NodeKind::field_place && indexed_place) {
        // The receiver/index are evaluated once in source order. The bounds
        // check belongs to the assignment target and precedes the RHS.
        auto index_place = *indexed_place;
        const auto &indexed = node(index_place);
        auto receiver = expr(indexed.children[0]);
        auto index = expr(indexed.children[1]);
        effect(Op::vec_check, LocalSlotId{0}, s, {receiver, index}, {},
               index_place);
        Instruction element;
        element.op = Op::vec_element_address;
        element.type = indexed.type;
        element.origin = index_place;
        element.inputs = {receiver, index};
        element.source = indexed.source.span;
        auto target = emit(element);
        std::vector<HirNodeId> fields;
        for (auto field = n.children[0];
             node(field).kind() == hir::NodeKind::field_place;
             field = node(field).children[0])
          fields.push_back(field);
        for (auto it = fields.rbegin(); it != fields.rend(); ++it) {
          const auto &field = node(*it);
          Instruction address;
          address.op = Op::field_address;
          address.type = field.type;
          address.field = std::get<hir::FieldPlace>(field.data).field;
          address.inputs = {target};
          address.source = field.source.span;
          target = emit(address);
        }
        auto compound = std::get<hir::Assign>(n.data).compound;
        std::optional<ValueId> lhs;
        if (compound)
          lhs = indirect(target, place.type, s);
        auto rhs = expr(n.children[1]);
        if (compound) {
          Instruction op;
          op.op = Op::binary;
          op.type = place.type;
          op.inputs = {*lhs, rhs};
          op.binary = *compound;
          op.source = s;
          rhs = emit(op);
        }
        take(n.children[1]);
        Instruction write;
        write.op = owning(place.type) ? Op::replace_indirect : Op::store_indirect;
        write.type = unit;
        write.inputs = {target, rhs};
        write.source = s;
        f.blocks[current.value].instructions.push_back(write);
        full(id);
        break;
      }
      if (place.kind() == hir::NodeKind::index_place) {
        auto receiver = expr(place.children[0]); auto index = expr(place.children[1]);
        const auto receiver_kind = ty(place.children[0]).kind;
        if (receiver_kind == hir::TypeKind::CForeignPointer ||
            receiver_kind == hir::TypeKind::CForeignBuffer) {
          const bool buffer = receiver_kind == hir::TypeKind::CForeignBuffer;
          auto compound = std::get<hir::Assign>(n.data).compound;
          std::optional<ValueId> lhs;
          if (compound) {
            Instruction read; read.op = buffer ? Op::c_buffer_read
                                               : Op::c_ptr_read;
            read.type = place.type; read.inputs = {receiver, index};
            read.origin = n.children[0];
            read.source = s; lhs = emit(read);
          }
          auto rhs = expr(n.children[1]);
          if (compound) {
            Instruction op; op.op = Op::binary; op.type = place.type;
            op.inputs = {*lhs, rhs}; op.binary = *compound; op.source = s;
            rhs = emit(op);
          }
          take(n.children[1]);
          Instruction write;
          write.op = buffer ? Op::c_buffer_write : Op::c_ptr_write;
          write.type = unit;
          write.origin = n.children[0];
          write.inputs = {receiver, index, rhs};
          write.source = s;
          f.blocks[current.value].instructions.push_back(std::move(write));
          full(id); break;
        }
        effect(Op::vec_check, LocalSlotId{0}, s, {receiver, index}, {},
               n.children[0]);
        auto compound = std::get<hir::Assign>(n.data).compound;
        std::optional<ValueId> lhs;
        if (compound) {
          Instruction read; read.op = Op::vec_read; read.type = place.type;
          read.origin = n.children[0];
          read.inputs = {receiver, index}; read.source = s;
          lhs = emit(read);
        }
        auto rhs = expr(n.children[1]);
        if (compound) {
          Instruction op; op.op = Op::binary; op.type = place.type;
          op.inputs = {*lhs, rhs}; op.binary = *compound; op.source = s;
          rhs = emit(op);
        }
        take(n.children[1]);
        effect(Op::vec_set, LocalSlotId{0}, s, {receiver, index, rhs}, {},
               n.children[0]);
        full(id); break;
      }
      auto target = expr(n.children[0]);
      if (place.kind() == hir::NodeKind::field_place)
        for (const auto &fe : h.ownership.full_expressions) if (fe.node == id)
          for (const auto &a : fe.actions) if (a.kind == hir::CleanupKind::end_borrow)
            begin_borrow(a.owner, target, s);
      auto compound = std::get<hir::Assign>(n.data).compound;
      std::optional<ValueId> lhs;
      if (compound)
        lhs = indirect(target, place.type, s);
      auto rhs = expr(n.children[1]);
      if (compound) {
        Instruction i;
        i.op = Op::binary;
        i.type = place.type;
        i.binary = *compound;
        i.origin = id;
        i.inputs = {*lhs, rhs};
        i.source = s;
        rhs = emit(i);
      }
      if (!compound || h.types.find(place.type)->kind != hir::TypeKind::String)
        take(n.children[1]);
      if (auto binding = std::get_if<hir::Place>(&place.data); binding && owning(place.type))
        effect(Op::destroy, slots.at(binding->symbol), s);
      Instruction write;
      write.op = owning(place.type) ? Op::replace_indirect : Op::store_indirect;
      write.type = unit;
      write.inputs = {target, rhs};
      write.source = s;
      if (auto binding = std::get_if<hir::Place>(&place.data); binding && owning(place.type))
        store(slots.at(binding->symbol), rhs, s);
      else
        f.blocks[current.value].instructions.push_back(write);
      full(id);
      break;
    }
    case hir::NodeKind::switch_: {
      const auto subject = n.children[0];
      const bool computed_owning =
          owning(node(subject).type) &&
          node(subject).kind() != hir::NodeKind::read;
      ValueId value;
      if (computed_owning) {
        // Evaluate the subject once. expr() installs the real temporary
        // slot; the switch full expression cleans only the other live
        // temporaries of the subject expression, never the result.
        value = expr(subject);
        full(id);
      } else if (owning(node(subject).type)) {
        auto symbol = std::get<hir::Read>(node(subject).data).symbol;
        value = access(Op::load, slots.at(symbol), s);
        begin_borrow({hir::OwnerKind::binding, symbol.value}, value, s);
      } else {
        value = expr(subject);
        full(subject);
      }
      auto after = block();
      loops.emplace(id, std::pair{after, after});
      for (const auto &arm : std::get<hir::Switch>(n.data).arms) {
        auto next = block();
        Extracted extracted;
        // A failed pattern keeps the same subject value and slot for the
        // next arm; only a complete match copies payload bindings.
        match_pattern(arm.pattern, value, next, extracted);
        bind_pattern(arm.pattern, value, extracted);
        if (!computed_owning)
          full(id);
        stmt(arm.body);
        if (live())
          jump(after, s);
        current = next;
      }
      end(Exit::unreachable, {}, {}, s);
      loops.erase(id);
      current = after;
      break;
    }
    case hir::NodeKind::if_: {
      auto condition = expr(n.children[0]);
      full(n.children[0]);
      auto yes = block(), no = block(), join = block();
      end(Exit::conditional, {condition}, {yes, no}, s);
      current = yes;
      stmt(n.children[1]);
      if (live())
        jump(join, s);
      current = no;
      if (n.children.size() == 3)
        stmt(n.children[2]);
      if (live())
        jump(join, s);
      current = join;
      break;
    }
    case hir::NodeKind::for_: {
      stmt(n.children[0]);
      auto condition = block(), body = block(), step = block(), after = block();
      jump(condition, s);
      loops.emplace(id, std::pair{after, step});
      current = condition;
      auto test = expr(n.children[1]);
      full(n.children[1]);
      end(Exit::conditional, {test}, {body, after}, s);
      current = body;
      stmt(n.children[3]);
      if (live())
        jump(step, s);
      current = step;
      stmt(n.children[2]);
      if (live())
        jump(condition, s);
      loops.erase(id);
      current = after;
      ExitSite site{id, hir::ExitKind::normal, {}};
      if (plan(site)) { clean(site); auto next = block(); jump(next, s); finish(site); current = next; }
      break;
    }
    case hir::NodeKind::for_in: {
      const auto &fi = std::get<hir::ForIn>(n.data);
      auto collection = expr(n.children[0]); take(n.children[0]);
      auto hidden = slot(node(n.children[0]).type);
      iterations[id] = hidden;
      f.slots[hidden.value].owner = hir::OwnerRef{hir::OwnerKind::iteration, id.value};
      store(hidden, collection, s); full(id);
      effect(Op::iteration_begin, hidden, s);
      auto condition = block(), body = block(), step = block(), done = block(), after = block();
      jump(condition, s); loops.emplace(id, std::pair{after, step});
      current = condition;
      Instruction cmp; cmp.op = Op::iteration_test; cmp.type = boolean; cmp.slot = hidden; cmp.source = s;
      end(Exit::conditional, {emit(cmp)}, {body, done}, s);
      current = body;
      auto element_type = h.types.find(node(n.children[0]).type)->type_args.empty()
                              ? TypeId{} : h.types.find(node(n.children[0]).type)->type_args[0];
      if (h.types.find(node(n.children[0]).type)->kind == hir::TypeKind::String)
        for (const auto &local : h.ownership.locals)
          if (local.symbol == fi.element) element_type = local.type;
      Instruction read; read.op = Op::iteration_read; read.type = element_type;
      read.slot = hidden; read.source = s;
      auto element = emit(read);
      auto local = slot(element_type); slots[fi.element] = local;
      f.slots[local.value].binding = fi.element;
      if (owning(element_type)) f.slots[local.value].owner = hir::OwnerRef{hir::OwnerKind::binding, fi.element.value};
      store(local, element, s);
      stmt(fi.body); if (live()) jump(step, s);
      current = step;
      effect(Op::iteration_next, hidden, s); jump(condition, s);
      current = done;
      ExitSite site{id, hir::ExitKind::normal, {}}; clean(site); jump(after, s); finish(site);
      loops.erase(id); current = after; break;
    }
    case hir::NodeKind::return_: {
      auto result =
          n.children.empty() ? constant(unit, 0, s) : expr(n.children[0]);
      ExitSite site{id, hir::ExitKind::return_, {}};
      if (!n.children.empty()) take(n.children[0]);
      clean(site, n.children.empty() ? std::nullopt : std::optional{result});
      end(Exit::return_, {result}, {}, s);
      finish(site);
      break;
    }
    case hir::NodeKind::break_: {
      ExitSite site{id, hir::ExitKind::break_, {}};
      clean(site);
      jump(loops.at(std::get<hir::Break>(n.data).loop).first, s);
      finish(site);
      break;
    }
    case hir::NodeKind::continue_: {
      ExitSite site{id, hir::ExitKind::continue_, {}};
      clean(site);
      jump(loops.at(std::get<hir::Continue>(n.data).loop).second, s);
      finish(site);
      break;
    }
    default:
      (void)expr(id);
      break;
    }
  }

public:
  Lower(const hir::HirModule &input, const hir::HirFunction &fn)
      : h(input), aggregates(input) {
    for (uint64_t t = 1; t <= h.types.size(); ++t) {
      auto k = h.types.find(TypeId{t})->kind;
      if (k == hir::TypeKind::Unit)
        unit = TypeId{t};
      if (k == hir::TypeKind::Bool)
        boolean = TypeId{t};
    }
    f.symbol = fn.symbol;
    f.signature = fn.signature;
    f.source = fn.source.span;
    const auto &signature = *h.types.find(fn.signature);
    for (uint32_t p = 0; p < fn.parameters.size(); ++p) {
      auto type = signature.parameters[p];
      auto local = slot(type.type);
      f.slots.back().parameter = p;
      f.slots.back().binding = fn.parameters[p].symbol;
      f.slots.back().inout = type.mode == hir::ParamMode::inout;
      if (owning(type.type)) {
        f.slots.back().owner = hir::OwnerRef{hir::OwnerKind::binding, fn.parameters[p].symbol.value};
        f.slots.back().owning_parameter = type.mode == hir::ParamMode::move;
      }
      // Discard labels do not affect position or SymbolId identity.
      slots.emplace(fn.parameters[p].symbol, local);
    }
    current = block();
    stmt(fn.body);
    if (live()) {
      if (h.types.find(signature.result)->kind == hir::TypeKind::Unit)
        end(Exit::return_, {constant(unit, 0, fn.source.span)}, {},
            fn.source.span);
      else
        end(Exit::unreachable, {}, {}, fn.source.span);
    }
  }
  Function take() {
    // Both branches can return, leaving a generated join with no predecessor.
    // Remove such blocks (and their descendants) before checking dominance.
    std::vector<bool> reachable(f.blocks.size(), false);
    std::vector<BlockId> work{BlockId{0}};
    while (!work.empty()) {
      auto b = work.back();
      work.pop_back();
      if (reachable[b.value])
        continue;
      reachable[b.value] = true;
      for (auto target : f.blocks[b.value].terminator.targets)
        work.push_back(target);
    }
    std::vector<BlockId> remap(f.blocks.size());
    std::vector<BasicBlock> kept;
    for (size_t b = 0; b < f.blocks.size(); ++b)
      if (reachable[b]) {
        remap[b] = BlockId{kept.size()};
        kept.push_back(std::move(f.blocks[b]));
      }
    for (auto &b : kept)
      for (auto &target : b.terminator.targets)
        target = remap[target.value];
    std::vector<Handler> kept_handlers;
    std::vector<bool> removed_handler_slot(f.slots.size(), false);
    for (const auto &handler : f.handlers) {
      if (!reachable[handler.block.value]) {
        if (handler.slot.value < removed_handler_slot.size())
          removed_handler_slot[handler.slot.value] = true;
        continue;
      }
      auto kept_handler = handler;
      kept_handler.block = remap[handler.block.value];
      kept_handler.protected_entry = remap[handler.protected_entry.value];
      kept_handlers.push_back(kept_handler);
    }
    // Slots are created while lowering. A slot whose only actual uses were in
    // blocks or handlers removed above belongs to that dead control flow, even
    // though moving kept blocks leaves its LocalSlotId dangling. Keep every
    // parameter slot and every slot that surviving IR still actually uses.
    std::vector<bool> removed_use(f.slots.size(), false);
    std::vector<bool> kept_use(f.slots.size(), false);
    for (size_t b = 0; b < f.blocks.size(); ++b) {
      if (reachable[b])
        continue;
      for (const auto &i : f.blocks[b].instructions)
        if (uses_slot(i.op) && i.slot.value < removed_use.size())
          removed_use[i.slot.value] = true;
    }
    for (const auto &b : kept)
      for (const auto &i : b.instructions)
        if (uses_slot(i.op) && i.slot.value < kept_use.size())
          kept_use[i.slot.value] = true;
    for (const auto &handler : kept_handlers)
      if (handler.slot.value < kept_use.size())
        kept_use[handler.slot.value] = true;
    std::vector<std::optional<LocalSlotId>> slot_remap(f.slots.size());
    std::vector<Slot> kept_slots;
    for (size_t s = 0; s < f.slots.size(); ++s) {
      bool dead_only = !f.slots[s].parameter && !kept_use[s] &&
                       (removed_use[s] || removed_handler_slot[s]);
      if (dead_only)
        continue;
      slot_remap[s] = LocalSlotId{kept_slots.size()};
      kept_slots.push_back(std::move(f.slots[s]));
    }
    auto mapped_slot = [&](LocalSlotId old) {
      if (old.value >= slot_remap.size() || !slot_remap[old.value])
        throw std::logic_error("slot reference belongs to removed control flow");
      return *slot_remap[old.value];
    };
    for (auto &b : kept)
      for (auto &i : b.instructions)
        if (uses_slot(i.op))
          i.slot = mapped_slot(i.slot);
    for (auto &handler : kept_handlers)
      handler.slot = mapped_slot(handler.slot);
    f.handlers = std::move(kept_handlers);
    f.slots = std::move(kept_slots);
    f.blocks = std::move(kept);
    return std::move(f);
  }
};
} // namespace
PhaseResult<VerifiedModule> lower(const hir::VerifiedHirModule &input) {
  auto capabilities = hir::check_backend_capabilities(input.module());
  if (!capabilities.success()) {
    PhaseResult<VerifiedModule> r;
    r.state = capabilities.state;
    r.failure = capabilities.failure;
    r.diagnostics = std::move(capabilities.diagnostics);
    return r;
  }
  if (input.config().backend != hir::Backend::native ||
      input.config().word_bits != 64) {
    PhaseResult<VerifiedModule> r;
    r.failure = FailureKind::unsupported;
    r.diagnostics.push_back({"E_CAPABILITY_UNAVAILABLE",
                             "lowering",
                             "error",
                             "native scalars require native/64 HIR",
                             {},
                             {},
                             {},
                             {},
                             {},
                             {}});
    return r;
  }
  try {
    Module m{input, {}};
    for (const auto &f : input.module().functions)
      m.functions.push_back(Lower(input.module(), f).take());
    return verify(m);
  } catch (const std::exception &e) {
    PhaseResult<VerifiedModule> r;
    r.diagnostics.push_back({"E_IR_INVALID",
                             "lowering",
                             "error",
                             e.what(),
                             {},
                             {},
                             {},
                             {},
                             {},
                             {}});
    return r;
  }
}
} // namespace lang::ir
