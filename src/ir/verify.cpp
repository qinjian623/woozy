#include "lang/ir/native.hpp"
#include "layout.hpp"
#include <algorithm>
#include <functional>
#include <set>
#include <sstream>

namespace lang::ir {
namespace {
using Set = std::set<size_t>;
template <class T> std::set<T> intersect(const std::set<T> &a, const std::set<T> &b) {
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
      case Exit::return_:
        shape = t.inputs.size() == 1 && t.targets.empty();
        break;
      case Exit::unreachable:
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
      for (size_t p = 0; p < bb.instructions.size(); ++p) {
        auto &i = bb.instructions[p];
        type(i.type, i.source);
        need(i.result.has_value() == (i.op != Op::store && i.op != Op::store_indirect),
             "invalid instruction result", i.source);
        if (i.result)
          need(defs.emplace(*i.result, Def{b, p, i.type, i.op == Op::address ||
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
    // every incoming path. Merely having a dominating tag comparison is not enough.
    using Facts = std::set<std::pair<ValueId, SymbolId>>;
    Facts universe;
    for (auto [value, i] : instructions)
      if (i->op == Op::enum_test && i->inputs.size() == 1)
        universe.emplace(i->inputs[0], i->variant);
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
          if (term.kind == Exit::conditional && term.targets[0].value == b &&
              term.targets[1].value != b) {
            auto found = instructions.find(term.inputs[0]);
            if (found != instructions.end()) {
              const auto &test = *found->second;
              if (test.op == Op::enum_test && test.inputs.size() == 1)
                edge.emplace(test.inputs[0], test.variant);
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
          args.push_back(use(v, b, p, i.source));
        auto arity = [&](size_t n) {
          need(args.size() == n, "instruction operand count", i.source);
        };
        auto value = [&](size_t n) -> const hir::Type & {
          need(!args.at(n).address, "address used as scalar value", i.source);
          return type(args[n].type, i.source);
        };
        switch (i.op) {
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
                        : (i.unary == hir::UnaryOp::negate ||
                           i.unary == hir::UnaryOp::bit_not) &&
                              integer(k)),
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
          need(i.binary >= hir::BinaryOp::add &&
                   i.binary <= hir::BinaryOp::greater_equal &&
                   ((integer(l) && integer(r)) ||
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
          const auto &cs = type(i.signature, i.source);
          need(cs.kind == hir::TypeKind::Function,
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
          auto fields = aggregates.fields(i.type, structure ? std::nullopt
                                                            : std::optional{i.variant});
          arity(fields.size());
          for (size_t a = 0; a < fields.size(); ++a) {
            value(a);
            need(args[a].type == fields[a].type, "constructor field type", i.source);
          }
          break;
        }
        case Op::field_read:
        case Op::field_address: {
          arity(1);
          need(args[0].address == (i.op == Op::field_address) &&
                   type(args[0].type, i.source).kind == hir::TypeKind::NominalStruct,
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
        case Op::store_indirect:
          arity(2);
          value(1);
          need(args[0].address && args[0].type == args[1].type &&
                   t.kind == hir::TypeKind::Unit,
               "indirect store type/mode", i.source);
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
            auto index = aggregates.field_index(args[0].type, i.field, i.variant);
            need(fields[index].type == i.type, "enum payload result type", i.source);
            need(facts[b].contains({i.inputs[0], i.variant}),
                 "enum payload without matching variant edge", i.source);
          }
          break;
        }
        default:
          need(false, "unknown native operation", i.source);
        }
      }
      const auto &t = bb.terminator;
      for (auto v : t.inputs) {
        const auto &d = use(v, b, bb.instructions.size(), t.source);
        need(!d.address &&
                 (t.kind == Exit::return_
                      ? d.type == sig.result
                      : type(d.type, t.source).kind == hir::TypeKind::Bool),
             "terminator type mismatch", t.source);
      }
    }
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
    for (size_t b = 0; b < f.blocks.size(); ++b) {
      out << " b" << b << ":\n";
      for (auto &i : f.blocks[b].instructions) {
        out << "  ";
        if (i.result)
          out << '%' << i.result->value << " = ";
        static constexpr const char *names[] = {"constant", "function", "load",
                                                "address",  "store",    "unary",
                                                "binary",   "convert",  "call",
            "struct_init", "enum_init", "field_read", "field_address",
            "load_indirect", "store_indirect", "enum_test", "enum_payload",
            "string_literal"};
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
        if (i.op == Op::field_read || i.op == Op::field_address || i.op == Op::enum_payload)
          out << " field=@" << i.field.value;
        if (i.op == Op::enum_init || i.op == Op::enum_test || i.op == Op::enum_payload)
          out << " variant=@" << i.variant.value;
        for (auto a : i.inputs)
          out << " %" << a.value;
        out << " source=" << i.source.snapshot.value << ':'
            << i.source.begin_byte << '-' << i.source.end_byte << '\n';
      }
      const auto &t = f.blocks[b].terminator;
      static constexpr const char *exits[] = {
          "missing", "branch", "conditional", "return", "unreachable"};
      out << "  " << exits[static_cast<unsigned>(t.kind)];
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
