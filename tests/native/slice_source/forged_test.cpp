// Counterfeit NativeIR metadata for the real Slice source interface must be
// rejected by the compiler's own verifier. The fixture is first checked and
// lowered for real; every mutation below changes only frozen metadata or an
// operation/type identity that must still agree with verified HIR. Operand
// substitutions use existing, dominating, type-correct SSA values; HIR stays
// unchanged.
#include "lang/backend/native.hpp"
#include "lang/sema/check.hpp"
#include <algorithm>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
using namespace lang;

static void require(bool value, const std::string &message) {
  if (!value)
    throw std::runtime_error(message);
}

static bool same_span(SourceSpan a, SourceSpan b) {
  return a.snapshot == b.snapshot && a.begin_byte == b.begin_byte &&
         a.end_byte == b.end_byte;
}

struct Site {
  size_t function = 0, block = 0, instruction = 0;
  bool found = false;
};

int main(int argc, char **argv) {
  try {
    require(argc == 2, "manifest argument");
    SourceManager sources;
    auto project = modules::load_project({argv[1], {}, {}}, sources);
    require(project.success(), "project load failed");
    auto resolved = modules::resolve_project(
        std::make_shared<const modules::Project>(std::move(*project.value)));
    require(resolved.success(), "project resolve failed");
    auto checked = sema::check_project(resolved);
    require(checked.success(), "real check failed");
    auto lowered = ir::lower(*checked.value);
    require(lowered.success(), "real lower failed");
    const auto original = lowered.value->module();

    auto is_slice = [&](TypeId id) {
      const auto *type = original.hir.module().types.find(id);
      return type && type->kind == hir::TypeKind::Slice;
    };
    Site system_site, sub_site, read_site, set_site, check_site;
    std::map<ValueId, TypeId> value_types;
    std::map<ValueId, SourceSpan> value_source;
    for (const auto &fn : original.functions) {
      for (const auto &bb : fn.blocks) {
        for (const auto &p : bb.parameters) {
          value_types.emplace(p.value, p.type);
          value_source.emplace(p.value, fn.source);
        }
        for (const auto &i : bb.instructions)
          if (i.result) {
            value_types.emplace(*i.result, i.type);
            value_source.emplace(*i.result, i.source);
          }
      }
    }
    auto receiver_is_slice = [&](const ir::Instruction &i) {
      if (i.inputs.empty())
        return false;
      auto found = value_types.find(i.inputs[0]);
      return found != value_types.end() && is_slice(found->second);
    };
    for (size_t f = 0; f < original.functions.size(); ++f) {
      const auto &fn = original.functions[f];
      for (size_t b = 0; b < fn.blocks.size(); ++b) {
        const auto &bb = fn.blocks[b];
        for (size_t n = 0; n < bb.instructions.size(); ++n) {
          const auto &i = bb.instructions[n];
          if (i.op == ir::Op::system_call && i.system &&
              *i.system == hir::SystemOperation::slice_view && !system_site.found)
            system_site = {f, b, n, true};
          if (i.op == ir::Op::vec_slice && is_slice(i.type) && i.bits == 3 &&
              !sub_site.found)
            sub_site = {f, b, n, true};
          if (i.op == ir::Op::vec_read && receiver_is_slice(i) && !read_site.found)
            read_site = {f, b, n, true};
          if (i.op == ir::Op::vec_set && receiver_is_slice(i) && !set_site.found)
            set_site = {f, b, n, true};
          if (i.op == ir::Op::vec_check && receiver_is_slice(i) && !check_site.found)
            check_site = {f, b, n, true};
        }
      }
    }
    require(system_site.found && sub_site.found && read_site.found &&
                set_site.found && check_site.found,
            "Slice fixture lacks required IR operations");

    // Dominators over the original CFG. A substituted operand must already be
    // defined on every path to the forged use, exactly like a producer could
    // have selected it from the untouched verified HIR.
    std::map<size_t, std::vector<std::set<size_t>>> dom_cache;
    auto dominators = [&](size_t f) -> const std::vector<std::set<size_t>> & {
      if (auto it = dom_cache.find(f); it != dom_cache.end())
        return it->second;
      const auto &fn = original.functions[f];
      const size_t count = fn.blocks.size();
      std::vector<std::set<size_t>> pred(count);
      for (size_t b = 0; b < count; ++b)
        for (auto target : fn.blocks[b].terminator.targets)
          pred[target.value].insert(b);
      std::set<size_t> reachable;
      std::vector<size_t> stack{0};
      while (!stack.empty()) {
        auto b = stack.back();
        stack.pop_back();
        if (!reachable.insert(b).second)
          continue;
        for (auto target : fn.blocks[b].terminator.targets)
          stack.push_back(target.value);
      }
      std::vector<std::set<size_t>> dom(count, reachable);
      dom[0] = {0};
      bool changed = true;
      while (changed) {
        changed = false;
        for (size_t b : reachable) {
          if (b == 0)
            continue;
          std::set<size_t> next = reachable;
          for (auto p : pred[b])
            if (reachable.contains(p)) {
              std::set<size_t> common;
              std::set_intersection(next.begin(), next.end(), dom[p].begin(),
                                    dom[p].end(),
                                    std::inserter(common, common.end()));
              next = std::move(common);
            }
          next.insert(b);
          if (next != dom[b]) {
            dom[b] = std::move(next);
            changed = true;
          }
        }
      }
      return dom_cache.emplace(f, std::move(dom)).first->second;
    };
    auto alternate = [&](const Site &site, size_t operand)
        -> std::optional<ValueId> {
      require(site.found, "alternate operand site missing");
      const auto &fn = original.functions[site.function];
      const auto &target = fn.blocks[site.block].instructions[site.instruction];
      require(operand < target.inputs.size(), "alternate operand index");
      const ValueId current = target.inputs[operand];
      auto wanted = value_types.find(current);
      auto original_source = value_source.find(current);
      require(wanted != value_types.end() && original_source != value_source.end(),
              "alternate operand type/source missing");
      const auto &dom = dominators(site.function);
      for (size_t b : dom[site.block]) {
        const auto &bb = fn.blocks[b];
        for (size_t n = 0; n < bb.instructions.size(); ++n) {
          if (b == site.block && n >= site.instruction)
            continue;
          const auto &candidate = bb.instructions[n];
          if (!candidate.result || *candidate.result == current ||
              value_types.at(*candidate.result) != wanted->second)
            continue;
          if (!same_span(value_source.at(*candidate.result),
                         original_source->second))
            return *candidate.result;
        }
      }
      return std::nullopt;
    };
    auto site_with = [&](bool want_read, bool want_check, bool want_set,
                         size_t operand) -> std::pair<Site, ValueId> {
      for (size_t f = 0; f < original.functions.size(); ++f) {
        const auto &fn = original.functions[f];
        for (size_t b = 0; b < fn.blocks.size(); ++b) {
          const auto &bb = fn.blocks[b];
          for (size_t n = 0; n < bb.instructions.size(); ++n) {
            const auto &i = bb.instructions[n];
            if (!receiver_is_slice(i) || i.inputs.size() <= operand)
              continue;
            if ((want_read && i.op != ir::Op::vec_read) ||
                (want_check && i.op != ir::Op::vec_check) ||
                (want_set && i.op != ir::Op::vec_set))
              continue;
            Site site{f, b, n, true};
            if (auto chosen = alternate(site, operand))
              return {site, *chosen};
          }
        }
      }
      throw std::runtime_error("no dominating alternate operand site");
    };

    auto [read_index_site, read_index_alt] =
        site_with(true, false, false, 1);
    auto [read_receiver_site, read_receiver_alt] =
        site_with(true, false, false, 0);
    auto [check_index_site, check_index_alt] =
        site_with(false, true, false, 1);
    auto [set_index_site, set_index_alt] =
        site_with(false, false, true, 1);
    auto [set_receiver_site, set_receiver_alt] =
        site_with(false, false, true, 0);

    // A real bounds-check/write pair in one block for matching mutations and
    // for removing the check entirely.
    Site pair_check, pair_set;
    for (size_t f = 0; f < original.functions.size() && !pair_check.found; ++f) {
      const auto &fn = original.functions[f];
      for (size_t b = 0; b < fn.blocks.size() && !pair_check.found; ++b) {
        const auto &bb = fn.blocks[b];
        for (size_t c = 0; c < bb.instructions.size() && !pair_check.found; ++c) {
          const auto &check = bb.instructions[c];
          if (check.op != ir::Op::vec_check || !receiver_is_slice(check) ||
              !check.origin || check.inputs.size() != 2)
            continue;
          for (size_t s = c + 1; s < bb.instructions.size(); ++s) {
            const auto &set = bb.instructions[s];
            if (set.op == ir::Op::vec_set && set.origin == check.origin &&
                set.inputs.size() == 3 && set.inputs[0] == check.inputs[0] &&
                set.inputs[1] == check.inputs[1]) {
              pair_check = {f, b, c, true};
              pair_set = {f, b, s, true};
              break;
            }
          }
        }
      }
    }
    require(pair_check.found && pair_set.found, "check/set pair missing");
    auto pair_alt = alternate(pair_check, 1);
    require(pair_alt.has_value(), "check/set alternate missing");

    size_t rejected = 0;
    auto bad = [&](const char *name, auto mutate) {
      auto m = original;
      mutate(m);
      auto r = ir::verify(m);
      require(!r.success() && !r.value && r.failure == FailureKind::internal &&
                  !r.diagnostics.empty() &&
                  r.diagnostics[0].code == "E_IR_INVALID",
              std::string("counterfeit Slice IR metadata was accepted: ") +
                  name);
      ++rejected;
    };
    auto instruction = [](auto &m, const Site &site) -> ir::Instruction & {
      return m.functions[site.function].blocks[site.block]
          .instructions[site.instruction];
    };

    // Existing metadata/type identity cases.
    bad("system op", [&](auto &m) {
      instruction(m, system_site).system = hir::SystemOperation::slice_len;
    });
    bad("origin", [&](auto &m) {
      instruction(m, system_site).origin.reset();
    });
    bad("slice result type", [&](auto &m) {
      instruction(m, sub_site).type =
          m.hir.module().types.find(instruction(m, sub_site).type)
              ->type_args.at(0);
    });
    bad("slice endpoint bits", [&](auto &m) {
      instruction(m, sub_site).bits ^= 1;
    });
    bad("read inputs", [&](auto &m) {
      auto &i = instruction(m, read_site);
      std::swap(i.inputs[0], i.inputs[1]);
    });
    bad("set result type", [&](auto &m) {
      auto &i = instruction(m, set_site);
      i.type = value_types.at(i.inputs[0]);
    });
    bad("check index", [&](auto &m) {
      auto &i = instruction(m, check_site);
      i.inputs[1] = i.inputs[0];
    });

    // Required source-identity regressions: six independent operand variants,
    // the matching wrong check+set pair, and a removed dominating check.
    bad("slice swapped endpoints", [&](auto &m) {
      auto &i = instruction(m, sub_site);
      std::swap(i.inputs[1], i.inputs[2]);
    });
    bad("read wrong index", [&](auto &m) {
      instruction(m, read_index_site).inputs[1] = read_index_alt;
    });
    bad("read wrong receiver", [&](auto &m) {
      instruction(m, read_receiver_site).inputs[0] = read_receiver_alt;
    });
    bad("check wrong index", [&](auto &m) {
      instruction(m, check_index_site).inputs[1] = check_index_alt;
    });
    bad("set wrong index", [&](auto &m) {
      instruction(m, set_index_site).inputs[1] = set_index_alt;
    });
    bad("set wrong receiver", [&](auto &m) {
      instruction(m, set_receiver_site).inputs[0] = set_receiver_alt;
    });
    bad("matching check+set wrong index", [&](auto &m) {
      instruction(m, pair_check).inputs[1] = *pair_alt;
      instruction(m, pair_set).inputs[1] = *pair_alt;
    });
    bad("removed check", [&](auto &m) {
      auto &instructions =
          m.functions[pair_set.function].blocks[pair_set.block].instructions;
      const size_t check_position = pair_check.instruction;
      const size_t set_position = pair_set.instruction;
      require(check_position < set_position && set_position < instructions.size(),
              "check/set positions");
      instructions.erase(instructions.begin() + check_position);
    });

    std::cout << "rejected=" << rejected << "\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "slice forged metadata test failed: " << error.what()
              << "\n";
    return 1;
  }
}
