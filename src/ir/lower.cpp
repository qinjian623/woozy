#include "lang/ir/native.hpp"
#include "layout.hpp"
#include <map>
#include <stdexcept>

namespace lang::ir {
namespace {
class Lower {
  const hir::HirModule &h;
  detail::AggregateTypes aggregates;
  Function f;
  BlockId current;
  uint64_t next_value = 0;
  std::map<SymbolId, LocalSlotId> slots;
  std::map<HirNodeId, std::pair<BlockId, BlockId>> loops;
  TypeId unit, boolean;
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
      for (auto d : p->defers) {
        marker(Op::cleanup_begin, {}, d);
        // Each expansion gets fresh local slots and control targets. External
        // bindings retain the very same slot, and nested expansions restore
        // their parent's context before the next deferred body is emitted.
        auto outer_slots = slots;
        auto outer_loops = loops;
        auto outer_handlers = handlers;
        stmt(node(d).children[0]);
        slots = std::move(outer_slots);
        loops = std::move(outer_loops);
        handlers = std::move(outer_handlers);
        marker(Op::cleanup_end, {}, d);
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
  void bind_pattern(const hir::Pattern &p, ValueId value) {
    if (p.binding) {
      auto local = slot(p.type);
      slots.emplace(*p.binding, local);
      store(local, value, p.source.span);
    }
    if (p.variant) {
      auto fields = aggregates.fields(p.type, p.variant);
      for (size_t a = 0; a < p.payload.size(); ++a) {
        // Wildcards need no payload read.
        if (!p.payload[a].binding)
          continue;
        Instruction i;
        i.op = Op::enum_payload;
        i.type = p.payload[a].type;
        i.variant = *p.variant;
        i.field = fields[a].symbol;
        i.inputs = {value};
        i.source = p.payload[a].source.span;
        bind_pattern(p.payload[a], emit(i));
      }
    }
  }
  ValueId expr(HirNodeId id) {
    const auto &n = node(id);
    Instruction i;
    i.type = n.type;
    i.source = n.source.span;
    switch (n.kind()) {
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
    case hir::NodeKind::read:
      return access(Op::load, slots.at(std::get<hir::Read>(n.data).symbol),
                    i.source);
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
      return emit(i);
    case hir::NodeKind::unary:
      i.op = Op::unary;
      i.unary = std::get<hir::Unary>(n.data).op;
      i.inputs = {expr(n.children[0])};
      return emit(i);
    case hir::NodeKind::binary: {
      i.op = Op::binary;
      i.binary = std::get<hir::Binary>(n.data).op;
      auto lhs = expr(n.children[0]);
      auto rhs = expr(n.children[1]);
      i.inputs = {lhs, rhs};
      return emit(i);
    }
    case hir::NodeKind::integer_convert:
      i.op = Op::convert;
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
      i.op = Op::call;
      i.origin = id;
      i.signature = c.signature;
      ValueId callee;
      if (c.callee)
        callee = expr(*c.callee);
      else {
        Instruction fn;
        fn.op = Op::function;
        fn.type = c.signature;
        fn.function = *c.direct_symbol;
        fn.source = i.source;
        callee = emit(fn);
      }
      i.inputs.resize(c.args.size() + 1);
      i.inputs[0] = callee;
      // Evaluation follows source order. Only the saved values are reordered.
      for (const auto &arg : c.args) {
        auto value = expr(arg.expression);
        i.inputs[arg.parameter_index + 1] = value;
      }
      const auto &signature = *h.types.find(c.signature);
      if (!signature.throws_type)
        return emit(i);
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
      break;
    case hir::NodeKind::binding: {
      auto initial = expr(n.children[0]);
      auto target = slot(node(n.children[0]).type);
      slots.emplace(std::get<hir::Binding>(n.data).symbol, target);
      store(target, initial, s);
      break;
    }
    case hir::NodeKind::assign: {
      const auto &place = node(n.children[0]);
      auto target = expr(n.children[0]);
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
        i.inputs = {*lhs, rhs};
        i.source = s;
        rhs = emit(i);
      }
      Instruction write;
      write.op = Op::store_indirect;
      write.type = unit;
      write.inputs = {target, rhs};
      write.source = s;
      f.blocks[current.value].instructions.push_back(write);
      break;
    }
    case hir::NodeKind::switch_: {
      auto value = expr(n.children[0]);
      auto after = block();
      loops.emplace(id, std::pair{after, after});
      for (const auto &arm : std::get<hir::Switch>(n.data).arms) {
        auto body = block(), next = block();
        if (arm.pattern.variant) {
          Instruction test;
          test.op = Op::enum_test;
          test.type = boolean;
          test.variant = *arm.pattern.variant;
          test.inputs = {value};
          test.source = arm.source.span;
          end(Exit::conditional, {emit(test)}, {body, next}, s);
        } else
          jump(body, s);
        current = body;
        bind_pattern(arm.pattern, value);
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
      break;
    }
    case hir::NodeKind::return_: {
      auto result =
          n.children.empty() ? constant(unit, 0, s) : expr(n.children[0]);
      ExitSite site{id, hir::ExitKind::return_, {}};
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
      f.slots.back().inout = type.mode == hir::ParamMode::inout;
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
    for (auto handler : f.handlers)
      if (reachable[handler.block.value]) {
        handler.block = remap[handler.block.value];
        handler.protected_entry = remap[handler.protected_entry.value];
        kept_handlers.push_back(handler);
      }
    f.handlers = std::move(kept_handlers);
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
