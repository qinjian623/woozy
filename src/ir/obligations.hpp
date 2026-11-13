#pragma once
#include "lang/ir/native.hpp"
#include "../hir/pattern_analysis.hpp"
#include <map>
#include <set>
#include <tuple>

namespace lang::ir::detail {
// Required occurrences, derived solely from the immutable HIR control tree.
// This is not machine lowering: it has no native block, value, slot or marker
// inputs. Calls remain possibly successful/failing and conditionals retain both
// edges, just as the unoptimized native CFG does (including literal
// conditions).
struct Obligations {
  // Context records which exit and defer caused each surrounding expansion.
  // Counts therefore cannot be satisfied by moving an obligation to another
  // inline copy of the same source body.
  using Context = std::vector<
      std::tuple<HirNodeId, hir::ExitKind, std::optional<TypeId>, HirNodeId>>;
  using Key =
      std::tuple<Context, HirNodeId, hir::ExitKind, std::optional<TypeId>>;
  std::map<Key, size_t> exits;
  std::map<std::pair<Context, HirNodeId>, size_t> calls;
};
class RequiredExits {
  const hir::HirModule &h;
  Obligations required;
  Obligations::Context context;
  struct Flow {
    bool falls = true;
    std::set<HirNodeId> breaks, continues;
    std::set<std::pair<HirNodeId, uint32_t>> catches;
    void merge(const Flow &other) {
      breaks.insert(other.breaks.begin(), other.breaks.end());
      continues.insert(other.continues.begin(), other.continues.end());
      catches.insert(other.catches.begin(), other.catches.end());
    }
  };
  const hir::HirNode &node(HirNodeId id) const { return h.nodes.at(id.value); }
  Flow exit(HirNodeId id, hir::ExitKind kind,
            std::optional<TypeId> error = {}) {
    Flow flow;
    for (const auto &p : node(id).exits)
      if (p.kind == kind && p.error_type == error) {
        ++required.exits[{context, id, kind, error}];
        for (auto d : p.defers) {
          context.emplace_back(id, kind, error, d);
          flow.merge(stmt(node(d).children[0]));
          context.pop_back();
        }
        if (auto c = std::get_if<hir::CatchTarget>(&p.target))
          flow.catches.emplace(c->try_node, c->index);
      }
    return flow;
  }
  Flow expr(HirNodeId id) {
    const auto &n = node(id);
    Flow flow;
    for (auto child : n.children)
      flow.merge(expr(child));
    if (auto c = std::get_if<hir::Call>(&n.data)) {
      ++required.calls[{context, id}];
      if (auto error = h.types.find(c->signature)->throws_type)
        flow.merge(exit(id, hir::ExitKind::error, error));
    }
    if (n.kind() == hir::NodeKind::await_) {
      const auto *task = h.types.find(node(n.children[0]).type);
      if (!task || task->kind != hir::TypeKind::Task ||
          task->type_args.size() != 2)
        throw std::logic_error("await child is not a Task");
      const auto error = task->type_args[1];
      if (h.types.find(error)->kind != hir::TypeKind::Unit)
        flow.merge(exit(id, hir::ExitKind::error, error));
      flow.merge(exit(id, hir::ExitKind::cancel));
    }
    return flow;
  }
  Flow stmt(HirNodeId id) {
    const auto &n = node(id);
    Flow flow;
    switch (n.kind()) {
    case hir::NodeKind::defer_:
    case hir::NodeKind::noop:
      return flow;
    case hir::NodeKind::block:
      for (auto child : n.children) {
        if (!flow.falls)
          break;
        auto next = stmt(child);
        flow.merge(next);
        flow.falls = next.falls;
      }
      if (flow.falls)
        flow.merge(exit(id, hir::ExitKind::normal));
      return flow;
    case hir::NodeKind::return_:
      if (!n.children.empty())
        flow.merge(expr(n.children[0]));
      flow.merge(exit(id, hir::ExitKind::return_));
      flow.falls = false;
      return flow;
    case hir::NodeKind::throw_:
      flow.merge(expr(n.children[0]));
      flow.merge(exit(id, hir::ExitKind::error, node(n.children[0]).type));
      flow.falls = false;
      return flow;
    case hir::NodeKind::break_:
      flow.merge(exit(id, hir::ExitKind::break_));
      flow.breaks.insert(std::get<hir::Break>(n.data).loop);
      flow.falls = false;
      return flow;
    case hir::NodeKind::continue_:
      flow.merge(exit(id, hir::ExitKind::continue_));
      flow.continues.insert(std::get<hir::Continue>(n.data).loop);
      flow.falls = false;
      return flow;
    case hir::NodeKind::if_: {
      flow.merge(expr(n.children[0]));
      auto yes = stmt(n.children[1]);
      auto no = n.children.size() == 3 ? stmt(n.children[2]) : Flow{};
      flow.merge(yes);
      flow.merge(no);
      flow.falls = yes.falls || no.falls;
      return flow;
    }
    case hir::NodeKind::for_in: {
      flow.merge(expr(n.children[0]));
      const auto &fi = std::get<hir::ForIn>(n.data);
      if (fi.iterator)
        flow.merge(expr(fi.iterator->next));
      auto body = stmt(fi.body); flow.merge(body);
      flow.breaks.erase(id); flow.continues.erase(id);
      flow.merge(exit(id, hir::ExitKind::normal)); flow.falls = true;
      return flow;
    }
    case hir::NodeKind::for_: {
      flow = stmt(n.children[0]);
      if (!flow.falls)
        return flow;
      flow.merge(expr(n.children[1]));
      auto body = stmt(n.children[3]);
      if (body.falls || body.continues.contains(id))
        flow.merge(stmt(n.children[2]));
      flow.merge(body);
      flow.breaks.erase(id);
      flow.continues.erase(id);
      flow.merge(exit(id, hir::ExitKind::normal));
      flow.falls = true;
      return flow;
    }
    case hir::NodeKind::switch_: {
      flow.merge(expr(n.children[0]));
      flow.falls = false;
      hir::TypeContext types = h.types;
      hir::PatternCoverage coverage(
          node(n.children[0]).type,
          [&](TypeId t) { return hir::pattern_domain(h, t, types); },
          std::stop_token{});
      for (const auto &arm : std::get<hir::Switch>(n.data).arms) {
        // Recompute nested arm reachability with the same constructor-column
        // analysis as semantic checking and HIR verification. A flat
        // outer-variant set cannot decide when the same outer variant has
        // several payload combinations.
        if (!coverage.add(arm.pattern))
          break;
        auto branch = stmt(arm.body);
        flow.merge(branch);
        flow.falls = flow.falls || branch.falls;
      }
      if (flow.breaks.erase(id))
        flow.falls = true;
      return flow;
    }
    case hir::NodeKind::try_: {
      flow = stmt(n.children[0]);
      const auto &cs = std::get<hir::Try>(n.data).catches;
      for (uint32_t c = 0; c < cs.size(); ++c)
        if (flow.catches.erase({id, c})) {
          auto handler = stmt(cs[c].body);
          flow.merge(handler);
          flow.falls = flow.falls || handler.falls;
        }
      return flow;
    }
    default:
      return expr(id);
    }
  }

public:
  explicit RequiredExits(const hir::HirModule &input) : h(input) {}
  Obligations run(const hir::HirFunction &function) {
    stmt(function.body);
    return std::move(required);
  }
};
} // namespace lang::ir::detail
