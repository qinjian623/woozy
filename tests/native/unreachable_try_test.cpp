#include "lang/backend/native.hpp"
#include "lang/sema/check.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
using namespace lang;

namespace {
void need(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
void write(const std::filesystem::path &path, const std::string &text) {
  std::ofstream out(path, std::ios::binary);
  out.write(text.data(), text.size());
  need(bool(out), "write " + path.string());
}
bool uses_slot(ir::Op op) {
  switch (op) {
  case ir::Op::load:
  case ir::Op::address:
  case ir::Op::store:
  case ir::Op::move:
  case ir::Op::take:
  case ir::Op::destroy:
  case ir::Op::begin_borrow:
  case ir::Op::iteration_begin:
  case ir::Op::iteration_end:
  case ir::Op::iteration_test:
  case ir::Op::iteration_read:
  case ir::Op::iteration_next:
    return true;
  default:
    return false;
  }
}
const ir::Function *function(const ir::Module &module, const hir::HirModule &hir,
                             const std::string &name) {
  for (const auto &f : module.functions)
    if (hir.resolved->symbols.at(f.symbol.value).name == name)
      return &f;
  return nullptr;
}
size_t slot_uses(const ir::Function &f, size_t slot) {
  size_t uses = 0;
  for (const auto &b : f.blocks)
    for (const auto &i : b.instructions)
      if (uses_slot(i.op) && i.slot.value == slot)
        ++uses;
  return uses;
}
size_t stores(const ir::Function &f, LocalSlotId slot) {
  size_t count = 0;
  for (const auto &b : f.blocks)
    for (const auto &i : b.instructions)
      if (i.op == ir::Op::store && i.slot == slot)
        ++count;
  return count;
}
void dense_checks(const ir::Function &f) {
  for (const auto &b : f.blocks)
    for (const auto &i : b.instructions)
      if (uses_slot(i.op))
        need(i.slot.value < f.slots.size(), "dangling instruction slot");
  for (const auto &handler : f.handlers)
    need(handler.slot.value < f.slots.size(), "dangling handler slot");
}
} // namespace

int main(int argc, char **argv) {
  try {
    need(argc == 3, "source root and output directory required");
    const std::filesystem::path root = argv[1];
    const std::filesystem::path out = argv[2];
    std::filesystem::create_directories(out);
    const auto fixtures = root / "tests/native/unreachable-try";
    size_t checked = 0, rejected = 0;
    std::string mutations;
    for (const auto &entry : std::filesystem::directory_iterator(fixtures)) {
      if (!entry.is_directory() ||
          !std::filesystem::exists(entry.path() / "lang.toml"))
        continue;
      const std::string name = entry.path().filename().string();
      SourceManager sources;
      auto project =
          modules::load_project({(entry.path() / "lang.toml").string(), {}, {}},
                                sources);
      need(project.success(), name + ": load");
      auto resolved = modules::resolve_project(
          std::make_shared<const modules::Project>(std::move(*project.value)));
      auto checked_hir = sema::check_project(resolved);
      if (!checked_hir.success())
        for (const auto &d : checked_hir.diagnostics)
          std::cerr << name << ": " << d.code << ": " << d.message << '\n';
      need(checked_hir.success(), name + ": check");
      auto lowered = ir::lower(*checked_hir.value);
      if (!lowered.success())
        for (const auto &d : lowered.diagnostics)
          std::cerr << name << ": " << d.code << ": " << d.message << '\n';
      need(lowered.success(), name + ": lower/verify");
      const auto &module = lowered.value->module();
      const auto &hir = module.hir.module();
      for (const auto &f : module.functions)
        dense_checks(f);
      std::filesystem::create_directories(out / name);
      write(out / name / "native.txt", ir::dump(module));

      if (name == "return-tail-owning") {
        const auto *main = function(module, hir, "main");
        need(main != nullptr, name + ": main");
        need(main->slots.size() == 2, name + ": tail owning slot survived");
        bool vec = false, temporary_owner = false;
        for (const auto &s : main->slots)
          temporary_owner |= s.owner && s.owner->kind == hir::OwnerKind::temporary;
        for (const auto &b : main->blocks)
          for (const auto &i : b.instructions)
            vec |= i.op == ir::Op::vec_init;
        need(!vec && !temporary_owner,
             name + ": unreachable Vec construction was emitted");
        need(main->slots[0].binding.has_value(),
             name + ": live slot zero was not kept");
      } else if (name == "catch-throws-tail") {
        const auto *run = function(module, hir, "run");
        need(run != nullptr, name + ": run");
        need(run->handlers.size() == 1, name + ": real handler was removed");
        const auto &handler = run->handlers.front();
        need(run->slots[handler.slot.value].owner.has_value(),
             name + ": owning handler slot lost its live flag");
        need(stores(*run, handler.slot) != 0,
             name + ": live handler slot is uninitialized");
        bool vec = false;
        for (const auto &b : run->blocks)
          for (const auto &i : b.instructions)
            vec |= i.op == ir::Op::vec_init;
        need(vec, name + ": reachable tail was removed");
      } else if (name == "dead-catch-nontail") {
        const auto *run = function(module, hir, "run");
        need(run != nullptr, name + ": run");
        need(run->handlers.empty(), name + ": dead catch handler survived");
        // x, Vec temporary and live binding; live[0] borrows that binding.
        // The dead catch slots are gone and reachable slots remain dense.
        need(run->slots.size() == 3, name + ": non-tail slot map is not dense");
        for (size_t s = 0; s < run->slots.size(); ++s)
          need(slot_uses(*run, s) != 0, name + ": unused remapped slot");
      } else if (name == "both-return-join") {
        const auto *branch = function(module, hir, "branch");
        need(branch != nullptr, name + ": branch");
        // flag plus each arm's Vec temporary and binding; no receiver copy.
        need(branch->slots.size() == 5,
             name + ": dead join owning slots survived");
        for (size_t s = 0; s < branch->slots.size(); ++s)
          need(slot_uses(*branch, s) != 0, name + ": unused remapped slot");
      } else if (name == "zero-locals-dead-tail") {
        const auto *run = function(module, hir, "run");
        need(run != nullptr, name + ": run");
        need(run->slots.empty(), name + ": dead owning slots survived");
        need(run->handlers.empty(), name + ": dead handler survived");
        size_t defaults = 0, real = 0;
        for (const auto &b : run->blocks)
          for (const auto &i : b.instructions) {
            if (uses_slot(i.op))
              ++real;
            else if (i.slot.value == 0)
              ++defaults;
          }
        need(real == 0 && defaults != 0,
             name + ": default slot zero was treated as a reference");
      }

      auto bad = [&](const std::string &mutation, auto edit) {
        auto copy = module;
        need(edit(copy), name + ": mutation " + mutation + " did not apply");
        auto result = ir::verify(copy);
        need(!result.success() && !result.value &&
                 !result.diagnostics.empty() &&
                 result.diagnostics[0].code == "E_IR_INVALID",
             name + ": corrupt slot map accepted: " + mutation);
        ++rejected;
        mutations += name + "/" + mutation + ": " +
                     result.diagnostics[0].message + "\n";
      };
      if (name == "both-return-join") {
        bad("reachable-owning-store-erased", [](auto &m) {
          for (auto &f : m.functions) {
            if (m.hir.module().resolved->symbols.at(f.symbol.value).name != "branch")
              continue;
            for (auto &b : f.blocks)
              for (auto it = b.instructions.begin(); it != b.instructions.end();
                   ++it)
                if (it->op == ir::Op::store &&
                    f.slots[it->slot.value].owner &&
                    f.slots[it->slot.value].owner->kind ==
                        hir::OwnerKind::binding) {
                  b.instructions.erase(it);
                  return true;
                }
          }
          return false;
        });
      }
      if (name == "catch-throws-tail") {
        bad("reachable-owning-cleanup-erased", [](auto &m) {
          for (auto &f : m.functions) {
            if (m.hir.module().resolved->symbols.at(f.symbol.value).name != "take")
              continue;
            for (auto &b : f.blocks)
              for (auto it = b.instructions.begin(); it != b.instructions.end();
                   ++it)
                if (it->op == ir::Op::destroy) {
                  b.instructions.erase(it);
                  return true;
                }
          }
          return false;
        });
        bad("wrong-slot-identity", [](auto &m) {
          for (auto &f : m.functions) {
            if (m.hir.module().resolved->symbols.at(f.symbol.value).name != "take")
              continue;
            for (auto &b : f.blocks)
              for (auto &i : b.instructions)
                if (i.op == ir::Op::destroy) {
                  for (size_t s = 0; s < f.slots.size(); ++s)
                    if (f.slots[s].owner && s != i.slot.value) {
                      i.slot = LocalSlotId{s};
                      return true;
                    }
                }
          }
          return false;
        });
      }
      ++checked;
    }
    need(checked == 5, "expected five unreachable-try fixtures");
    // Emit the actual LLVM for both optimizations. The Python runner links the
    // pre-optimization text with vec_observer.cpp, so allocations are observed
    // by runtime object identity, not by guessing a generated C++ ABI.
    for (const auto &entry : std::filesystem::directory_iterator(fixtures)) {
      if (!entry.is_directory() ||
          !std::filesystem::exists(entry.path() / "lang.toml"))
        continue;
      const std::string name = entry.path().filename().string();
      SourceManager sources;
      auto project =
          modules::load_project({(entry.path() / "lang.toml").string(), {}, {}},
                                sources);
      need(project.success(), name + ": reload");
      auto resolved = modules::resolve_project(
          std::make_shared<const modules::Project>(std::move(*project.value)));
      auto checked_hir = sema::check_project(resolved);
      need(checked_hir.success(), name + ": recheck");
      auto lowered = ir::lower(*checked_hir.value);
      need(lowered.success(), name + ": relower");
      SymbolId main_id;
      for (const auto &f : checked_hir.value->module().functions)
        if (checked_hir.value->module().resolved->symbols.at(f.symbol.value)
                .name == "main")
          main_id = f.symbol;
      need(main_id.value != 0, name + ": main symbol");
      for (unsigned optimization : {0u, 2u}) {
        backend::native::Options options;
        options.optimization = optimization;
        auto generated =
            backend::native::emit(*lowered.value, main_id, options);
        if (!generated.success())
          for (const auto &d : generated.diagnostics)
            std::cerr << name << ": " << d.message << '\n';
        need(generated.success(), name + ": LLVM emit");
        write(out / name / ("program-O" + std::to_string(optimization) + ".ll"),
              generated.value->llvm_before_optimization);
      }
    }
    write(out / "mutations.txt", mutations);
    std::cout << "{\"fixtures\":" << checked << ",\"rejected\":" << rejected
              << "}\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
