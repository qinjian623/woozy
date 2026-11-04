#include "ownership.hpp"
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace lang::hir {
namespace {
struct Problem {
  Source source;
  std::string code, message;
};
const Type &ty(const HirModule &m, TypeId id) {
  auto p = m.types.find(id);
  if (!p)
    throw std::invalid_argument("ownership type is not in public TypeContext");
  return *p;
}
const NominalDecl &decl(const HirModule &m, SymbolId id) {
  for (const auto &d : m.nominals)
    if (d.symbol == id)
      return d;
  throw std::invalid_argument("ownership nominal declaration missing");
}
size_t weight(const HirModule &m, TypeId id) {
  size_t result = 1;
  for (auto a : ty(m, id).type_args)
    result += weight(m, a);
  return result;
}
bool recursive(const HirModule &m, TypeId id,
               const std::set<uint64_t> &active) {
  const auto &t = ty(m, id);
  for (auto prior : active) {
    if (prior == id.value)
      return true;
    const auto &p = ty(m, TypeId{prior});
    if ((t.kind == TypeKind::NominalStruct ||
         t.kind == TypeKind::NominalEnum) &&
        p.kind == t.kind && p.declaration == t.declaration &&
        weight(m, id) >= weight(m, TypeId{prior}))
      return true;
  }
  return active.size() >= 128;
}
ValueOps classify(const HirModule &m, TypeId id, std::set<uint64_t> &active) {
  auto t = ty(m, id);
  ValueOps out;
  if (t.kind == TypeKind::TypeParameter) {
    out.kind = ValueKind::dependent;
    return out;
  }
  if (t.kind == TypeKind::String || t.kind == TypeKind::Error ||
      t.kind == TypeKind::ForeignOpaque) {
    out.reason =
        "dynamic string/foreign/error value ownership is not supported";
    return out;
  }
  if (t.kind != TypeKind::Vec && t.kind != TypeKind::NominalStruct &&
      t.kind != TypeKind::NominalEnum) {
    out.kind = ValueKind::trivial;
    out.pod_inout = t.kind == TypeKind::Bool || t.kind == TypeKind::SignedInt ||
                    t.kind == TypeKind::UnsignedInt ||
                    t.kind == TypeKind::Isize || t.kind == TypeKind::Usize ||
                    t.kind == TypeKind::Float;
    return out;
  }
  // Reject declaration recursion, including through Vec, before substitution
  // can produce an unbounded sequence such as S[T] -> Vec[S[Vec[T]]].
  uint64_t key = id.value;
  if (recursive(m, id, active)) {
    out.reason = "recursive owning graph is not supported";
    return out;
  }
  active.insert(key);
  if (t.kind == TypeKind::Vec) {
    auto e = classify(m, t.type_args.at(0), active);
    out.kind = e.kind == ValueKind::unsupported ? ValueKind::unsupported
                                                : ValueKind::vec;
    out.needs_drop = out.kind != ValueKind::unsupported;
    out.reason = e.reason;
  } else {
    const auto &d = decl(m, t.declaration);
    TypeContext ctx = m.types;
    Substitutions sub;
    for (size_t i = 0; i < d.params.size(); ++i)
      sub.push_back({d.params[i].symbol, t.type_args.at(i)});
    out.kind = t.kind == TypeKind::NominalStruct ? ValueKind::structure
                                                 : ValueKind::enumeration;
    out.pod_inout = true;
    auto fields = [&](const auto &fs, std::optional<SymbolId> v) {
      for (const auto &f : fs) {
        auto ft = substitute(f.type, sub, ctx);
        // No IDs from the scratch context may appear in returned records.
        if (!m.types.find(ft))
          throw std::invalid_argument(
              "concrete ownership member type was not interned publicly");
        auto ops = classify(m, ft, active);
        out.fields.push_back({v, f.symbol, ft});
        out.needs_drop |= ops.needs_drop;
        out.pod_inout &= ops.pod_inout;
        if (ops.kind == ValueKind::unsupported) {
          out.kind = ValueKind::unsupported;
          out.reason = ops.reason;
        }
      }
    };
    fields(d.fields, {});
    for (const auto &v : d.variants)
      fields(v.payload, v.symbol);
  }
  active.erase(key);
  return out;
}
void intern_members(HirModule &m, TypeId id, std::set<uint64_t> &active,
                    std::set<uint64_t> &done) {
  if (done.count(id.value))
    return;
  auto t = ty(m, id);
  if (t.kind == TypeKind::Vec) {
    if (recursive(m, id, active))
      return;
    active.insert(id.value);
    intern_members(m, t.type_args.at(0), active, done);
    active.erase(id.value);
    done.insert(id.value);
    return;
  }
  if (t.kind != TypeKind::NominalStruct && t.kind != TypeKind::NominalEnum) {
    done.insert(id.value);
    return;
  }
  if (recursive(m, id, active))
    return;
  active.insert(id.value); // classifier issues the source diagnostic
  auto d = decl(m, t.declaration);
  Substitutions sub;
  for (size_t i = 0; i < d.params.size(); ++i)
    sub.push_back({d.params[i].symbol, t.type_args.at(i)});
  auto fields = [&](const auto &fs) {
    for (const auto &f : fs)
      intern_members(m, substitute(f.type, sub, m.types), active, done);
  };
  fields(d.fields);
  for (const auto &v : d.variants)
    fields(v.payload);
  active.erase(id.value);
  done.insert(id.value);
}
struct Derived {
  Ownership records;
  std::vector<std::vector<ExitPlan>> exits;
  std::set<uint64_t> checked_nodes;
};
class Analysis {
  const HirModule &m;
  std::stop_token stop;
  Derived out;
  std::vector<std::optional<HirNodeId>> parents;
  std::map<uint64_t, LocalLifetime> locals;
  std::map<uint64_t, HirNodeId> declarations;
  std::map<uint64_t, HirNodeId> scope_nodes;
  std::map<uint64_t, std::vector<SymbolId>> scope_locals;
  std::map<uint64_t, std::vector<CleanupStep>> failure_prefix;
  std::set<uint64_t> enabled;
  std::vector<CleanupStep> active_borrows;
  const HirFunction *fun = nullptr;
  using State =
      std::map<uint64_t, unsigned>; // bit 1: live, bit 2: moved/uninitialized
  struct Edge {
    ExitKind kind;
    uint64_t target;
    TypeId error;
    State state;
  };
  struct Flow {
    std::optional<State> normal;
    std::vector<Edge> edges;
  };
  const HirNode &n(HirNodeId id) const { return m.nodes.at(id.value); }
  [[noreturn]] void issue(HirNodeId id, std::string code,
                          std::string msg) const {
    throw Problem{n(id).source, std::move(code), std::move(msg)};
  }
  bool owned(TypeId t) const { return classify_value(m, t).needs_drop; }
  static void join(std::optional<State> &dst, const State &s) {
    if (!dst) {
      dst = s;
      return;
    }
    for (auto [id, v] : s)
      (*dst)[id] |= v;
  }
  static void edges(Flow &to, Flow &from) {
    to.edges.insert(to.edges.end(), from.edges.begin(), from.edges.end());
  }
  void local(SymbolId s, ScopeId scope, TypeId type, ParamMode mode) {
    auto order = uint32_t(scope_locals[scope.value].size());
    locals.emplace(s.value, LocalLifetime{s, scope, type, order, mode});
    scope_locals[scope.value].push_back(s); // declaration order
  }
  void pattern(const Pattern &p, ScopeId s) {
    if (p.binding)
      local(*p.binding, s, p.type, ParamMode::move);
    for (const auto &c : p.payload)
      pattern(c, s);
  }
  void gather(HirNodeId id, std::optional<HirNodeId> p = {}) {
    check_cancelled(stop);
    parents[id.value] = p;
    const auto &x = n(id);
    if (x.kind() == NodeKind::block || x.kind() == NodeKind::for_ ||
        x.kind() == NodeKind::for_in)
      scope_nodes[x.scope.value] = id;
    if (ty(m, x.type).kind != TypeKind::String) {
      auto ops = classify_value(m, x.type);
      if (ops.kind == ValueKind::unsupported)
        issue(id, "E_CAPABILITY_UNAVAILABLE", ops.reason);
      if (ops.needs_drop || x.kind() == NodeKind::move ||
          x.kind() == NodeKind::for_in)
        enabled.insert(fun->symbol.value);
    }
    if (auto fi = std::get_if<ForIn>(&x.data))
      local(fi->element, fi->body_scope,
            ty(m, n(x.children[0]).type).type_args[0], ParamMode::move);
    if (auto sw = std::get_if<Switch>(&x.data)) {
      if (owned(n(x.children[0]).type) &&
          n(x.children[0]).kind() != NodeKind::read)
        issue(id, "E_CAPABILITY_UNAVAILABLE",
              "computed owning switch temporary lifetime is not specified");
      for (const auto &a : sw->arms)
        pattern(a.pattern, a.scope);
    }
    if (auto tr = std::get_if<Try>(&x.data))
      for (const auto &c : tr->catches)
        local(c.binding, c.scope, c.error_type, ParamMode::move);
    for (auto c : x.children)
      gather(c, id);
    if (auto b = std::get_if<Binding>(&x.data)) {
      declarations.emplace(b->symbol.value, id);
      local(b->symbol, x.scope, n(x.children[0]).type, ParamMode::move);
    }
  }
  std::vector<CleanupStep>
  temp_cleanup(const std::vector<HirNodeId> &live) const {
    std::vector<CleanupStep> r;
    for (auto it = live.rbegin(); it != live.rend(); ++it)
      r.push_back({CleanupKind::destroy_if_live,
                   {OwnerKind::temporary, it->value},
                   n(*it).type});
    return r;
  }
  void consume(std::vector<HirNodeId> &live, HirNodeId c) {
    auto it = std::find(live.begin(), live.end(), c);
    if (it != live.end())
      live.erase(it);
  }
  std::optional<SymbolId> borrow_root(HirNodeId id) const {
    const auto &x = n(id);
    if (auto r = std::get_if<Read>(&x.data))
      return r->symbol;
    if (auto r = std::get_if<Place>(&x.data))
      return r->symbol;
    if (x.kind() == NodeKind::field_read || x.kind() == NodeKind::field_place)
      return borrow_root(x.children[0]);
    return {};
  }
  std::vector<CleanupStep>
  prefix_cleanup(const std::vector<HirNodeId> &live) const {
    std::vector<CleanupStep> r(active_borrows.rbegin(), active_borrows.rend());
    auto drops = temp_cleanup(live);
    r.insert(r.end(), drops.begin(), drops.end());
    return r;
  }
  void use(HirNodeId id, uint32_t operand, HirNodeId child, UseKind kind) {
    if (!owned(n(child).type) && n(child).kind() != NodeKind::move &&
        kind != UseKind::inout &&
        !(kind == UseKind::borrow && borrow_root(child)))
      return;
    std::optional<SymbolId> root = borrow_root(child);
    if (auto r = std::get_if<Read>(&n(child).data))
      root = r->symbol;
    if (auto r = std::get_if<Place>(&n(child).data))
      root = r->symbol;
    if (auto r = std::get_if<Move>(&n(child).data))
      root = r->binding;
    out.records.uses.push_back({id, operand, kind, root, n(child).type});
  }
  const std::vector<CallArg> *call_args(const HirNode &x) const {
    if (auto c = std::get_if<Call>(&x.data))
      return &c->args;
    if (auto c = std::get_if<ConstraintCall>(&x.data))
      return &c->args;
    return nullptr;
  }
  std::optional<HirNodeId> call_callee(const HirNode &x) const {
    if (auto c = std::get_if<Call>(&x.data))
      return c->callee;
    return {};
  }
  // Evaluates all potential expression edges in source order. Live prefixes
  // describe completed owned values, never an uninitialized result of a call.
  void expr_records(HirNodeId id, HirNodeId fe, std::vector<HirNodeId> &live,
                    bool borrowed_value = false) {
    const auto &x = n(id);
    if (auto sc = std::get_if<ShortCircuit>(&x.data)) {
      expr_records(x.children[0], fe, live);
      auto branch = live;
      expr_records(x.children[1], fe, branch);
      // Scalar boolean operands cannot themselves leave an owned result.
      for (auto c : branch)
        if (std::find(live.begin(), live.end(), c) == live.end())
          live.push_back(c);
    } else if (auto args = call_args(x)) {
      auto floor = active_borrows.size();
      if (auto callee = call_callee(x))
        expr_records(*callee, fe, live);
      for (const auto &a : *args) {
        auto root = a.mode == ParamMode::ordinary ? borrow_root(a.expression)
                                                  : std::nullopt;
        expr_records(a.expression, fe, live, bool(root));
        if (root)
          active_borrows.push_back({CleanupKind::end_borrow,
                                    {OwnerKind::binding, root->value},
                                    locals.at(root->value).type});
        else if (a.mode == ParamMode::ordinary && owned(n(a.expression).type))
          active_borrows.push_back({CleanupKind::end_borrow,
                                    {OwnerKind::temporary, a.expression.value},
                                    n(a.expression).type});
        else if (a.mode == ParamMode::inout)
          active_borrows.push_back(
              {CleanupKind::end_borrow,
               {OwnerKind::binding,
                std::get<Place>(n(a.expression).data).symbol.value},
               n(a.expression).type});
      }
      std::vector<CleanupStep> ends;
      for (size_t i = active_borrows.size(); i > floor; --i)
        ends.push_back(active_borrows[i - 1]);
      if (!ends.empty())
        out.records.calls.push_back({id, ends});
      // Entry into this callee consumes only its move arguments. Earlier
      // pending arguments of an enclosing call still belong to the caller.
      for (auto a : *args)
        if (a.mode == ParamMode::move)
          consume(live, a.expression);
      failure_prefix[id.value] = prefix_cleanup(live);
      active_borrows.resize(floor);
    } else {
      for (auto c : x.children)
        expr_records(c, fe, live,
                     borrowed_value && x.kind() == NodeKind::field_read);
    }
    if (!call_args(x))
      failure_prefix[id.value] = prefix_cleanup(live);
    for (size_t i = 0; i < x.children.size(); ++i) {
      auto c = x.children[i];
      UseKind kind = UseKind::borrow;
      if (x.kind() == NodeKind::struct_init ||
          x.kind() == NodeKind::enum_init || x.kind() == NodeKind::vec_init ||
          x.kind() == NodeKind::move)
        kind = UseKind::take;
      if (auto args = call_args(x)) {
        size_t offset = call_callee(x) ? 1 : 0;
        if (i >= offset) {
          auto pm = (*args)[i - offset].mode;
          kind = pm == ParamMode::move    ? UseKind::take
                 : pm == ParamMode::inout ? UseKind::inout
                                          : UseKind::borrow;
        }
      }
      use(id, uint32_t(i), c, kind);
      if (kind == UseKind::take)
        consume(live, c);
    }
    if (auto r = std::get_if<Read>(&x.data);
        r && (owned(x.type) || borrowed_value))
      out.records.uses.push_back(
          {id, 0, borrowed_value ? UseKind::borrow : UseKind::copy, r->symbol,
           x.type});
    if (auto r = std::get_if<Move>(&x.data); r && r->binding)
      out.records.uses.push_back({id, 0, UseKind::take, r->binding, x.type});
    if (!borrowed_value && owned(x.type) && x.kind() != NodeKind::place &&
        x.kind() != NodeKind::field_place &&
        x.kind() != NodeKind::index_place) {
      out.records.temporaries.push_back({id, fe, x.type});
      live.push_back(id);
    }
  }
  void full(HirNodeId root, HirNodeId expression, bool transfer) {
    std::vector<HirNodeId> live;
    active_borrows.clear();
    expr_records(expression, root, live);
    if (transfer) {
      use(root, 0, expression, UseKind::take);
      consume(live, expression);
    }
    out.records.full_expressions.push_back({root, temp_cleanup(live)});
    failure_prefix[root.value] = temp_cleanup(live);
  }
  void records(HirNodeId id) {
    const auto &x = n(id);
    switch (x.kind()) {
    case NodeKind::binding:
    case NodeKind::return_:
    case NodeKind::throw_:
      if (!x.children.empty())
        full(id, x.children[0], true);
      return;
    case NodeKind::assign: {
      std::vector<HirNodeId> live;
      active_borrows.clear();
      expr_records(x.children[0], id, live);
      if (n(x.children[0]).kind() == NodeKind::field_place) {
        auto root = borrow_root(x.children[0]);
        if (root)
          active_borrows.push_back({CleanupKind::end_borrow,
                                    {OwnerKind::binding, root->value},
                                    locals.at(root->value).type});
      }
      expr_records(x.children[1], id, live);
      use(id, 0, x.children[0], UseKind::borrow);
      use(id, 1, x.children[1], UseKind::take);
      consume(live, x.children[1]);
      out.records.full_expressions.push_back({id, prefix_cleanup(live)});
      active_borrows.clear();
      return;
    }
    case NodeKind::expression_statement:
      full(id, x.children[0], false);
      return;
    case NodeKind::if_:
      full(x.children[0], x.children[0], false);
      for (size_t i = 1; i < x.children.size(); ++i)
        records(x.children[i]);
      return;
    case NodeKind::for_:
      records(x.children[0]);
      full(x.children[1], x.children[1], false);
      records(x.children[2]);
      records(x.children[3]);
      return;
    case NodeKind::for_in: {
      full(id, x.children[0], true);
      const auto &fi = std::get<ForIn>(x.data);
      auto type = locals.at(fi.element.value).type;
      if (owned(type))
        out.records.uses.push_back(
            {id, 0, UseKind::copy, {}, type, fi.element});
      records(x.children[1]);
      return;
    }
    case NodeKind::switch_: {
      if (owned(n(x.children[0]).type)) {
        auto root = std::get<Read>(n(x.children[0]).data).symbol;
        out.records.uses.push_back(
            {id, 0, UseKind::borrow, root, n(x.children[0]).type});
        std::function<void(const Pattern &, HirNodeId)> copies =
            [&](const Pattern &p, HirNodeId body) {
              if (p.binding && owned(p.type))
                out.records.uses.push_back(
                    {body, 0, UseKind::copy, root, p.type, p.binding});
              for (const auto &ch : p.payload)
                copies(ch, body);
            };
        for (const auto &a : std::get<Switch>(x.data).arms)
          copies(a.pattern, a.body);
        out.records.full_expressions.push_back(
            {id,
             {{CleanupKind::end_borrow,
               {OwnerKind::binding, root.value},
               n(x.children[0]).type}}});
      } else
        full(x.children[0], x.children[0], false);
      for (size_t i = 1; i < x.children.size(); ++i)
        records(x.children[i]);
      return;
    }
    case NodeKind::try_: {
      records(x.children[0]);
      for (const auto &c : std::get<Try>(x.data).catches) {
        if (owned(c.error_type))
          out.records.uses.push_back(
              {c.body, 0, UseKind::take, {}, c.error_type, c.binding});
        records(c.body);
      }
      return;
    }
    default:
      for (auto c : x.children)
        records(c);
      return;
    }
  }
  bool contains(HirNodeId outer, HirNodeId id) const {
    for (auto p = std::optional(id); p; p = parents[p->value])
      if (*p == outer)
        return true;
    return false;
  }
  std::optional<TypeId> error(const HirNode &x) const {
    if (auto c = std::get_if<Call>(&x.data))
      return ty(m, c->signature).throws_type;
    if (auto c = std::get_if<ConstraintCall>(&x.data))
      return ty(m, c->signature).throws_type;
    if (x.kind() == NodeKind::throw_)
      return n(x.children[0]).type;
    return {};
  }
  void exit_records(HirNodeId id) {
    const auto &x = n(id);
    out.checked_nodes.insert(id.value);
    auto emit = [&](ExitKind kind, ExitTarget target,
                    std::optional<ScopeId> keep, std::optional<TypeId> e,
                    bool save) {
      ExitPlan p{kind, e, target, {}, {}, save};
      if (save) {
        auto t = e ? *e : n(x.children[0]).type;
        if (owned(t))
          p.actions.push_back(
              {CleanupKind::save_output,
               {e ? OwnerKind::error_output : OwnerKind::return_output,
                id.value},
               t});
      }
      if (auto it = failure_prefix.find(id.value);
          it != failure_prefix.end() && kind != ExitKind::normal)
        p.actions.insert(p.actions.end(), it->second.begin(), it->second.end());
      std::optional<ScopeId> scope = x.scope;
      while (scope != keep) {
        if (!scope)
          throw std::invalid_argument("ownership exit target not ancestor");
        p.exited_scopes.push_back(*scope);
        auto construct = scope_nodes.at(scope->value);
        const auto &block = n(construct);
        if (block.kind() == NodeKind::block) {
          for (auto it = block.children.rbegin(); it != block.children.rend();
               ++it) {
            if (id != construct && !contains(construct, id))
              throw std::invalid_argument("ownership scope");
            if (id != construct) {
              bool before = false;
              for (auto c : block.children) {
                if (contains(c, id))
                  break;
                if (c == *it)
                  before = true;
              }
              if (!before)
                continue;
            }
            if (n(*it).kind() == NodeKind::defer_) {
              p.defers.push_back(*it);
              p.actions.push_back({CleanupKind::defer_,
                                   {OwnerKind::temporary, it->value},
                                   TypeId{0}});
            }
          }
        }
        auto &ls = scope_locals[scope->value];
        for (auto it = ls.rbegin(); it != ls.rend(); ++it) {
          auto l = locals.at(it->value);
          if (l.mode != ParamMode::move || !owned(l.type))
            continue;
          bool visible = true;
          // Declarations after this exit (including the current unfinished
          // initializer) were never installed. Parameters/patterns are ready.
          if (auto it = declarations.find(l.symbol.value);
              it != declarations.end()) {
            auto binding = it->second;
            if (contains(binding, id))
              visible = false;
            else if (id != construct) {
              auto a = std::find(block.children.begin(), block.children.end(),
                                 binding);
              if (a != block.children.end()) {
                visible = false;
                for (auto c : block.children) {
                  if (contains(c, id))
                    break;
                  if (c == binding)
                    visible = true;
                }
              }
            }
          }
          if (visible)
            p.actions.push_back({CleanupKind::destroy_if_live,
                                 {OwnerKind::binding, l.symbol.value},
                                 l.type});
        }
        if (block.kind() == NodeKind::for_in &&
            !contains(block.children[0], id)) {
          p.actions.push_back({CleanupKind::end_iteration,
                               {OwnerKind::iteration, construct.value},
                               TypeId{0}});
          p.actions.push_back({CleanupKind::destroy_if_live,
                               {OwnerKind::iteration, construct.value},
                               n(block.children[0]).type});
        }
        scope = m.scopes.at(scope->value).parent;
      }
      if (e || !p.actions.empty())
        out.exits[id.value].push_back(std::move(p));
    };
    if (auto e = error(x)) {
      std::optional<CatchTarget> handler;
      auto branch = id;
      for (auto p = parents[id.value]; p; p = parents[p->value]) {
        const auto &y = n(*p);
        if (y.kind() == NodeKind::defer_)
          break;
        if (auto t = std::get_if<Try>(&y.data); t && branch == y.children[0])
          for (size_t i = 0; i < t->catches.size(); ++i)
            if (t->catches[i].error_type == *e) {
              handler = CatchTarget{*p, uint32_t(i)};
              break;
            }
        if (handler)
          break;
        branch = *p;
      }
      if (handler)
        emit(ExitKind::error, *handler, n(handler->try_node).scope, e, true);
      else
        emit(ExitKind::error, FunctionExit{fun->symbol}, {}, e, true);
    }
    if (x.kind() == NodeKind::return_)
      emit(ExitKind::return_, FunctionExit{fun->symbol}, {}, {},
           !x.children.empty());
    if (x.kind() == NodeKind::break_ || x.kind() == NodeKind::continue_) {
      auto dest = x.kind() == NodeKind::break_
                      ? std::get<Break>(x.data).loop
                      : std::get<Continue>(x.data).loop;
      auto keep = std::optional(n(dest).scope);
      if (x.kind() == NodeKind::break_ && (n(dest).kind() == NodeKind::for_ ||
                                           n(dest).kind() == NodeKind::for_in))
        keep = m.scopes.at(keep->value).parent;
      emit(x.kind() == NodeKind::break_ ? ExitKind::break_
                                        : ExitKind::continue_,
           JumpExit{dest}, keep, {}, false);
    }
    if (x.kind() == NodeKind::block || x.kind() == NodeKind::for_ ||
        x.kind() == NodeKind::for_in)
      emit(ExitKind::normal, ScopeExit{m.scopes.at(x.scope.value).parent},
           m.scopes.at(x.scope.value).parent, {}, false);
    for (auto c : x.children)
      exit_records(c);
  }
  void readable(HirNodeId id, SymbolId s, const State &st) {
    if (auto it = st.find(s.value); it == st.end() || it->second != 1)
      issue(id, "E_OWNERSHIP_USE_AFTER_MOVE",
            "binding was moved or may have been moved");
  }
  Flow expression(HirNodeId id, State st, std::set<uint64_t> borrowed = {}) {
    check_cancelled(stop);
    const auto &x = n(id);
    Flow out{st, {}};
    if (auto r = std::get_if<Read>(&x.data))
      readable(id, r->symbol, st);
    if (x.kind() == NodeKind::field_place) {
      auto base = x.children[0];
      while (n(base).kind() == NodeKind::field_place)
        base = n(base).children[0];
      if (auto p = std::get_if<Place>(&n(base).data))
        readable(id, p->symbol, st);
    }
    if (x.kind() == NodeKind::bool_literal) {
      const auto &ref = x.source.node;
      for (const auto &o : m.resolved->occurrences)
        if (o.source.module == ref.module && o.source.file == ref.file &&
            o.source.node == ref.node) {
          auto symbol = o.symbol;
          for (const auto &instance : m.instances)
            if (instance.symbol == fun->symbol)
              for (auto [from, to] : instance.renamed)
                if (from == symbol) {
                  symbol = to;
                  break;
                }
          if (locals.count(symbol.value))
            readable(id, symbol, st);
        }
    }
    if (auto r = std::get_if<Move>(&x.data); r && r->binding) {
      auto l = locals.at(r->binding->value);
      if (l.mode != ParamMode::move)
        issue(id, "E_CALL_ARGUMENT", "borrowed parameter cannot be moved");
      if (borrowed.count(r->binding->value))
        issue(
            id, "E_CALL_ARGUMENT",
            "binding is borrowed by an earlier argument or assignment target");
      readable(id, *r->binding, st);
      out.normal->at(r->binding->value) = 2;
    }
    if (auto sc = std::get_if<ShortCircuit>(&x.data)) {
      auto a = expression(x.children[0], st, borrowed);
      out = a;
      auto known = std::get_if<BoolLiteral>(&n(x.children[0]).data);
      bool take =
          !known || (sc->op == LogicalOp::and_ ? known->value : !known->value);
      if (take && a.normal) {
        auto b = expression(x.children[1], *a.normal, borrowed);
        edges(out, b);
        if (known)
          out.normal = b.normal;
        else if (b.normal)
          join(out.normal, *b.normal);
      }
      return out;
    }
    if (auto args = call_args(x)) {
      if (auto callee = call_callee(x)) {
        auto f = expression(*callee, *out.normal, borrowed);
        out.normal = f.normal;
        edges(out, f);
      }
      for (const auto &a : *args) {
        if (!out.normal)
          break;
        auto f = expression(a.expression, *out.normal, borrowed);
        out.normal = f.normal;
        edges(out, f);
        if (a.mode == ParamMode::ordinary)
          if (auto root = borrow_root(a.expression))
            borrowed.insert(root->value);
      }
      if (auto e = error(x); e && out.normal)
        out.edges.push_back({ExitKind::error, 0, *e, *out.normal});
      return out;
    }
    for (auto c : x.children) {
      if (!out.normal)
        break;
      auto f = expression(c, *out.normal, borrowed);
      out.normal = f.normal;
      edges(out, f);
    }
    if (auto e = error(x); e && out.normal)
      out.edges.push_back({ExitKind::error, 0, *e, *out.normal});
    return out;
  }
  void initialize_pattern(const Pattern &p, State &st) {
    if (p.binding)
      st[p.binding->value] = 1;
    for (auto &c : p.payload)
      initialize_pattern(c, st);
  }
  // Cleanup is interpreted at each reached exit. A defer registration never
  // evaluates its body, and earlier deferred writes affect later defers.
  std::optional<State> cleanup(State st, const std::vector<HirNodeId> &defers,
                               ScopeId scope) {
    for (auto it = defers.rbegin(); it != defers.rend(); ++it) {
      auto f = statement(n(*it).children[0], st);
      if (!f.edges.empty())
        throw std::invalid_argument("escaping ownership cleanup");
      // A legal cleanup can diverge in an internal loop. No earlier defer,
      // outer cleanup, or original return/error/jump is reachable on this path.
      if (!f.normal)
        return {};
      st = *f.normal;
    }
    for (auto [id, l] : locals)
      if (l.scope == scope)
        st.erase(id);
    return st;
  }
  void clean_edges(Flow &flow, const std::vector<HirNodeId> &defers,
                   ScopeId scope) {
    std::erase_if(flow.edges, [&](Edge &edge) {
      auto state = cleanup(edge.state, defers, scope);
      if (!state)
        return true;
      edge.state = std::move(*state);
      return false;
    });
  }
  Flow statement(HirNodeId id, State st) {
    check_cancelled(stop);
    const auto &x = n(id);
    Flow out{st, {}};
    switch (x.kind()) {
    case NodeKind::block: {
      std::vector<HirNodeId> defers;
      for (auto c : x.children) {
        if (!out.normal)
          break;
        if (n(c).kind() == NodeKind::defer_) {
          defers.push_back(c);
          continue;
        }
        auto f = statement(c, *out.normal);
        clean_edges(f, defers, x.scope);
        edges(out, f);
        out.normal = f.normal;
      }
      if (out.normal)
        out.normal = cleanup(*out.normal, defers, x.scope);
      return out;
    }
    case NodeKind::binding: {
      auto f = expression(x.children[0], st);
      if (f.normal)
        (*f.normal)[std::get<Binding>(x.data).symbol.value] = 1;
      return f;
    }
    case NodeKind::assign: {
      auto f = expression(x.children[0], st);
      if (auto p = std::get_if<Place>(&n(x.children[0]).data);
          p && std::get<Assign>(x.data).compound)
        readable(x.children[0], p->symbol, st);
      if (f.normal) {
        std::set<uint64_t> protected_root;
        if (n(x.children[0]).kind() == NodeKind::field_place)
          if (auto root = borrow_root(x.children[0]))
            protected_root.insert(root->value);
        auto rhs = expression(x.children[1], *f.normal, protected_root);
        edges(f, rhs);
        f.normal = rhs.normal;
      }
      if (f.normal)
        if (auto p = std::get_if<Place>(&n(x.children[0]).data))
          (*f.normal)[p->symbol.value] = 1;
      return f;
    }
    case NodeKind::return_:
    case NodeKind::throw_: {
      if (!x.children.empty())
        out = expression(x.children[0], st);
      if (out.normal)
        out.edges.push_back(
            {x.kind() == NodeKind::return_ ? ExitKind::return_
                                           : ExitKind::error,
             fun->symbol.value,
             x.kind() == NodeKind::throw_ ? n(x.children[0]).type : TypeId{0},
             *out.normal});
      out.normal.reset();
      return out;
    }
    case NodeKind::break_:
    case NodeKind::continue_:
      out.edges.push_back({x.kind() == NodeKind::break_ ? ExitKind::break_
                                                        : ExitKind::continue_,
                           x.kind() == NodeKind::break_
                               ? std::get<Break>(x.data).loop.value
                               : std::get<Continue>(x.data).loop.value,
                           TypeId{0}, st});
      out.normal.reset();
      return out;
    case NodeKind::if_: {
      auto cond = expression(x.children[0], st);
      out.normal.reset();
      out.edges = cond.edges;
      auto b = std::get_if<BoolLiteral>(&n(x.children[0]).data);
      if (cond.normal) {
        for (size_t i = 1; i < x.children.size(); ++i)
          if (!b || i == (b->value ? 1u : 2u)) {
            auto f = statement(x.children[i], *cond.normal);
            edges(out, f);
            if (f.normal)
              join(out.normal, *f.normal);
          }
        if (x.children.size() == 2 && (!b || !b->value))
          join(out.normal, *cond.normal);
      }
      return out;
    }
    case NodeKind::try_: {
      auto body = statement(x.children[0], st);
      out.normal = body.normal;
      out.edges.clear();
      const auto &cs = std::get<Try>(x.data).catches;
      std::vector<std::optional<State>> entries(cs.size());
      for (auto &e : body.edges) {
        size_t i = 0;
        if (e.kind == ExitKind::error)
          while (i < cs.size() && cs[i].error_type != e.error)
            ++i;
        else
          i = cs.size();
        if (i == cs.size())
          out.edges.push_back(e);
        else
          join(entries[i], e.state);
      }
      for (size_t i = 0; i < cs.size(); ++i)
        if (entries[i]) {
          (*entries[i])[cs[i].binding.value] = 1;
          auto f = statement(cs[i].body, *entries[i]);
          edges(out, f);
          if (f.normal)
            join(out.normal, *f.normal);
        }
      return out;
    }
    case NodeKind::switch_: {
      auto value = expression(x.children[0], st);
      out.normal.reset();
      out.edges = value.edges;
      if (value.normal)
        for (const auto &a : std::get<Switch>(x.data).arms) {
          auto entry = *value.normal;
          initialize_pattern(a.pattern, entry);
          auto f = statement(a.body, entry);
          if (f.normal)
            join(out.normal, *f.normal);
          for (auto &e : f.edges)
            if (e.kind == ExitKind::break_ && e.target == id.value)
              join(out.normal, e.state);
            else
              out.edges.push_back(e);
        }
      return out;
    }
    case NodeKind::for_:
    case NodeKind::for_in: {
      bool fi = x.kind() == NodeKind::for_in;
      auto init =
          fi ? expression(x.children[0], st) : statement(x.children[0], st);
      out.normal.reset();
      out.edges = init.edges;
      if (!init.normal)
        return out;
      std::optional<State> head = init.normal;
      auto known =
          fi ? nullptr : std::get_if<BoolLiteral>(&n(x.children[1]).data);
      for (size_t iteration = 0; iteration <= locals.size() * 2 + 2;
           ++iteration) {
        auto cond = fi ? Flow{head, {}} : expression(x.children[1], *head);
        edges(out, cond);
        if (!cond.normal)
          break;
        if (fi || !known || !known->value)
          join(out.normal, *cond.normal);
        if (known && !known->value)
          break;
        auto entry = *cond.normal;
        if (fi)
          entry[std::get<ForIn>(x.data).element.value] = 1;
        auto body = statement(x.children[fi ? 1 : 3], entry);
        std::optional<State> back = body.normal;
        for (auto &e : body.edges) {
          if (e.target == id.value && e.kind == ExitKind::break_)
            join(out.normal, e.state);
          else if (e.target == id.value && e.kind == ExitKind::continue_)
            join(back, e.state);
          else
            out.edges.push_back(e);
        }
        if (!fi && back) {
          auto step = statement(x.children[2], *back);
          edges(out, step);
          back = step.normal;
        }
        auto next = head;
        if (back)
          join(next, *back);
        if (next == head)
          break;
        head = next;
        if (iteration == locals.size() * 2 + 2)
          throw std::invalid_argument("ownership loop fixed point budget");
      }
      if (out.normal)
        out.normal = cleanup(*out.normal, {}, x.scope);
      clean_edges(out, {}, x.scope);
      return out;
    }
    case NodeKind::expression_statement:
      return expression(x.children[0], st);
    case NodeKind::noop:
    case NodeKind::defer_:
      return out;
    default:
      return expression(id, st);
    }
  }

public:
  Analysis(const HirModule &module, std::stop_token s)
      : m(module), stop(s), parents(m.nodes.size()) {
    out.exits.resize(m.nodes.size());
  }
  Derived run() {
    std::vector<const HirFunction *> fs;
    for (const auto &f : m.functions)
      fs.push_back(&f);
    for (const auto &t : m.templates)
      fs.push_back(&t.function);
    for (auto f : fs) {
      fun = f;
      auto sig = ty(m, f->signature);
      auto supported = [&](TypeId type) {
        if (ty(m, type).kind == TypeKind::String)
          return;
        auto ops = classify_value(m, type);
        if (ops.kind == ValueKind::unsupported)
          issue(f->body, "E_CAPABILITY_UNAVAILABLE", ops.reason);
      };
      supported(sig.result);
      if (sig.throws_type)
        supported(*sig.throws_type);
      for (size_t i = 0; i < f->parameters.size(); ++i) {
        auto p = sig.parameters[i];
        supported(p.type);
        local(f->parameters[i].symbol, f->scope, p.type, p.mode);
        if (p.mode == ParamMode::move || owned(p.type))
          enabled.insert(f->symbol.value);
      }
      gather(f->body);
      if (!enabled.count(f->symbol.value))
        continue;
      for (auto [sid, l] : locals)
        if (m.scopes.at(l.scope.value).function == f->symbol)
          out.records.locals.push_back(l);
      records(f->body);
      exit_records(f->body);
      State st;
      for (auto p : f->parameters)
        st[p.symbol.value] = 1;
      statement(f->body, st);
    }
    return out;
  }
};
PhaseResult<std::monostate> result(const std::function<void()> &fn,
                                   bool source) {
  PhaseResult<std::monostate> r;
  try {
    fn();
    r.value = std::monostate{};
    r.state = PhaseState::complete;
  } catch (const Cancelled &) {
    r.state = PhaseState::cancelled;
  } catch (const Problem &e) {
    r.failure = source ? (e.code == "E_CAPABILITY_UNAVAILABLE"
                              ? FailureKind::unsupported
                              : FailureKind::source)
                       : FailureKind::internal;
    r.diagnostics.push_back({source ? e.code : "E_IR_INVALID",
                             source ? "type" : "lowering", "error", e.message,
                             e.source.span});
  } catch (const std::exception &e) {
    r.diagnostics.push_back({"E_IR_INVALID", "lowering", "error", e.what()});
  }
  return r;
}
} // namespace
ValueOps classify_value(const HirModule &m, TypeId id) {
  std::set<uint64_t> active;
  return classify(m, id, active);
}
namespace detail {
PhaseResult<std::monostate> produce_ownership(HirModule &m,
                                              std::stop_token stop) {
  return result(
      [&] {
        std::set<uint64_t> active, done;
        auto roots = m.types.size();
        for (size_t i = 1; i <= roots; ++i)
          intern_members(m, TypeId{i}, active, done);
        auto derived = Analysis(m, stop).run();
        m.ownership = std::move(derived.records);
        for (size_t i = 0; i < m.nodes.size(); ++i)
          if (derived.checked_nodes.count(i))
            m.nodes[i].exits = std::move(derived.exits[i]);
      },
      true);
}
PhaseResult<std::monostate> verify_ownership(const HirModule &m,
                                             std::stop_token stop) {
  return result(
      [&] {
        auto derived = Analysis(m, stop).run();
        if (m.ownership != derived.records)
          throw std::invalid_argument(
              "ownership records differ from independently recomputed tree "
              "obligations");
        for (size_t i = 0; i < m.nodes.size(); ++i)
          if (derived.checked_nodes.count(i) &&
              m.nodes[i].exits != derived.exits[i])
            throw std::invalid_argument(
                "ownership exit actions/active output/prefix differ from "
                "independently recomputed obligations");
        for (size_t i = 0; i < m.nodes.size(); ++i)
          if (!derived.checked_nodes.count(i))
            for (const auto &e : m.nodes[i].exits)
              if (!e.actions.empty())
                throw std::invalid_argument(
                    "unexpected ownership action on a trivial function");
        // Empty legacy records are legal only for functions independently found
        // to have no Vec/move/owning aggregate obligation.
      },
      false);
}
} // namespace detail
} // namespace lang::hir
