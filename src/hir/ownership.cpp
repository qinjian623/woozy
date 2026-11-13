#include "ownership.hpp"
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace lang::hir {
bool inout_data_type(const HirModule &, TypeId,
                     const std::set<uint64_t> &);
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
constexpr size_t kCompletionDepth = 128;
constexpr size_t kCompletionTypes = 4096;
constexpr size_t kClassificationDepth = 512;
constexpr size_t kClassificationTypes = 65536;

bool contains_type_parameter(const HirModule &m, TypeId root) {
  std::vector<TypeId> pending{root};
  std::set<uint64_t> seen;
  while (!pending.empty()) {
    auto id = pending.back();
    pending.pop_back();
    if (!seen.insert(id.value).second)
      continue;
    if (seen.size() > kClassificationTypes)
      throw std::invalid_argument(
          "concrete ownership type traversal budget exceeded");
    const auto &t = ty(m, id);
    if (t.kind == TypeKind::TypeParameter)
      return true;
    pending.insert(pending.end(), t.type_args.begin(), t.type_args.end());
    for (auto p : t.parameters)
      pending.push_back(p.type);
    if (t.kind == TypeKind::Function) {
      pending.push_back(t.result);
      if (t.throws_type)
        pending.push_back(*t.throws_type);
    }
  }
  return false;
}

// A growing concrete family such as Grow[T] -> Vec[Grow[Vec[T]]] creates a
// new TypeId at every expansion.  The producer stops within this fixed budget
// and reports the source location of the actual root that demanded the type.
struct RootBudget {
  Source source;
  std::set<uint64_t> expanded;
  void account(TypeId id, size_t depth) {
    if (depth > kCompletionDepth)
      throw Problem{source, "E_CAPABILITY_UNAVAILABLE",
                    "concrete ownership member/type expansion budget exceeded"};
    if (expanded.insert(id.value).second && expanded.size() > kCompletionTypes)
      throw Problem{source, "E_CAPABILITY_UNAVAILABLE",
                    "concrete ownership member/type expansion budget exceeded"};
  }
};

void intern_members(HirModule &m, TypeId id, std::set<uint64_t> &active,
                    std::set<uint64_t> &done, RootBudget &budget, size_t depth,
                    std::stop_token stop) {
  check_cancelled(stop);
  if (done.count(id.value))
    return;
  // An exact concrete TypeId back-edge is the finite reference cycle we now
  // support.  It ends this branch before counting a new expansion.
  if (active.count(id.value))
    return;
  budget.account(id, depth);
  auto t = ty(m, id);
  if (t.kind == TypeKind::Vec || t.kind == TypeKind::Slice) {
    auto element = t.type_args.at(0);
    // A Vec/Slice element that still contains a real TypeParameter is deferred
    // to the concrete instance. Both already own retain/release work.
    if (!contains_type_parameter(m, element))
      intern_members(m, element, active, done, budget, depth + 1, stop);
    done.insert(id.value);
    return;
  }
  if (t.kind != TypeKind::NominalStruct && t.kind != TypeKind::NominalEnum) {
    done.insert(id.value);
    return;
  }
  const auto &d = decl(m, t.declaration);
  if (t.type_args.size() != d.params.size())
    throw std::invalid_argument("ownership nominal type argument count");
  active.insert(id.value);
  Substitutions sub;
  for (size_t i = 0; i < d.params.size(); ++i)
    sub.push_back({d.params[i].symbol, t.type_args.at(i)});
  auto fields = [&](const auto &fs) {
    for (const auto &f : fs) {
      auto concrete = substitute(f.type, sub, m.types);
      intern_members(m, concrete, active, done, budget, depth + 1, stop);
    }
  };
  fields(d.fields);
  for (const auto &v : d.variants)
    fields(v.payload);
  active.erase(id.value);
  done.insert(id.value);
}

void complete_root(HirModule &m, TypeId root, const Source &source,
                   std::set<uint64_t> &done, std::stop_token stop) {
  check_cancelled(stop);
  RootBudget budget{source};
  std::set<uint64_t> active;
  auto complete = [&](TypeId id) {
    intern_members(m, id, active, done, budget, 0, stop);
  };
  // Copy the root Type before completing any member: substitution interns new
  // concrete types into m.types and may reallocate its storage, invalidating a
  // reference into it.  TypeId fields in this value remain stable indices.
  const Type t = ty(m, root);
  if (t.kind == TypeKind::Function) {
    if (t.throws_type)
      complete(*t.throws_type);
    for (auto p : t.parameters)
      complete(p.type);
    complete(t.result);
  } else {
    complete(root);
  }
}

void complete_pattern(HirModule &m, const Pattern &pattern,
                      std::set<uint64_t> &done, std::stop_token stop) {
  complete_root(m, pattern.type, pattern.source, done, stop);
  for (const auto &child : pattern.payload)
    complete_pattern(m, child, done, stop);
}

void complete_function(HirModule &m, const HirFunction &function,
                       std::set<uint64_t> &done, std::stop_token stop) {
  complete_root(m, function.signature, function.source, done, stop);
  std::vector<HirNodeId> pending{function.body};
  std::set<uint64_t> seen;
  while (!pending.empty()) {
    check_cancelled(stop);
    auto id = pending.back();
    pending.pop_back();
    if (!seen.insert(id.value).second)
      continue;
    const auto &n = m.nodes.at(id.value);
    complete_root(m, n.type, n.source, done, stop);
    if (auto call = std::get_if<Call>(&n.data))
      complete_root(m, call->signature, n.source, done, stop);
    if (auto call = std::get_if<ConstraintCall>(&n.data)) {
      complete_root(m, call->signature, n.source, done, stop);
      complete_root(m, call->receiver, n.source, done, stop);
    }
    if (auto attempts = std::get_if<Try>(&n.data))
      for (const auto &c : attempts->catches)
        complete_root(m, c.error_type, c.source, done, stop);
    if (auto branches = std::get_if<Switch>(&n.data))
      for (const auto &arm : branches->arms)
        complete_pattern(m, arm.pattern, done, stop);
    if (auto value = std::get_if<FunctionValue>(&n.data)) {
      for (const auto &capture : value->captures)
        complete_root(m, capture.type, n.source, done, stop);
      if (value->bound)
        complete_root(m, value->bound->type, n.source, done, stop);
    }
    pending.insert(pending.end(), n.children.begin(), n.children.end());
  }
}

ValueKind provisional_kind(const Type &t) {
  switch (t.kind) {
  case TypeKind::ThreadHandle:
  case TypeKind::MutexHandle:
  case TypeKind::CondvarHandle:
  case TypeKind::AtomicI64Handle:
  case TypeKind::IpcHandle:
  case TypeKind::Task:
    return ValueKind::runtime_handle;
  case TypeKind::Vec:
    return ValueKind::vec;
  case TypeKind::Slice:
    return ValueKind::slice;
  case TypeKind::Function:
    return ValueKind::function;
  case TypeKind::TraitObject:
    return ValueKind::trait_object;
  case TypeKind::NominalStruct:
    return ValueKind::structure;
  case TypeKind::NominalEnum:
    return ValueKind::enumeration;
  case TypeKind::TypeParameter:
    return ValueKind::dependent;
  case TypeKind::ForeignOpaque:
    return ValueKind::foreign_reference;
  case TypeKind::ForeignCallable:
    return ValueKind::foreign_callable;
  case TypeKind::CForeignPointer:
    return ValueKind::c_pointer;
  case TypeKind::CForeignBuffer:
    return ValueKind::c_buffer;
  case TypeKind::CForeignRecord:
    return ValueKind::c_record;
  case TypeKind::CForeignResource:
    return ValueKind::c_resource;
  case TypeKind::CForeignCallback:
    return ValueKind::c_callback;
  case TypeKind::CForeignOpaque:
    return ValueKind::c_pointer;
  case TypeKind::Error:
    return ValueKind::unsupported;
  default:
    return ValueKind::trivial;
  }
}

// Support is a graph reachability property over reference storage (Vec
// elements and nominal fields).  A seen TypeId is a real reference back-edge;
// all remaining branches are still checked by the outer visit.  needs_drop is
// deliberately computed by a separate value-storage walk below, so a
// temporary back-edge result is never cached as a completed classification.
class Classifier {
  const HirModule &m;
  TypeContext scratch;
  std::set<uint64_t> support_seen;
  size_t support_count = 0;

  struct Support {
    ValueKind kind = ValueKind::unsupported;
    std::string reason;
  };

  Support support(TypeId id, size_t depth, std::vector<ValueField> *out) {
    if (depth > kClassificationDepth)
      throw std::invalid_argument(
          "concrete ownership type expansion budget exceeded");
    const auto &t = ty(m, id);
    if (t.kind == TypeKind::TypeParameter)
      return {ValueKind::dependent, {}};
    if (t.kind == TypeKind::String)
      return {ValueKind::string, {}};
    if (t.kind == TypeKind::ThreadHandle ||
        t.kind == TypeKind::MutexHandle ||
        t.kind == TypeKind::CondvarHandle ||
        t.kind == TypeKind::AtomicI64Handle ||
        t.kind == TypeKind::IpcHandle || t.kind == TypeKind::Task)
      return {ValueKind::runtime_handle, {}};
    if (t.kind == TypeKind::ForeignOpaque ||
        t.kind == TypeKind::ForeignCallable) {
      if (t.kind == TypeKind::ForeignCallable)
        return {ValueKind::foreign_callable, {}};
      const auto &d = m.resolved->project->foreign_binding(t.binding);
      return {std::get<modules::BindingType>(d.detail).kind == "runtime_error"
                  ? ValueKind::foreign_error
                  : ValueKind::foreign_reference,
              {}};
    }
    if (t.kind == TypeKind::CForeignPointer)
      return {ValueKind::c_pointer, {}};
    if (t.kind == TypeKind::CForeignBuffer)
      return {ValueKind::c_buffer, {}};
    if (t.kind == TypeKind::CForeignRecord)
      return {ValueKind::c_record, {}};
    if (t.kind == TypeKind::CForeignCallback)
      return {ValueKind::c_callback, {}};
    if (t.kind == TypeKind::CForeignOpaque)
      return {ValueKind::c_pointer, {}};
    if (t.kind == TypeKind::CForeignResource)
      return {ValueKind::c_resource, {}};
    if (t.kind == TypeKind::Weak)
      return {ValueKind::weak, {}};
    // A function value is an owning reference to code plus an optional shared
    // environment. It is never trivial, including as an aggregate member.
    if (t.kind == TypeKind::Function)
      return {ValueKind::function, {}};
    if (t.kind == TypeKind::TraitObject)
      return {ValueKind::trait_object, {}};
    if (t.kind == TypeKind::Error)
      return {ValueKind::unsupported,
              "dynamic string/foreign/error value ownership is not supported"};
    if (t.kind != TypeKind::Vec && t.kind != TypeKind::Slice &&
        t.kind != TypeKind::NominalStruct &&
        t.kind != TypeKind::NominalEnum)
      return {ValueKind::trivial, {}};
    if (support_seen.count(id.value))
      return {provisional_kind(t), {}};
    if (++support_count > kClassificationTypes)
      throw std::invalid_argument(
          "concrete ownership type expansion budget exceeded");
    support_seen.insert(id.value);
    if (t.kind == TypeKind::Vec || t.kind == TypeKind::Slice) {
      const auto container = t.kind == TypeKind::Vec ? ValueKind::vec
                                                      : ValueKind::slice;
      auto element = t.type_args.at(0);
      if (contains_type_parameter(m, element))
        return {container, {}};
      auto e = support(element, depth + 1, nullptr);
      // A declared py.Error is a finite owning reference: copies share the
      // same runtime payload and release only the language element slot.
      // Foreign callables remain unavailable because they are not storable
      // source values and cannot be copied or dropped independently.
      if (e.kind == ValueKind::unsupported ||
          e.kind == ValueKind::foreign_callable)
        return {ValueKind::unsupported,
                e.reason.empty() ? "external callable values in Vec/Slice are not supported"
                                 : e.reason};
      return {container, {}};
    }
    const auto &d = decl(m, t.declaration);
    if (t.type_args.size() != d.params.size())
      throw std::invalid_argument("ownership nominal type argument count");
    Substitutions sub;
    for (size_t i = 0; i < d.params.size(); ++i)
      sub.push_back({d.params[i].symbol, t.type_args.at(i)});
    Support result{t.kind == TypeKind::NominalStruct ? ValueKind::structure
                                                     : ValueKind::enumeration,
                   {}};
    auto fields = [&](const auto &fs, std::optional<SymbolId> variant) {
      for (const auto &f : fs) {
        auto concrete = substitute(f.type, sub, scratch);
        if (!m.types.find(concrete))
          throw std::invalid_argument(
              "concrete ownership member type was not interned publicly");
        if (out)
          out->push_back({variant, f.symbol, concrete});
        auto s = support(concrete, depth + 1, nullptr);
        // py.Error fields are shared owning references, like ordinary host
        // references. Foreign callables are still not valid aggregate fields.
        if (s.kind == ValueKind::unsupported ||
            s.kind == ValueKind::foreign_callable) {
          result.kind = ValueKind::unsupported;
          if (result.reason.empty())
            result.reason = s.reason.empty()
                                ? "external callable aggregate fields are not supported"
                                : s.reason;
        }
      }
    };
    fields(d.fields, {});
    for (const auto &v : d.variants)
      fields(v.payload, v.symbol);
    return result;
  }

  bool needs_drop(TypeId id, std::set<uint64_t> &active, size_t depth) {
    if (depth > kClassificationDepth)
      throw std::invalid_argument(
          "concrete ownership value dependency budget exceeded");
    const auto &t = ty(m, id);
    if (t.kind == TypeKind::Vec || t.kind == TypeKind::Slice ||
        t.kind == TypeKind::String)
      return true;
    if (t.kind == TypeKind::ThreadHandle ||
        t.kind == TypeKind::MutexHandle ||
        t.kind == TypeKind::CondvarHandle ||
        t.kind == TypeKind::AtomicI64Handle ||
        t.kind == TypeKind::IpcHandle || t.kind == TypeKind::Task)
      return true;
    if (t.kind == TypeKind::ForeignOpaque ||
        t.kind == TypeKind::ForeignCallable)
      return true;
    if (t.kind == TypeKind::CForeignResource)
      return true;
    if (t.kind == TypeKind::Weak)
      return true;
    if (t.kind == TypeKind::Function)
      return true;
    if (t.kind == TypeKind::TraitObject)
      return true;
    if (t.kind == TypeKind::CForeignPointer ||
        t.kind == TypeKind::CForeignBuffer ||
        t.kind == TypeKind::CForeignRecord ||
        t.kind == TypeKind::CForeignCallback ||
        t.kind == TypeKind::CForeignOpaque)
      return false;
    if (t.kind != TypeKind::NominalStruct && t.kind != TypeKind::NominalEnum)
      return false;
    // Value storage recursion that does not pass through Vec has no finite
    // layout.  A caller may invoke this classifier directly on malformed HIR;
    // stop instead of looping or publishing a false completed result.
    if (!active.insert(id.value).second)
      throw std::invalid_argument("cyclic value ownership dependency");
    const auto &d = decl(m, t.declaration);
    if (t.type_args.size() != d.params.size()) {
      active.erase(id.value);
      throw std::invalid_argument("ownership nominal type argument count");
    }
    Substitutions sub;
    for (size_t i = 0; i < d.params.size(); ++i)
      sub.push_back({d.params[i].symbol, t.type_args.at(i)});
    bool result = false;
    auto fields = [&](const auto &fs) {
      for (const auto &f : fs) {
        auto concrete = substitute(f.type, sub, scratch);
        if (!m.types.find(concrete))
          throw std::invalid_argument(
              "concrete ownership member type was not interned publicly");
        result |= needs_drop(concrete, active, depth + 1);
      }
    };
    fields(d.fields);
    for (const auto &v : d.variants)
      fields(v.payload);
    active.erase(id.value);
    return result;
  }

public:
  explicit Classifier(const HirModule &module)
      : m(module), scratch(module.types) {}
  ValueOps run(TypeId id) {
    std::vector<ValueField> fields;
    auto support_result = support(id, 0, &fields);
    ValueOps out;
    out.fields = std::move(fields);
    const auto &t = ty(m, id);
    if (t.kind == TypeKind::TypeParameter) {
      out.kind = ValueKind::dependent;
      return out;
    }
    if (t.kind == TypeKind::String) {
      out.kind = ValueKind::string;
      out.needs_drop = true;
      return out;
    }
    if (t.kind == TypeKind::ThreadHandle ||
        t.kind == TypeKind::MutexHandle ||
        t.kind == TypeKind::CondvarHandle ||
        t.kind == TypeKind::AtomicI64Handle ||
        t.kind == TypeKind::IpcHandle || t.kind == TypeKind::Task) {
      out.kind = ValueKind::runtime_handle;
      out.needs_drop = true;
      return out;
    }
    if (t.kind == TypeKind::ForeignOpaque ||
        t.kind == TypeKind::ForeignCallable) {
      out.kind = support_result.kind;
      out.needs_drop = true;
      return out;
    }
    if (t.kind == TypeKind::CForeignResource) {
      out.kind = ValueKind::c_resource;
      out.needs_drop = true;
      return out;
    }
    if (t.kind == TypeKind::Weak) {
      out.kind = ValueKind::weak;
      out.needs_drop = true;
      out.pod_inout = false;
      return out;
    }
    if (t.kind == TypeKind::Function) {
      out.kind = ValueKind::function;
      out.needs_drop = true;
      out.pod_inout = false;
      return out;
    }
    if (t.kind == TypeKind::TraitObject) {
      out.kind = ValueKind::trait_object;
      out.needs_drop = true;
      out.pod_inout = false;
      return out;
    }
    if (t.kind == TypeKind::CForeignPointer ||
        t.kind == TypeKind::CForeignBuffer ||
        t.kind == TypeKind::CForeignRecord ||
        t.kind == TypeKind::CForeignCallback ||
        t.kind == TypeKind::CForeignOpaque) {
      out.kind = support_result.kind;
      return out;
    }
    if (t.kind == TypeKind::Error) {
      out.kind = ValueKind::unsupported;
      out.reason = support_result.reason;
      return out;
    }
    if (t.kind != TypeKind::Vec && t.kind != TypeKind::Slice &&
        t.kind != TypeKind::NominalStruct &&
        t.kind != TypeKind::NominalEnum) {
      out.kind = ValueKind::trivial;
      out.pod_inout = t.kind == TypeKind::Bool ||
                      t.kind == TypeKind::SignedInt ||
                      t.kind == TypeKind::UnsignedInt ||
                      t.kind == TypeKind::Isize || t.kind == TypeKind::Usize ||
                      t.kind == TypeKind::Float;
      return out;
    }
    out.kind = support_result.kind;
    out.reason = support_result.reason;
    if (t.kind == TypeKind::Vec || t.kind == TypeKind::Slice) {
      out.needs_drop = out.kind != ValueKind::unsupported;
      return out;
    }
    std::set<uint64_t> active;
    out.needs_drop = needs_drop(id, active, 0);
    out.pod_inout = inout_data_type(m, id);
    return out;
  }
};
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
  // Computed owning switch subjects: arm scope -> the real subject result
  // temporary. Exiting the arm scope drops it after the arm's defers and
  // local/pattern bindings, before any outer scope cleanup. The subject is
  // never a fabricated binding; only its genuine Temporary record is used.
  std::map<uint64_t, HirNodeId> switch_subjects;
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
  static bool inert_function_value(const HirNode &n) {
    if (n.kind() != NodeKind::function_value)
      return false;
    const auto &value = std::get<FunctionValue>(n.data);
    return value.captures.empty() && !value.bound;
  }
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
  void local(SymbolId s, ScopeId scope, TypeId type, ParamMode mode,
             bool read_only_capture = false) {
    auto order = uint32_t(scope_locals[scope.value].size());
    locals.emplace(s.value,
                   LocalLifetime{s, scope, type, order, mode,
                                 read_only_capture});
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
    auto ops = classify_value(m, x.type);
    if (ops.kind == ValueKind::unsupported)
      issue(id, "E_CAPABILITY_UNAVAILABLE", ops.reason);
    // A noncapturing function value is classified as owning (C5) but has a
    // null environment, so its copy/drop is a backend no-op. It therefore
    // does not by itself create owned temporaries/cleanup records.
    const bool inert_function = inert_function_value(x);
    if (ops.needs_drop || x.kind() == NodeKind::move ||
        x.kind() == NodeKind::for_in)
      if (!inert_function)
        enabled.insert(fun->symbol.value);
    if (auto fi = std::get_if<ForIn>(&x.data)) {
      auto collection = ty(m, n(x.children[0]).type);
      TypeId element{};
      if (fi->iterator) {
        // The private iterator copy lives in the loop scope; the Option result
        // is recreated in the body scope for every next call. Registering them
        // in this order makes element, option, then cursor the reverse cleanup
        // order at the corresponding exits.
        local(fi->iterator->cursor, x.scope, fi->iterator->iterator,
              ParamMode::move);
        local(fi->iterator->option, fi->body_scope,
              n(fi->iterator->next).type, ParamMode::move);
        element = fi->iterator->element;
      } else if (collection.kind == TypeKind::String) {
        for (uint64_t i = 1; i <= m.types.size(); ++i) {
          const auto &candidate = ty(m, TypeId{i});
          if (candidate.kind == TypeKind::UnsignedInt && candidate.bits == 32) {
            element = TypeId{i}; break;
          }
        }
        if (!element.value) throw std::invalid_argument("String iteration has no public u32 type");
      } else element = collection.type_args[0];
      local(fi->element, fi->body_scope, element, ParamMode::move);
    }
    if (auto sw = std::get_if<Switch>(&x.data))
      for (const auto &a : sw->arms)
        pattern(a.pattern, a.scope);
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
  std::set<uint32_t> safe_borrow_receivers;
  std::set<uint64_t> parameter_symbols;
  // The evaluated expression root of the full expression currently being
  // recorded, and its moved/inout/rebound roots collected once. expr_records
  // visits one full expression many times; nested index reads must not rescan
  // a large tree for every receiver.
  std::optional<HirNodeId> full_expression_root;
  std::map<uint64_t, std::set<uint64_t>> full_expression_effects;

  bool is_borrowed_param(SymbolId sym) const {
    if (!parameter_symbols.count(sym.value))
      return false;
    auto it = locals.find(sym.value);
    if (it == locals.end())
      return false;
    return it->second.mode != ParamMode::move;
  }

  bool expr_consumes_symbol(HirNodeId expr_id, SymbolId sym) const {
    const auto &x = n(expr_id);
    if (auto r = std::get_if<Move>(&x.data); r && r->binding && *r->binding == sym)
      return true;
    if (x.kind() == NodeKind::assign) {
      if (auto p = std::get_if<Place>(&n(x.children[0]).data); p && p->symbol == sym)
        return true;
    }
    if (auto args = call_args(x)) {
      for (const auto &a : *args) {
        if (a.mode == ParamMode::move || a.mode == ParamMode::inout) {
          if (borrow_root(a.expression) == sym)
            return true;
        }
      }
    }
    for (auto c : x.children)
      if (expr_consumes_symbol(c, sym))
        return true;
    return false;
  }

  // Collects the owning roots that this expression may move, borrow as inout,
  // or replace. This mirrors expr_consumes_symbol's effect rules exactly.
  void collect_effects(HirNodeId expr_id, std::set<uint64_t> &out) const {
    const auto &x = n(expr_id);
    if (auto r = std::get_if<Move>(&x.data); r && r->binding)
      out.insert(r->binding->value);
    if (x.kind() == NodeKind::assign)
      if (auto p = std::get_if<Place>(&n(x.children[0]).data))
        out.insert(p->symbol.value);
    if (auto args = call_args(x))
      for (const auto &a : *args)
        if (a.mode == ParamMode::move || a.mode == ParamMode::inout)
          if (auto root = borrow_root(a.expression))
            out.insert(root->value);
    for (auto c : x.children)
      collect_effects(c, out);
  }

  const std::set<uint64_t> &full_effects() {
    auto root = *full_expression_root;
    auto it = full_expression_effects.find(root.value);
    if (it != full_expression_effects.end())
      return it->second;
    std::set<uint64_t> effects;
    collect_effects(root, effects);
    return full_expression_effects.emplace(root.value, std::move(effects))
        .first->second;
  }

  static bool integer_index_kind(TypeKind kind) {
    return kind == TypeKind::SignedInt || kind == TypeKind::UnsignedInt ||
           kind == TypeKind::Isize || kind == TypeKind::Usize;
  }

  // Only a builtin integer tree of literals, integer reads, unary negate/bit
  // complement and integer binary arithmetic/bit/shift may be evaluated while
  // the receiver binding is borrowed. Calls, moves, inout arguments,
  // assignments, conversions and short-circuit forms stay on the copy path.
  // Wrapping overflow and division/remainder Panic rules are unchanged:
  // borrowing only removes an extra handle, not the index evaluation or
  // bounds checks.
  bool pure_integer_index(HirNodeId id) const {
    const auto &x = n(id);
    if (!integer_index_kind(ty(m, x.type).kind))
      return false;
    if (x.kind() == NodeKind::integer_literal || x.kind() == NodeKind::read)
      return true;
    if (x.kind() == NodeKind::unary) {
      auto op = std::get<Unary>(x.data).op;
      return x.children.size() == 1 &&
             (op == UnaryOp::negate || op == UnaryOp::bit_not) &&
             pure_integer_index(x.children[0]);
    }
    if (x.kind() != NodeKind::binary || x.children.size() != 2)
      return false;
    switch (std::get<Binary>(x.data).op) {
    case BinaryOp::add:
    case BinaryOp::subtract:
    case BinaryOp::multiply:
    case BinaryOp::divide:
    case BinaryOp::remainder:
    case BinaryOp::shift_left:
    case BinaryOp::shift_right:
    case BinaryOp::bit_and:
    case BinaryOp::bit_or:
    case BinaryOp::bit_xor:
      return pure_integer_index(x.children[0]) &&
             pure_integer_index(x.children[1]);
    default:
      return false;
    }
  }

  // A direct trivial-element read or direct element write of a plain local
  // Vec may borrow the binding's existing handle when the complete full
  // expression cannot move, inout borrow, or rebind that binding. The binding
  // owns the Vec through the full expression, so the old receiver copy was only
  // an extra non-final handle. Parameters keep their separate rule; fields,
  // computed receivers, owning elements and impure indexes stay on the copy
  // path.
  bool local_index_borrow(HirNodeId receiver, HirNodeId index) const {
    const auto *read = std::get_if<Read>(&n(receiver).data);
    if (!read || !declarations.count(read->symbol.value) ||
        parameter_symbols.count(read->symbol.value))
      return false;
    auto local = locals.find(read->symbol.value);
    if (local == locals.end() ||
        m.scopes.at(local->second.scope.value).function != fun->symbol)
      return false;
    const auto &receiver_type = ty(m, n(receiver).type);
    if (receiver_type.kind != TypeKind::Vec ||
        receiver_type.type_args.size() != 1)
      return false;
    auto element = receiver_type.type_args[0];
    if (contains_type_parameter(m, element) || owned(element))
      return false;
    return pure_integer_index(index);
  }

  // Shared read/write gate: a plain local trivial-element Vec receiver may
  // borrow its binding handle for this complete full expression.
  bool local_index_can_borrow(HirNodeId receiver, HirNodeId index,
                              SymbolId root) {
    return full_expression_root && local_index_borrow(receiver, index) &&
           !full_effects().count(root.value);
  }

  // Ascends only through storage places. Index reads are value expressions,
  // not borrowable storage: an ordinary `text[i]` argument must not borrow
  // the string binding. An inout index place, however, is rooted in its
  // receiver even when that receiver is itself an indexed element.
  std::optional<SymbolId> place_borrow_root(HirNodeId id) const {
    const auto &x = n(id);
    if (auto r = std::get_if<Read>(&x.data))
      return r->symbol;
    if (auto r = std::get_if<Place>(&x.data))
      return r->symbol;
    if (x.kind() == NodeKind::field_read || x.kind() == NodeKind::field_place ||
        x.kind() == NodeKind::index_read || x.kind() == NodeKind::index_place)
      return place_borrow_root(x.children[0]);
    return {};
  }
  std::optional<SymbolId> borrow_root(HirNodeId id) const {
    const auto &x = n(id);
    if (auto r = std::get_if<Read>(&x.data))
      return r->symbol;
    if (auto r = std::get_if<Place>(&x.data))
      return r->symbol;
    if (x.kind() == NodeKind::field_read || x.kind() == NodeKind::field_place)
      return borrow_root(x.children[0]);
    if (x.kind() == NodeKind::index_place)
      return place_borrow_root(x.children[0]);
    return {};
  }
  // Assignment targets keep the old direct-field protection: an indexed
  // field's receiver and index are evaluated before the RHS and the element
  // address is already fixed, so moving the container there is not an active
  // borrow of the target storage. Inout argument borrows use borrow_root.
  std::optional<SymbolId> assignment_field_root(HirNodeId id) const {
    const auto &x = n(id);
    if (auto r = std::get_if<Read>(&x.data))
      return r->symbol;
    if (auto r = std::get_if<Place>(&x.data))
      return r->symbol;
    if (x.kind() == NodeKind::field_read || x.kind() == NodeKind::field_place)
      return assignment_field_root(x.children[0]);
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
    // A noncapturing function value has no environment storage to borrow.
    if (kind == UseKind::borrow && inert_function_value(n(child)))
      return;
    // A computed trivial value has no storage to borrow. In particular a
    // Move result is distinct from its now-consumed source binding.
    if (kind == UseKind::borrow && !owned(n(child).type) && !borrow_root(child))
      return;
    if (!owned(n(child).type) && n(child).kind() != NodeKind::move &&
        kind != UseKind::inout &&
        !(kind == UseKind::borrow && borrow_root(child)))
      return;
    std::optional<SymbolId> root = borrow_root(child);
    if (auto r = std::get_if<Read>(&n(child).data))
      root = r->symbol;
    if (auto r = std::get_if<Place>(&n(child).data))
      root = r->symbol;
    if (auto r = std::get_if<Move>(&n(child).data);
        r && kind != UseKind::borrow)
      root = r->binding;
    out.records.uses.push_back({id, operand, kind, root, n(child).type});
  }
  const std::vector<CallArg> *call_args(const HirNode &x) const {
    if (auto c = std::get_if<Call>(&x.data))
      return &c->args;
    if (auto c = std::get_if<ConstraintCall>(&x.data))
      return &c->args;
    if (auto c = std::get_if<TraitDispatch>(&x.data))
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
      if (auto callee = call_callee(x)) {
        auto root = borrow_root(*callee);
        const bool borrowed = root.has_value();
        expr_records(*callee, fe, live, borrowed);
        if (borrowed)
          active_borrows.push_back({CleanupKind::end_borrow,
                                    {OwnerKind::binding, root->value},
                                    locals.at(root->value).type});
        else if (owned(n(*callee).type) &&
                 !inert_function_value(n(*callee)))
          active_borrows.push_back({CleanupKind::end_borrow,
                                    {OwnerKind::temporary, callee->value},
                                    n(*callee).type});
      }
      for (const auto &a : *args) {
        auto root = a.mode == ParamMode::ordinary ? borrow_root(a.expression)
                                                  : std::nullopt;
        expr_records(a.expression, fe, live, bool(root));
        if (root)
          active_borrows.push_back({CleanupKind::end_borrow,
                                    {OwnerKind::binding, root->value},
                                    locals.at(root->value).type});
        else if (a.mode == ParamMode::ordinary &&
                 owned(n(a.expression).type) &&
                 !inert_function_value(n(a.expression)))
          active_borrows.push_back({CleanupKind::end_borrow,
                                    {OwnerKind::temporary, a.expression.value},
                                    n(a.expression).type});
        else if (a.mode == ParamMode::inout) {
          auto root = borrow_root(a.expression);
          if (root)
            active_borrows.push_back(
                {CleanupKind::end_borrow,
                 {OwnerKind::binding, root->value},
                 n(a.expression).type});
        }
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
    } else if (x.kind() == NodeKind::index_read) {
      auto receiver = x.children[0];
      auto index = x.children[1];
      auto root = borrow_root(receiver);
      bool can_borrow = root && is_borrowed_param(*root) && !expr_consumes_symbol(index, *root);
      if (!can_borrow && root &&
          local_index_can_borrow(receiver, index, *root))
        can_borrow = true;
      expr_records(receiver, fe, live, can_borrow);
      expr_records(index, fe, live, false);
    } else if (x.kind() == NodeKind::index_place) {
      auto receiver = x.children[0];
      auto index = x.children[1];
      bool can_borrow = safe_borrow_receivers.count(receiver.value) > 0;
      expr_records(receiver, fe, live, can_borrow);
      expr_records(index, fe, live, false);
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
          x.kind() == NodeKind::function_value ||
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
        x.kind() != NodeKind::index_place && !inert_function_value(x)) {
      out.records.temporaries.push_back({id, fe, x.type});
      live.push_back(id);
    }
  }
  void full(HirNodeId root, HirNodeId expression, bool transfer) {
    std::vector<HirNodeId> live;
    active_borrows.clear();
    full_expression_root = expression;
    expr_records(expression, root, live);
    if (transfer) {
      use(root, 0, expression, UseKind::take);
      consume(live, expression);
    }
    out.records.full_expressions.push_back({root, temp_cleanup(live)});
    // A call used directly as a condition/switch subject already recorded
    // its failing argument borrows before releasing the successful ones.
    failure_prefix.try_emplace(root.value, temp_cleanup(live));
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
      full_expression_root = id;
      if (n(x.children[0]).kind() == NodeKind::index_place) {
        auto receiver = n(x.children[0]).children[0];
        auto index = n(x.children[0]).children[1];
        auto root = borrow_root(receiver);
        if (root && is_borrowed_param(*root) && !expr_consumes_symbol(index, *root) &&
            !expr_consumes_symbol(x.children[1], *root))
          safe_borrow_receivers.insert(receiver.value);
        else if (root && local_index_can_borrow(receiver, index, *root))
          safe_borrow_receivers.insert(receiver.value);
      }
      expr_records(x.children[0], id, live);
      if (n(x.children[0]).kind() == NodeKind::field_place) {
        auto root = assignment_field_root(x.children[0]);
        if (root)
          active_borrows.push_back({CleanupKind::end_borrow,
                                    {OwnerKind::binding, root->value},
                                    locals.at(root->value).type});
      }
      expr_records(x.children[1], id, live);
      use(id, 0, x.children[0], UseKind::borrow);
      bool text_compound = std::get<Assign>(x.data).compound &&
                           ty(m, n(x.children[0]).type).kind == TypeKind::String;
      use(id, 1, x.children[1], text_compound ? UseKind::borrow : UseKind::take);
      if (!text_compound) consume(live, x.children[1]);
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
      if (fi.iterator) {
        // next is evaluated once per iteration. Its successful result is
        // transferred into the per-iteration Option local, not destroyed at
        // the call full-expression. Some's payload is copied into the source
        // element binding under the ordinary value rules.
        full(fi.iterator->next, fi.iterator->next, true);
        if (owned(type))
          out.records.uses.push_back(
              {id, 0, UseKind::copy, {}, type, fi.element});
      } else if (owned(type))
        out.records.uses.push_back(
            {id, 0, UseKind::copy, {}, type, fi.element});
      records(x.children[1]);
      return;
    }
    case NodeKind::switch_: {
      const auto subject = x.children[0];
      if (owned(n(subject).type)) {
        if (n(subject).kind() == NodeKind::read) {
          auto root = std::get<Read>(n(subject).data).symbol;
          out.records.uses.push_back(
              {id, 0, UseKind::borrow, root, n(subject).type});
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
                 n(subject).type}}});
        } else {
          // Computed owning subject: evaluate it exactly once and register
          // its real result temporary. The switch full expression cleans
          // every other live temporary of the subject expression; the result
          // itself is held per arm and dropped by the arm-scope exit plans.
          std::vector<HirNodeId> live;
          active_borrows.clear();
          full_expression_root = subject;
          expr_records(subject, id, live);
          consume(live, subject);
          out.records.full_expressions.push_back({id, prefix_cleanup(live)});
          std::function<void(const Pattern &, HirNodeId)> copies =
              [&](const Pattern &p, HirNodeId body) {
                if (p.binding && owned(p.type))
                  out.records.uses.push_back(
                      {body, 0, UseKind::copy, {}, p.type, p.binding});
                for (const auto &ch : p.payload)
                  copies(ch, body);
              };
          for (const auto &a : std::get<Switch>(x.data).arms)
            copies(a.pattern, a.body);
          for (const auto &a : std::get<Switch>(x.data).arms)
            switch_subjects.emplace(a.scope.value, subject);
        }
      } else
        full(subject, subject, false);
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
    // TraitDispatch carries an ABI signature without a throws clause.
    if (auto r = std::get_if<ForeignRead>(&x.data))
      return m.foreign_bindings.at(r->binding.value).read_error;
    if (x.kind() == NodeKind::throw_)
      return n(x.children[0]).type;
    if (x.kind() == NodeKind::await_) {
      const auto &task = ty(m, n(x.children[0]).type);
      if (task.type_args.size() != 2)
        throw std::invalid_argument("await task arity");
      const auto error = task.type_args[1];
      if (ty(m, error).kind != TypeKind::Unit)
        return error;
    }
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
            !std::get<ForIn>(block.data).iterator &&
            !contains(block.children[0], id)) {
          p.actions.push_back({CleanupKind::end_iteration,
                               {OwnerKind::iteration, construct.value},
                               TypeId{0}});
          p.actions.push_back({CleanupKind::destroy_if_live,
                               {OwnerKind::iteration, construct.value},
                               n(block.children[0]).type});
        }
        // A computed owning switch subject held by this arm is released
        // after the arm's defers, pattern bindings and local values, and
        // before any enclosing scope cleanup. Inner catches that stay in
        // the arm never reach this walk, so they keep the result alive.
        if (auto it = switch_subjects.find(scope->value);
            it != switch_subjects.end())
          p.actions.push_back({CleanupKind::destroy_if_live,
                               {OwnerKind::temporary, it->second.value},
                               n(it->second).type});
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
    // Every await is a cancellation point. The plan unwinds this function's
    // scopes (defers, move locals and pending temporaries) before the frame
    // returns a cancelled status to the executor.
    if (x.kind() == NodeKind::await_)
      emit(ExitKind::cancel, FunctionExit{fun->symbol}, {}, {}, false);
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
    // Folded local constants still read their source binding.
    if (x.kind() == NodeKind::bool_literal ||
        x.kind() == NodeKind::string_literal ||
        x.kind() == NodeKind::constant_literal) {
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
    // An await can leave through cancellation at any time. The cancel edge
    // carries the same live state as the error edge; the recorded cancel plan
    // describes the defers/drops executed before the frame is released.
    if (x.kind() == NodeKind::await_ && out.normal)
      out.edges.push_back({ExitKind::cancel, fun->symbol.value, TypeId{0},
                           *out.normal});
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
        if (n(x.children[0]).kind() == NodeKind::field_place ||
            (std::get<Assign>(x.data).compound &&
             ty(m, n(x.children[0]).type).kind == TypeKind::String))
          if (auto root = assignment_field_root(x.children[0]))
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
      const ForIn *loop_data = fi ? &std::get<ForIn>(x.data) : nullptr;
      if (loop_data && loop_data->iterator)
        (*head)[loop_data->iterator->cursor.value] = 1;
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
        if (fi) {
          entry[std::get<ForIn>(x.data).element.value] = 1;
          if (loop_data && loop_data->iterator)
            entry[loop_data->iterator->option.value] = 1;
        }
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
        parameter_symbols.insert(f->parameters[i].symbol.value);
        if (p.mode == ParamMode::move || owned(p.type))
          enabled.insert(f->symbol.value);
      }
      // Capture locals are entry-initialized read-only aliases of one
      // environment row field. They are borrowed, never dropped at exit.
      for (const auto &capture : f->captures) {
        supported(capture.type);
        local(capture.local, f->scope, capture.type, ParamMode::ordinary,
              true);
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
      for (const auto &capture : f->captures)
        st[capture.local.value] = 1;
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
bool weak_target(const HirModule &m, TypeId id) {
  const auto *t = m.types.find(id);
  if (!t || !m.resolved || !m.resolved->project)
    return false;
  if (t->kind == TypeKind::Vec || t->kind == TypeKind::Function)
    return true;
  if (t->kind != TypeKind::NominalStruct || !t->declaration.value ||
      t->declaration.value >= m.resolved->symbols.size())
    return false;
  const auto &declaration = m.resolved->symbols[t->declaration.value];
  if (declaration.kind != modules::SymbolKind::type || !declaration.exported ||
      !declaration.definition || declaration.foreign)
    return false;
  const auto &module = m.resolved->project->module(declaration.module);
  if (module.binding)
    return false;
  struct Canonical {
    std::string_view path, name, field;
    TypeKind handle;
    size_t arity;
  };
  static constexpr Canonical canonicals[] = {
      {"std/thread", "Thread", "state", TypeKind::ThreadHandle, 0},
      {"std/sync", "Mutex", "state", TypeKind::MutexHandle, 0},
      {"std/sync", "Condvar", "state", TypeKind::CondvarHandle, 0},
      {"std/sync", "AtomicI64", "state", TypeKind::AtomicI64Handle, 0},
      {"std/ipc", "Memory", "handle", TypeKind::IpcHandle, 0},
      // Box is the only wrapper whose canonical private field is a Vec cell.
      {"std/box", "Box", "cell", TypeKind::Vec, 1},
  };
  const Canonical *canonical = nullptr;
  for (const auto &candidate : canonicals)
    if (module.logical_path == candidate.path &&
        declaration.name == candidate.name &&
        t->type_args.size() == candidate.arity) {
      canonical = &candidate;
      break;
    }
  if (!canonical)
    return false;
  // Require the unique real exported declaration, not a duplicated symbol
  // that merely carries the same module/name fields.
  size_t matches = 0;
  for (const auto &s : m.resolved->symbols)
    if (s.module == declaration.module &&
        s.kind == modules::SymbolKind::type && s.exported && s.definition &&
        s.name == declaration.name)
      ++matches;
  if (matches != 1)
    return false;
  const NominalDecl *nominal = nullptr;
  for (const auto &d : m.nominals)
    if (d.symbol == t->declaration) {
      nominal = &d;
      break;
    }
  if (!nominal || nominal->kind != TypeKind::NominalStruct ||
      nominal->params.size() != canonical->arity ||
      nominal->fields.size() != 1 || !nominal->variants.empty())
    return false;
  const auto &field = nominal->fields.front();
  if (field.exported || m.resolved->symbols.at(field.symbol.value).name !=
                            canonical->field)
    return false;
  const auto *field_type = m.types.find(field.type);
  if (!field_type)
    return false;
  if (canonical->handle != TypeKind::Vec) {
    // The wrapper's single private field is the exact compiler-private
    // runtime handle; a same-shaped user field is not accepted.
    return field_type->kind == canonical->handle;
  }
  if (field_type->kind != TypeKind::Vec || field_type->type_args.size() != 1)
    return false;
  const auto *element = m.types.find(field_type->type_args[0]);
  return element && element->kind == TypeKind::TypeParameter &&
         element->declaration == nominal->params.front().symbol;
}

bool ordinary_copyable(const HirModule &m, TypeId root,
                       std::stop_token stop) {
  // Logical copyability for closure captures and bound receivers. Fn itself
  // shares its environment and is copyable even when the environment contains
  // a non-copyable resource; the resource is only reached by moving it out of
  // a source binding, not by copying the Fn value.
  TypeContext scratch = m.types;
  std::set<uint64_t> active;
  size_t visited = 0;
  std::function<bool(TypeId, size_t)> copyable = [&](TypeId id,
                                                     size_t depth) -> bool {
    check_cancelled(stop);
    if (depth > 128 || ++visited > 65536)
      return false;
    const auto *found = scratch.find(id);
    if (!found)
      return false;
    const Type t = *found;
    switch (t.kind) {
    case TypeKind::Unit:
    case TypeKind::Bool:
    case TypeKind::SignedInt:
    case TypeKind::UnsignedInt:
    case TypeKind::Float:
    case TypeKind::Isize:
    case TypeKind::Usize:
    case TypeKind::String:
    case TypeKind::Vec:
    case TypeKind::Slice:
    case TypeKind::Weak:
    case TypeKind::ThreadHandle:
    case TypeKind::MutexHandle:
    case TypeKind::CondvarHandle:
    case TypeKind::AtomicI64Handle:
    case TypeKind::IpcHandle:
    case TypeKind::Task:
    case TypeKind::Function:
    case TypeKind::TraitObject:
    case TypeKind::ForeignOpaque:
    case TypeKind::ForeignCallable:
    case TypeKind::CForeignPointer:
    case TypeKind::CForeignBuffer:
    case TypeKind::CForeignRecord:
    case TypeKind::CForeignCallback:
    case TypeKind::CForeignOpaque:
      return true;
    case TypeKind::CForeignResource: {
      if (!t.c_type || !m.resolved || !m.resolved->project ||
          t.c_type > m.resolved->project->c_types.size())
        return false;
      return !m.resolved->project->c_types.at(t.c_type - 1)
                  .retain_symbol.empty();
    }
    case TypeKind::TypeParameter:
      return false;
    case TypeKind::Error:
      return false;
    case TypeKind::NominalStruct:
    case TypeKind::NominalEnum:
      break;
    }
    if (!m.resolved || !m.resolved->project)
      return false;
    if (!active.insert(id.value).second)
      return false; // Value-storage cycle has no finite copy rule.
    const NominalDecl *d = nullptr;
    for (const auto &candidate : m.nominals)
      if (candidate.symbol == t.declaration) {
        d = &candidate;
        break;
      }
    if (!d || d->params.size() != t.type_args.size()) {
      active.erase(id.value);
      return false;
    }
    Substitutions sub;
    for (size_t i = 0; i < d->params.size(); ++i)
      sub.push_back({d->params[i].symbol, t.type_args[i]});
    auto fields = [&](const auto &fs) {
      for (const auto &f : fs) {
        auto concrete = substitute(f.type, sub, scratch);
        if (!scratch.find(concrete) || !copyable(concrete, depth + 1)) {
          active.erase(id.value);
          return false;
        }
      }
      return true;
    };
    bool ok = fields(d->fields);
    for (const auto &v : d->variants)
      ok = ok && fields(v.payload);
    active.erase(id.value);
    return ok;
  };
  return m.types.find(root) && copyable(root, 0);
}

bool owning_value(const HirModule &m, TypeId root, std::stop_token stop) {
  // Move-intent classification used while checking a closure creation site.
  // Concrete member types may not be interned publicly until ownership
  // completion, so this walk uses a private substitution context and never
  // publishes records.
  TypeContext scratch = m.types;
  std::set<uint64_t> active;
  size_t visited = 0;
  std::function<bool(TypeId, size_t)> drop = [&](TypeId id,
                                                 size_t depth) -> bool {
    check_cancelled(stop);
    if (depth > 128 || ++visited > 65536)
      return true;
    const auto *found = scratch.find(id);
    if (!found)
      return true;
    const Type t = *found;
    if (t.kind == TypeKind::Vec || t.kind == TypeKind::Slice ||
        t.kind == TypeKind::String || t.kind == TypeKind::Weak ||
        t.kind == TypeKind::ForeignOpaque ||
        t.kind == TypeKind::ForeignCallable ||
        t.kind == TypeKind::CForeignResource ||
        t.kind == TypeKind::Function ||
        t.kind == TypeKind::TraitObject ||
        t.kind == TypeKind::ThreadHandle ||
        t.kind == TypeKind::MutexHandle ||
        t.kind == TypeKind::CondvarHandle ||
        t.kind == TypeKind::AtomicI64Handle ||
        t.kind == TypeKind::IpcHandle)
      return true;
    if (t.kind != TypeKind::NominalStruct && t.kind != TypeKind::NominalEnum)
      return false;
    if (!active.insert(id.value).second)
      return false;
    const NominalDecl *d = nullptr;
    for (const auto &candidate : m.nominals)
      if (candidate.symbol == t.declaration) {
        d = &candidate;
        break;
      }
    if (!d || d->params.size() != t.type_args.size()) {
      active.erase(id.value);
      return true;
    }
    Substitutions sub;
    for (size_t i = 0; i < d->params.size(); ++i)
      sub.push_back({d->params[i].symbol, t.type_args[i]});
    bool result = false;
    auto fields = [&](const auto &fs) {
      for (const auto &f : fs) {
        auto concrete = substitute(f.type, sub, scratch);
        result |= drop(concrete, depth + 1);
      }
    };
    fields(d->fields);
    for (const auto &v : d->variants)
      fields(v.payload);
    active.erase(id.value);
    return result;
  };
  return m.types.find(root) && drop(root, 0);
}

bool inout_data_type(const HirModule &m, TypeId root,
                     const std::set<uint64_t> &parameters) {
  // Substitution may intern storage-only member types. Keep a private copy so
  // no local TypeId escapes and no reference into m.types is held across a
  // possible growth of the context.
  TypeContext types = m.types;
  struct ActiveNominal {
    uint64_t declaration;
    size_t function_depth;
    size_t weight;
  };
  std::vector<ActiveNominal> active;
  std::set<uint64_t> functions;
  std::function<size_t(TypeId, size_t)> type_weight =
      [&](TypeId id, size_t depth) -> size_t {
        if (depth > 128)
          return SIZE_MAX;
        const auto *found = types.find(id);
        if (!found)
          return SIZE_MAX;
        const Type t = *found;
        size_t result = 1;
        for (auto a : t.type_args) {
          auto w = type_weight(a, depth + 1);
          if (w == SIZE_MAX || result > SIZE_MAX - w)
            return SIZE_MAX;
          result += w;
        }
        return result;
      };
  std::function<bool(TypeId, bool, size_t, size_t)> visit =
      [&](TypeId id, bool member, size_t depth, size_t function_depth) {
        if (depth > 128)
          return false;
        const auto *found = types.find(id);
        if (!found)
          return false;
        const Type t = *found;
        if (t.kind == TypeKind::Unit)
          return member;
        if (t.kind == TypeKind::Function) {
          // Function values own their environment and are never POD inout,
          // whether at a top-level slot or as a struct/enum member.
          (void)member;
          (void)functions;
          (void)function_depth;
          return false;
        }
        if (t.kind == TypeKind::Bool || t.kind == TypeKind::SignedInt ||
            t.kind == TypeKind::UnsignedInt || t.kind == TypeKind::Isize ||
            t.kind == TypeKind::Usize || t.kind == TypeKind::Float)
          return true;
        if (t.kind == TypeKind::TypeParameter)
          return parameters.count(t.declaration.value) != 0;
        if (t.kind != TypeKind::NominalStruct &&
            t.kind != TypeKind::NominalEnum)
          return false;
        const NominalDecl *decl = nullptr;
        for (const auto &d : m.nominals)
          if (d.symbol == t.declaration) {
            decl = &d;
            break;
          }
        if (!decl || t.type_args.size() != decl->params.size())
          return false;
        // A nominal that contains itself through value storage has infinite
        // layout and cannot be a finite POD. Vec/foreign recursion is already
        // rejected by the kind checks above.
        const size_t current_weight = type_weight(id, depth);
        for (const auto &prior : active)
          if (prior.declaration == t.declaration.value &&
              prior.function_depth == function_depth &&
              current_weight >= prior.weight)
            return false;
        active.push_back({t.declaration.value, function_depth,
                          current_weight});
        Substitutions sub;
        for (size_t i = 0; i < decl->params.size(); ++i)
          sub.push_back({decl->params[i].symbol, t.type_args[i]});
        auto check = [&](const auto &fields) {
          for (const auto &f : fields) {
            auto field = substitute(f.type, sub, types);
            if (!visit(field, true, depth + 1, function_depth))
              return false;
          }
          return true;
        };
        bool ok = check(decl->fields);
        for (const auto &v : decl->variants)
          ok = ok && check(v.payload);
        active.pop_back();
        return ok;
      };
  return visit(root, false, 0, 0);
}

// Inout reuses each type's copy/drop; POD classification remains separate.
// Task and external lifetimes are not supported here.
const char *inout_slot_reason(InoutSlot kind) {
  switch (kind) {
  case InoutSlot::allowed:
    return "";
  case InoutSlot::unit:
    return "inout of Unit is not a writable value place";
  case InoutSlot::task:
    return "Task inout is not supported: the executor owns its completion "
           "lifetime";
  case InoutSlot::external:
    return "external C resource inout is not supported: no verified "
           "retain/release pairing";
  case InoutSlot::type_parameter:
    return "inout type parameter has no concrete value";
  case InoutSlot::unsupported:
    break;
  }
  return "inout requires a whole Woozy value place: numeric/bool, POD "
         "aggregate, Vec, Slice, string, function value, Weak, dyn trait "
         "object, runtime handle, or an owning struct/enum";
}
InoutSlot inout_slot(const HirModule &m, TypeId root,
                     const std::set<uint64_t> &parameters) {
  // Substitution may intern storage-only member types; the walk keeps its own
  // TypeContext copy so no reference is held across a possible reallocation.
  TypeContext types = m.types;
  std::map<uint64_t, InoutSlot> memo;
  size_t visited = 0;
  std::function<InoutSlot(TypeId, bool, size_t)> visit =
      [&](TypeId id, bool member, size_t depth) -> InoutSlot {
    if (depth > 128 || ++visited > 65536)
      return InoutSlot::unsupported;
    if (auto known = memo.find(id.value); known != memo.end())
      return known->second;
    const auto *found = types.find(id);
    if (!found)
      return InoutSlot::unsupported;
    const Type t = *found;
    InoutSlot result = InoutSlot::allowed;
    switch (t.kind) {
    case TypeKind::Unit:
      result = member ? InoutSlot::allowed : InoutSlot::unit;
      break;
    case TypeKind::Bool:
    case TypeKind::SignedInt:
    case TypeKind::UnsignedInt:
    case TypeKind::Isize:
    case TypeKind::Usize:
    case TypeKind::Float:
    case TypeKind::String:
    case TypeKind::Function:
    case TypeKind::TraitObject:
    case TypeKind::ThreadHandle:
    case TypeKind::MutexHandle:
    case TypeKind::CondvarHandle:
    case TypeKind::AtomicI64Handle:
    case TypeKind::IpcHandle:
      result = InoutSlot::allowed;
      break;
    case TypeKind::Vec:
    case TypeKind::Slice:
    case TypeKind::Weak:
      // Element/owner handles are stored inline, so an unsupported member
      // would make the whole slot's copy/drop unsupported as well.
      for (auto a : t.type_args) {
        auto nested = visit(a, true, depth + 1);
        if (nested != InoutSlot::allowed) {
          result = nested;
          break;
        }
      }
      break;
    case TypeKind::TypeParameter:
      result = parameters.count(t.declaration.value) != 0
                   ? InoutSlot::allowed
                   : InoutSlot::type_parameter;
      break;
    case TypeKind::Task:
      result = InoutSlot::task;
      break;
    case TypeKind::CForeignResource:
    case TypeKind::CForeignPointer:
    case TypeKind::CForeignBuffer:
    case TypeKind::CForeignRecord:
    case TypeKind::CForeignCallback:
    case TypeKind::CForeignOpaque:
    case TypeKind::ForeignOpaque:
    case TypeKind::ForeignCallable:
      result = InoutSlot::external;
      break;
    case TypeKind::Error:
      result = InoutSlot::unsupported;
      break;
    case TypeKind::NominalStruct:
    case TypeKind::NominalEnum: {
      const NominalDecl *decl = nullptr;
      for (const auto &d : m.nominals)
        if (d.symbol == t.declaration) {
          decl = &d;
          break;
        }
      if (!decl || t.type_args.size() != decl->params.size()) {
        result = InoutSlot::unsupported;
        break;
      }
      // A recursive value layout is impossible for a finite struct/enum; a
      // revisit therefore means a handle broke the cycle and is allowed.
      memo.emplace(id.value, InoutSlot::allowed);
      Substitutions sub;
      for (size_t i = 0; i < decl->params.size(); ++i)
        sub.push_back({decl->params[i].symbol, t.type_args[i]});
      auto fields = [&](const auto &fs) {
        InoutSlot outcome = InoutSlot::allowed;
        for (const auto &f : fs) {
          auto concrete = substitute(f.type, sub, types);
          auto nested = visit(concrete, true, depth + 1);
          if (nested != InoutSlot::allowed) {
            outcome = nested;
            break;
          }
        }
        return outcome;
      };
      result = fields(decl->fields);
      for (const auto &v : decl->variants) {
        if (result != InoutSlot::allowed)
          break;
        result = fields(v.payload);
      }
      memo.erase(id.value);
      break;
    }
    }
    memo.emplace(id.value, result);
    return result;
  };
  return visit(root, false, 0);
}

ValueOps classify_value(const HirModule &m, TypeId id) {
  return Classifier(m).run(id);
}
namespace detail {
PhaseResult<std::monostate> produce_ownership(HirModule &m,
                                              std::stop_token stop) {
  return result(
      [&] {
        std::set<uint64_t> done;
        for (const auto &f : m.functions)
          complete_function(m, f, done, stop);
        for (const auto &t : m.templates)
          complete_function(m, t.function, done, stop);
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
