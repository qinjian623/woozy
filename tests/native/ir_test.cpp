#include "lang/backend/native.hpp"
#include "lang/sema/check.hpp"
#include <algorithm>
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
    // A binding freezes its own owning local even when the initializer is an
    // inert noncapturing function value. NativeIR must name the binding for
    // that slot and must not invent a temporary owner for the initializer.
    const std::string binding_body =
        "func plain_i32() i32 {return 0;} "
        "func alternate(flag bool) bool {return flag;} "
        "func store_fn() i32 {let value func() i32=plain_i32;return 0;} "
        "func store_lambda() i32 {let value func() i32=func() i32 {return "
        "7;};return 0;} "
        "func scalar_only() i32 {let n i32=3;return n-3;} "
        "func main() i32 {return store_fn()+store_lambda()+scalar_only();}";
    SourceManager binding_sources;
    auto binding_source = binding_sources.update(
        binding_sources.add_file("native-fn-binding.wz"),
        "package app; " + binding_body);
    auto binding_tokens = lex(binding_source);
    require(binding_tokens.success(), "function-binding lex failed");
    auto binding_ast = parse(*binding_tokens.value);
    require(binding_ast.success(), "function-binding parse failed");
    auto binding_project = std::make_shared<modules::Project>();
    binding_project->backend = "native";
    binding_project->word_bits = 64;
    modules::Module binding_decl;
    binding_decl.id = ModuleId{0};
    binding_decl.logical_path = "app";
    binding_decl.files.push_back(
        std::make_shared<const AstFile>(std::move(*binding_ast.value)));
    binding_project->modules.push_back(std::move(binding_decl));
    auto binding_names = modules::resolve_project(binding_project);
    require(binding_names.success(), "function-binding resolve failed");
    auto binding_resolved = std::make_shared<const modules::ResolvedProject>(
        std::move(*binding_names.value));
    auto binding_checked = sema::check_project(binding_resolved);
    require(binding_checked.success(), "function-binding check failed");
    auto binding_lowered = ir::lower(*binding_checked.value);
    require(binding_lowered.success(),
            "function-binding NativeIR lower failed");
    auto binding_ir = binding_lowered.value->module();
    require(ir::verify(binding_ir).success(),
            "function-binding NativeIR verify failed");
    const auto &binding_hir = binding_ir.hir.module();
    for (auto name : {"store_fn", "store_lambda"}) {
      std::optional<SymbolId> target;
      for (const auto &s : binding_resolved->symbols)
        if (s.name == name && s.kind == modules::SymbolKind::function)
          target = s.id;
      require(target.has_value(), std::string(name) + " symbol missing");
      size_t function_index = binding_ir.functions.size();
      for (size_t i = 0; i < binding_ir.functions.size(); ++i)
        if (binding_ir.functions[i].symbol == *target)
          function_index = i;
      require(function_index < binding_ir.functions.size(),
              std::string(name) + " native function missing");
      const auto &native_function = binding_ir.functions[function_index];
      std::optional<HirNodeId> binding_node;
      for (size_t i = 0; i < binding_hir.nodes.size(); ++i) {
        const auto &n = binding_hir.nodes[i];
        if (n.kind() == hir::NodeKind::binding &&
            binding_hir.scopes.at(n.scope.value).function == *target) {
          require(!binding_node, std::string(name) + " has two bindings");
          binding_node = HirNodeId{i};
        }
      }
      require(binding_node.has_value(), std::string(name) + " binding missing");
      auto local_symbol =
          std::get<hir::Binding>(binding_hir.nodes[binding_node->value].data)
              .symbol;
      auto initializer =
          binding_hir.nodes[binding_node->value].children.at(0);
      auto init_type = binding_hir.nodes[initializer.value].type;
      size_t slot_index = native_function.slots.size();
      for (size_t i = 0; i < native_function.slots.size(); ++i) {
        const auto &s = native_function.slots[i];
        if (s.binding && *s.binding == local_symbol) {
          require(slot_index == native_function.slots.size(),
                  std::string(name) + " has two binding slots");
          slot_index = i;
        }
      }
      require(slot_index < native_function.slots.size(),
              std::string(name) + " binding slot missing");
      const auto &slot = native_function.slots[slot_index];
      require(slot.type == init_type,
              std::string(name) + " slot type differs from HIR");
      require(slot.owner.has_value() &&
                  slot.owner->kind == hir::OwnerKind::binding &&
                  slot.owner->identity == local_symbol.value,
              std::string(name) + " slot owner is not the binding");
      require(hir::classify_value(binding_hir, slot.type).needs_drop,
              std::string(name) + " slot is no longer owning");
      require(std::none_of(
                  native_function.slots.begin(), native_function.slots.end(),
                  [&](const auto &s) {
                    return s.owner &&
                           s.owner->kind == hir::OwnerKind::temporary &&
                           s.owner->identity == initializer.value;
                  }),
              std::string(name) +
                  " inert initializer gained a temporary owner slot");
      require(std::none_of(binding_hir.ownership.temporaries.begin(),
                           binding_hir.ownership.temporaries.end(),
                           [&](const auto &t) {
                             return t.producer == initializer;
                           }),
              std::string(name) +
                  " inert initializer gained a frozen temporary");
      size_t frozen = 0;
      for (const auto &l : binding_hir.ownership.locals)
        if (l.symbol == local_symbol) {
          ++frozen;
          require(l.type == init_type &&
                      l.scope == binding_hir.nodes[binding_node->value].scope &&
                      binding_hir.scopes.at(l.scope.value).function == *target,
                  std::string(name) + " frozen local identity changed");
        }
      require(frozen == 1,
              std::string(name) + " must freeze exactly one local");
      auto bad_binding = [&](const std::string &label, auto mutate) {
        auto m = binding_ir;
        mutate(m);
        auto r = ir::verify(m);
        require(!r.success() && !r.value && !r.diagnostics.empty() &&
                    r.diagnostics[0].code == "E_IR_INVALID",
                "corrupt function-binding NativeIR accepted: " + label);
        ++rejected;
      };
      bad_binding(std::string("wrong slot owner ") + name, [&](auto &m) {
        m.functions[function_index].slots[slot_index].owner =
            hir::OwnerRef{hir::OwnerKind::binding, local_symbol.value + 991};
      });
      std::optional<TypeId> other_owned;
      for (uint64_t id = 1; id <= binding_hir.types.size(); ++id)
        if (id != init_type.value &&
            hir::classify_value(binding_hir, TypeId{id}).needs_drop) {
          other_owned = TypeId{id};
          break;
        }
      require(other_owned.has_value(), "alternate owning type missing");
      bad_binding(std::string("wrong slot type ") + name, [&](auto &m) {
        m.functions[function_index].slots[slot_index].type = *other_owned;
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
