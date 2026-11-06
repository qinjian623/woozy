#include "lang/ir/native.hpp"
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
      } else
        need(!slot.inout, "local slot cannot be an incoming pointer", s);
    }
    need(parameters.size() == sig.parameters.size(), "missing parameter slot",
         s);
    struct Def {
      size_t block, position;
      TypeId type;
      bool address;
    };
    std::map<ValueId, Def> defs;
    std::map<ValueId, const Instruction *> instructions;
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
        need(defs.emplace(param.value, Def{b, 0, param.type, false}).second,
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
                  i.op != Op::ownership_end && i.op != Op::replace_indirect && i.op != Op::begin_borrow && i.op != Op::vec_check),
             "invalid instruction result", i.source);
        if (i.result)
          need(defs.emplace(*i.result, Def{b, p + 1, i.type,
                                           i.op == Op::address ||
                                               i.op == Op::field_address})
                   .second,
               "duplicate value definition", i.source);
        if (i.result)
          instructions.emplace(*i.result, &i);
      }
    }
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
    for (size_t i = 0; i < f.slots.size(); ++i) {
      all_slots.insert(i);
      if (f.slots[i].parameter)
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
    for (size_t b = 0; b < f.blocks.size(); ++b) {
      Set initialized = in[b];
      const auto &bb = f.blocks[b];
      for (size_t p = 0; p < bb.instructions.size(); ++p) {
        const auto &i = bb.instructions[p];
        const auto &t = type(i.type, i.source);
        need(i.op == Op::string_literal || i.utf8.empty(),
             "string bytes on a non-string instruction", i.source);
        std::vector<Def> args;
        for (auto v : i.inputs)
          args.push_back(use(v, b, p + 1, i.source));
        auto arity = [&](size_t n) {
          need(args.size() == n, "instruction operand count", i.source);
        };
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
          arity(0); need(i.origin && i.bits <= 1 && t.kind == hir::TypeKind::Unit,
                        "ownership batch shape", i.source); break;
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
        case Op::vec_set:
          arity(i.op == Op::vec_set ? 3 : 2);
          need(value(0).kind == hir::TypeKind::Vec && integer(value(1).kind),
               "Vec access receiver/index", i.source);
          if (i.op == Op::vec_read)
            need(type(args[0].type, i.source).type_args[0] == i.type, "Vec read result", i.source);
          else if (i.op == Op::vec_check) need(t.kind == hir::TypeKind::Unit, "Vec check type", i.source);
          else {
            value(2); need(type(args[0].type, i.source).type_args[0] == args[2].type &&
                          t.kind == hir::TypeKind::Unit, "Vec write type", i.source);
          }
          break;
        case Op::vec_slice:
          need(value(0).kind == hir::TypeKind::Vec && args[0].type == i.type && i.bits <= 3,
               "Vec slice type", i.source);
          arity(1 + bool(i.bits & 1) + bool(i.bits & 2));
          for (size_t a = 1; a < args.size(); ++a) need(integer(value(a).kind), "slice endpoint type", i.source);
          break;
        case Op::iteration_begin:
        case Op::iteration_next:
        case Op::iteration_read:
        case Op::iteration_test:
          arity(0);
          need(i.slot.value < f.slots.size() && f.slots[i.slot.value].owner &&
               f.slots[i.slot.value].owner->kind == hir::OwnerKind::iteration &&
               type(f.slots[i.slot.value].type, i.source).kind == hir::TypeKind::Vec,
               "iteration slot", i.source);
          if (i.op == Op::iteration_read)
            need(i.type == type(f.slots[i.slot.value].type, i.source).type_args[0], "iteration element type", i.source);
          else need(t.kind == (i.op == Op::iteration_test ? hir::TypeKind::Bool : hir::TypeKind::Unit),
                    "iteration operation type", i.source);
          break;
        case Op::vec_length:
          arity(1); need(value(0).kind == hir::TypeKind::Vec && t.kind == hir::TypeKind::Usize,
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
        case Op::function:
          arity(0);
          need(functions.contains(i.function) &&
                   functions.at(i.function) == i.type,
               "unknown function identity/signature", i.source);
          break;
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
          need(i.binary >= hir::BinaryOp::add &&
                   i.binary <= hir::BinaryOp::greater_equal &&
                   ((integer(l) && integer(r)) || floating ||
                    (eq && l == hir::TypeKind::Bool && r == l)) &&
                   (shift || args[0].type == args[1].type) &&
                   (comparison ? t.kind == hir::TypeKind::Bool
                               : i.type == args[0].type),
               "invalid binary types/op", i.source);
          break;
        }
        case Op::convert:
          arity(1);
          need(integer(value(0).kind) && integer(t.kind),
               "invalid integer conversion", i.source);
          break;
        case Op::call: {
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
      if (t.kind == Exit::error_return)
        need(t.origin && t.exit && t.exit->kind == hir::ExitKind::error &&
                 t.exit->error_type == sig.throws_type,
             "ErrorReturn without verified error exit", t.source);
      if (t.kind == Exit::return_ && !t.origin)
        need(type(sig.result, t.source).kind == hir::TypeKind::Unit &&
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
      } else {
        need(t.signature.value == 0, "signature on non-TryCall", t.source);
        for (const auto &d : operands)
          need(!d.address &&
                   (t.kind == Exit::return_ ? d.type == sig.result
                    : t.kind == Exit::error_return
                        ? sig.throws_type == d.type
                        : type(d.type, t.source).kind == hir::TypeKind::Bool),
               "terminator type mismatch", t.source);
      }
      if (!bb.parameters.empty()) {
        need(b != 0 && predecessors[b].size() == 1, "orphan block parameter",
             t.source);
        const auto &pred = f.blocks[*predecessors[b].begin()].terminator;
        need(pred.kind == Exit::try_call, "parameter without TryCall edge",
             t.source);
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
        if (i.op == Op::call && !impossible.contains(b))
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
      if (t.origin && t.kind != Exit::try_call) {
        const auto &n = node(*t.origin);
        for (const auto &p : n.exits) {
          bool relevant =
              t.kind == Exit::return_        ? p.kind == hir::ExitKind::return_
              : t.kind == Exit::error_return ? p.kind == hir::ExitKind::error
                                             : true;
          if (relevant)
            need(t.exit && t.exit->origin == *t.origin &&
                     p.kind == t.exit->kind &&
                     p.error_type == t.exit->error_type,
                 "exit omitted required cleanup plan", t.source);
        }
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
        } else
          need(t.kind == Exit::branch, "normal/jump exit is not branch",
               t.source);
        state.pop_back();
      }
      // An exhaustive enum switch's default edge cannot execute, even when
      // generated inside a cleanup. Other unfinished exits remain invalid.
      if (t.kind == Exit::return_ || t.kind == Exit::error_return ||
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
            "vec_init", "vec_read", "vec_set", "vec_slice", "iteration_begin", "iteration_end", "vec_length", "iteration_test", "iteration_read", "iteration_next", "ownership_begin", "ownership_end", "replace_indirect", "begin_borrow", "vec_check"};
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
        if (i.op == Op::call)
          out << " signature=" << i.signature.value;
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
          "unreachable", "try_call", "error_return"};
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
