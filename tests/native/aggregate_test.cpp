#include "lang/backend/native.hpp"
#include "lang/sema/check.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace lang;
void need(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}
ir::Module load(const std::filesystem::path &path) {
  SourceManager sources;
  auto project = modules::load_project({path, {}, {}}, sources);
  need(project.success(), "load aggregate source");
  auto resolved = modules::resolve_project(
      std::make_shared<const modules::Project>(std::move(*project.value)));
  auto checked = sema::check_project(resolved);
  need(checked.success(), "check aggregate source");
  auto count = checked.value->module().types.size();
  auto lowered = ir::lower(*checked.value);
  need(lowered.success(), "lower aggregate source");
  need(checked.value->module().types.size() == count, "lower mutated TypeContext");
  return lowered.value->module();
}
int main(int argc, char **argv) {
  try {
    need(argc == 2, "repository root required");
    std::filesystem::path root = argv[1];
    size_t rejected = 0;
    auto corrupt = [&](const ir::Module &original, ir::Op op, auto mutate) {
      auto m = original;
      bool found = false;
      for (auto &f : m.functions)
        for (auto &b : f.blocks)
          for (auto &i : b.instructions)
            if (!found && i.op == op) {
              mutate(m, f, b, i);
              found = true;
            }
      need(found, "missing corruption target");
      auto result = ir::verify(m);
      need(!result.success() && !result.value &&
               result.failure == FailureKind::internal &&
               result.diagnostics.at(0).code == "E_IR_INVALID",
           "corrupt aggregate CFG accepted");
      ++rejected;
    };
    auto structure = load(root / "tests/native/pod/copy-return/lang.toml");
    auto enumeration = load(root / "tests/native/pod/enum-copy/lang.toml");
    auto control = load(root / "tests/native/pod/enum-control/lang.toml");
    for (auto op : {ir::Op::field_read, ir::Op::field_address}) {
      corrupt(structure, op, [](auto &, auto &, auto &, auto &i) {
        i.field = SymbolId{999999};
      });
      corrupt(structure, op, [](auto &, auto &, auto &, auto &i) {
        i.type = TypeId{1}; // Unit cannot replace this i32/struct projection.
      });
      corrupt(structure, op, [](auto &, auto &, auto &, auto &i) {
        i.inputs.clear();
      });
    }
    corrupt(structure, ir::Op::struct_init, [](auto &, auto &, auto &, auto &i) {
      i.inputs.pop_back();
    });
    corrupt(structure, ir::Op::struct_init, [](auto &, auto &, auto &, auto &i) {
      std::swap(i.inputs[0], i.inputs[1]);
    });
    corrupt(structure, ir::Op::field_address, [](auto &, auto &, auto &, auto &i) {
      i.op = ir::Op::field_read; // address/value distinction
    });
    corrupt(structure, ir::Op::load_indirect, [](auto &, auto &, auto &, auto &i) {
      i.inputs = {*i.result}; // use before definition
    });
    corrupt(structure, ir::Op::store_indirect, [](auto &, auto &, auto &, auto &i) {
      std::swap(i.inputs[0], i.inputs[1]);
    });
    for (auto op : {ir::Op::enum_init, ir::Op::enum_test, ir::Op::enum_payload})
      corrupt(enumeration, op, [](auto &, auto &, auto &, auto &i) {
        i.variant = SymbolId{999999};
      });
    corrupt(enumeration, ir::Op::enum_payload, [](auto &, auto &, auto &, auto &i) {
      i.field = SymbolId{999999};
    });
    corrupt(enumeration, ir::Op::enum_payload, [](auto &, auto &, auto &, auto &i) {
      i.type = TypeId{1};
    });
    corrupt(enumeration, ir::Op::enum_test, [](auto &, auto &, auto &b, auto &) {
      std::swap(b.terminator.targets[0], b.terminator.targets[1]);
    });
    corrupt(enumeration, ir::Op::enum_test, [](auto &, auto &, auto &b, auto &) {
      b.terminator.targets[1] = b.terminator.targets[0];
    });
    corrupt(control, ir::Op::enum_test, [](auto &, auto &, auto &b, auto &) {
      b.terminator.targets[0] = BlockId{999999};
    });
    // Initializing a field does not initialize an otherwise uninitialized root.
    corrupt(structure, ir::Op::address, [](auto &, auto &f, auto &, auto &i) {
      f.slots.push_back({i.type, {}, false});
      i.slot = LocalSlotId{f.slots.size() - 1};
    });
    corrupt(enumeration, ir::Op::enum_payload, [](auto &, auto &f, auto &, auto &i) {
      f.blocks[0].terminator.inputs = {*i.result};
    });
    // Every public ID remains in the shared context; storage-only substitution
    // is private even for expanding function results.
    auto finite = load(root / "tests/native/pod/finite-function-field/lang.toml");
    auto count = finite.hir.module().types.size();
    need(count < 100, "finite function field expanded shared types");
    auto verified = ir::verify(finite);
    need(verified.success(), "finite CFG verify");
    auto entry = finite.functions.back().symbol;
    for (const auto &s : finite.hir.module().resolved->symbols)
      if (s.name == "main" && s.kind == modules::SymbolKind::function)
        entry = s.id;
    for (unsigned opt : {0u, 2u}) {
      backend::native::Options options;
      options.optimization = opt;
      auto output = backend::native::emit(*verified.value, entry, options);
      need(output.success() && !output.value->object.empty(), "finite LLVM emit");
      need(finite.hir.module().types.size() == count, "emit mutated shared types");
    }
    std::cout << "passed: " << rejected << " corrupt aggregate CFGs rejected; "
              << "finite function field shared types=" << count << '\n';
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
