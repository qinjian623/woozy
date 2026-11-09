#include "lang/backend/native.hpp"
#include "lang/sema/check.hpp"
#include <iostream>
#include <map>
#include <stdexcept>
using namespace lang;
void require(bool b, const std::string &s) {
  if (!b)
    throw std::runtime_error(s);
}
int main(int argc, char **argv) {
  try {
    require(argc == 2 || argc == 3, "manifest argument");
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
    size_t literal_rejected = 0;
    if (argc == 3) {
      SourceManager literal_sources;
      auto literal_project =
          modules::load_project({argv[2], {}, {}}, literal_sources);
      require(literal_project.success(), "literal load failed");
      auto literal_resolved = modules::resolve_project(
          std::make_shared<const modules::Project>(
              std::move(*literal_project.value)));
      auto literal_checked = sema::check_project(literal_resolved);
      require(literal_checked.success(), "literal check failed");
      auto lowered_literal = ir::lower(*literal_checked.value);
      require(lowered_literal.success(), "literal NativeIR verification failed");
      auto literal_module = lowered_literal.value->module();
      auto bad_literal = [&](const std::string &name, auto mutate) {
        auto m = literal_module;
        mutate(m);
        auto r = ir::verify(m);
        require(!r.success() && !r.value && !r.diagnostics.empty() &&
                    r.diagnostics[0].code == "E_IR_INVALID",
                "corrupt literal NativeIR accepted: " + name);
        ++literal_rejected;
      };
      bad_literal("string equality as addition", [](auto &m) {
        std::map<ValueId, TypeId> definitions;
        for (const auto &f : m.functions)
          for (const auto &b : f.blocks)
            for (const auto &i : b.instructions)
              if (i.result)
                definitions.emplace(*i.result, i.type);
        for (auto &f : m.functions)
          for (auto &b : f.blocks)
            for (auto &i : b.instructions)
              if (i.op == ir::Op::binary && i.binary == hir::BinaryOp::equal &&
                  i.inputs.size() == 2 && definitions.contains(i.inputs[0]) &&
                  m.hir.module().types.find(definitions.at(i.inputs[0])) &&
                  m.hir.module().types.find(definitions.at(i.inputs[0]))->kind ==
                      hir::TypeKind::String) {
                i.binary = hir::BinaryOp::add;
                return;
              }
        throw std::runtime_error("string equality instruction missing");
      });
      bad_literal("string equality mixed operand", [](auto &m) {
        std::map<ValueId, TypeId> definitions;
        uint64_t next_value = 0;
        for (const auto &f : m.functions)
          for (const auto &b : f.blocks) {
            for (const auto &p : b.parameters)
              next_value = std::max(next_value, p.value.value + 1);
            for (const auto &i : b.instructions) {
              if (i.result) {
                definitions.emplace(*i.result, i.type);
                next_value = std::max(next_value, i.result->value + 1);
              }
              for (auto input : i.inputs)
                next_value = std::max(next_value, input.value + 1);
            }
            for (auto input : b.terminator.inputs)
              next_value = std::max(next_value, input.value + 1);
          }
        std::optional<TypeId> integer;
        for (uint64_t id = 1; id <= m.hir.module().types.size(); ++id) {
          const auto *t = m.hir.module().types.find(TypeId{id});
          if (t && (t->kind == hir::TypeKind::SignedInt ||
                    t->kind == hir::TypeKind::UnsignedInt ||
                    t->kind == hir::TypeKind::Isize ||
                    t->kind == hir::TypeKind::Usize)) {
            integer = TypeId{id};
            break;
          }
        }
        if (!integer)
          throw std::runtime_error("integer type missing");
        for (auto &f : m.functions)
          for (auto &b : f.blocks)
            for (size_t p = 0; p < b.instructions.size(); ++p) {
              const auto &i = b.instructions[p];
              if (i.op != ir::Op::binary || i.binary != hir::BinaryOp::equal ||
                  i.inputs.size() != 2 ||
                  !definitions.contains(i.inputs[0]))
                continue;
              const auto *t =
                  m.hir.module().types.find(definitions.at(i.inputs[0]));
              if (!t || t->kind != hir::TypeKind::String)
                continue;
              ir::Instruction constant;
              constant.op = ir::Op::constant;
              constant.result = ValueId{next_value++};
              constant.type = *integer;
              constant.bits = 0;
              constant.source = i.source;
              b.instructions.insert(b.instructions.begin() + p, constant);
              b.instructions[p + 1].inputs[1] = *constant.result;
              return;
            }
        throw std::runtime_error("string equality with integer operand missing");
      });
      bad_literal("enum payload without matching tag variant", [](auto &m) {
        for (auto &f : m.functions)
          for (auto &b : f.blocks)
            for (auto &i : b.instructions)
              if (i.op == ir::Op::enum_payload) {
                for (const auto &n : m.hir.module().nominals)
                  for (const auto &v : n.variants)
                    if (v.symbol != i.variant) {
                      i.variant = v.symbol;
                      return;
                    }
                throw std::runtime_error("alternate enum variant missing");
              }
        throw std::runtime_error("enum payload instruction missing");
      });
    }
    std::cout << "passed: " << rejected
              << " corrupt CFGs rejected; immutable publication; O0/O2 LLVM "
                 "emit; explicit targets rejected";
    if (argc == 3)
      std::cout << "; " << literal_rejected
                << " corrupt literal NativeIR rejected";
    std::cout << '\n';
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
