#include "lang/backend/native.hpp"
#include "lang/sema/check.hpp"
#include <iostream>
#include <stdexcept>
using namespace lang;
void require(bool b, const std::string &s) {
  if (!b)
    throw std::runtime_error(s);
}
int main(int argc, char **argv) {
  try {
    require(argc == 2, "manifest argument");
    SourceManager sources;
    auto project = modules::load_project({argv[1], {}, {}}, sources);
    require(project.success(), "load failed");
    auto resolved = modules::resolve_project(
        std::make_shared<const modules::Project>(std::move(*project.value)));
    auto checked = sema::check_project(resolved);
    require(checked.success(), "real check failed");
    auto lowered = ir::lower(*checked.value);
    if (!lowered.success())
      for (auto &d : lowered.diagnostics)
        std::cerr << d.message << '\n';
    require(lowered.success(), "CFG verification failed");
    auto original = lowered.value->module();
    size_t rejected = 0;
    auto bad = [&](auto mutate) {
      auto m = original;
      mutate(m);
      auto r = ir::verify(m);
      require(!r.success() && !r.value && r.failure == FailureKind::internal &&
                  !r.diagnostics.empty() &&
                  r.diagnostics[0].code == "E_IR_INVALID",
              "corrupt CFG accepted");
      ++rejected;
    };
    bad([](auto &m) {
      m.functions[0].blocks[0].terminator.kind = ir::Exit::missing;
    });
    bad([](auto &m) {
      m.functions[0].blocks[0].terminator.targets = {BlockId{999999}};
    });
    bad([](auto &m) {
      m.functions[0].blocks[0].terminator.targets[0] = BlockId{0};
    });
    bad([](auto &m) {
      m.functions[0].blocks[0].instructions[1].result =
          m.functions[0].blocks[0].instructions[0].result;
    });
    bad([](auto &m) {
      m.functions[0].blocks[0].terminator.inputs = {ValueId{999999}};
    });
    bad([](auto &m) {
      m.functions[0].blocks[0].instructions[0].slot = LocalSlotId{99999};
    });
    bad([](auto &m) { m.functions[0].slots[0].parameter = 99999; });
    bad([](auto &m) { m.functions[0].slots[0].parameter.reset(); });
    bad([](auto &m) { m.functions[0].slots[0].inout = true; });
    bad([](auto &m) { m.functions.push_back(m.functions[0]); });
    bad([](auto &m) { m.functions.pop_back(); });
    bad([](auto &m) { m.functions[0].signature = TypeId{99999}; });
    bad([](auto &m) {
      m.functions[0].blocks[0].instructions[0].result.reset();
    });
    bad([](auto &m) {
      m.functions[0].blocks[0].instructions[0].inputs = {ValueId{0}};
    });
    bad([](auto &m) {
      m.functions[0].blocks[0].instructions[0].op = ir::Op::call;
    });
    // Load without initialization is independently checked by native IR.
    bad([](auto &m) {
      auto &f = m.functions[0];
      auto st = f.slots[0].type;
      f.slots.push_back({st, {}, false});
      f.blocks[0].instructions[0].slot = LocalSlotId{f.slots.size() - 1};
    });
    // A bool return cannot stand in for i32, even when LLVM could cast it.
    bad([](auto &m) {
      auto &f = m.functions[0];
      f.blocks[0].terminator = {ir::Exit::return_,
                                {*f.blocks[0].instructions.back().result},
                                {},
                                f.source};
    });
    bad([](auto &m) {
      auto &f = m.functions[0];
      f.blocks[3].terminator.inputs = {*f.blocks[1].instructions[0].result};
    });
    // Immutable publication is unaffected by later mutation of builder data.
    auto mutable_module = original;
    auto published = ir::verify(mutable_module);
    require(published.success(), "publish");
    mutable_module.functions.clear();
    require(published.value->module().functions.size() ==
                original.functions.size(),
            "mutable alias");
    SymbolId entry;
    for (auto &s : resolved.value->symbols)
      if (s.name == "main" && s.kind == modules::SymbolKind::function)
        entry = s.id;
    for (unsigned optimization : {0u, 2u}) {
      backend::native::Options opts;
      opts.optimization = optimization;
      auto generated = backend::native::emit(*published.value, entry, opts);
      if (!generated.success())
        for (auto &d : generated.diagnostics)
          std::cerr << d.message << '\n';
      require(generated.success() && !generated.value->object.empty(),
              "real LLVM emit failed");
      require(generated.value->triple == "x86_64-unknown-linux-gnu" &&
                  generated.value->data_layout.find("p270:32:32") !=
                      std::string::npos,
              "target layout missing");
    }
    for (auto triple : {"aarch64-unknown-linux-gnu", "x86_64-pc-windows-msvc",
                        "x86_64-apple-darwin"}) {
      backend::native::Options opts;
      opts.triple = triple;
      auto r = backend::native::emit(*published.value, entry, opts);
      require(!r.success() && r.failure == FailureKind::unsupported &&
                  r.diagnostics[0].code == "E_CAPABILITY_UNAVAILABLE",
              "target accepted");
    }
    auto no_entry = backend::native::emit(*published.value, SymbolId{999999});
    require(!no_entry.success() && no_entry.failure == FailureKind::internal,
            "unknown entry accepted");
    std::cout << "passed: " << rejected
              << " corrupt CFGs rejected; immutable publication; O0/O2 LLVM "
                 "emit; explicit targets rejected\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
