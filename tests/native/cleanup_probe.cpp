#include "lang/backend/native.hpp"
#include "lang/sema/check.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/SourceMgr.h>
using namespace lang;
void need(bool ok, const std::string &s) {
  if (!ok)
    throw std::runtime_error(s);
}
void write_file(const std::filesystem::path &p, const std::string &s) {
  std::ofstream out(p, std::ios::binary);
  out.write(s.data(), s.size());
  need(bool(out), "write artifact");
}
int main(int argc, char **argv) {
  try {
    need(argc == 4, "manifest, output directory, optimization required");
    std::filesystem::path out = argv[2];
    std::filesystem::create_directories(out);
    SourceManager sources;
    auto project = modules::load_project({argv[1], {}, {}}, sources);
    need(project.success(), "load cleanup source");
    auto resolved = modules::resolve_project(
        std::make_shared<const modules::Project>(std::move(*project.value)));
    auto checked = sema::check_project(resolved);
    if (!checked.success())
      for (auto &d : checked.diagnostics)
        std::cerr << d.code << ": " << d.message << '\n';
    need(checked.success(), "publish real VerifiedHIR");
    auto cfg = ir::lower(*checked.value);
    if (!cfg.success())
      for (auto &d : cfg.diagnostics)
        std::cerr << d.message << '\n';
    need(cfg.success(), "lower real cleanup HIR");
    const auto original = cfg.value->module();
    size_t rejected = 0;
    std::string mutations;
    auto bad = [&](const std::string &name, auto edit) {
      auto m = original;
      if (!edit(m))
        return;
      auto r = ir::verify(m);
      need(!r.success() && !r.value && r.failure == FailureKind::internal &&
               !r.diagnostics.empty() &&
               r.diagnostics[0].code == "E_IR_INVALID",
           "corrupt native IR accepted: " + name);
      ++rejected;
      mutations += name + ": " + r.diagnostics[0].message + "\n";
    };
    auto term = [&](ir::Exit kind, auto edit) {
      for (auto &f : original.functions)
        for (auto &b : f.blocks)
          if (b.terminator.kind == kind) {
            bad("terminator-" + std::to_string(int(kind)), [&](auto &m) {
              for (auto &mf : m.functions)
                for (auto &mb : mf.blocks)
                  if (mb.terminator.kind == kind) {
                    edit(mf, mb.terminator);
                    return true;
                  }
              return false;
            });
            return;
          }
    };
    term(ir::Exit::try_call,
         [](auto &, auto &t) { t.targets[1] = t.targets[0]; });
    term(ir::Exit::try_call,
         [](auto &, auto &t) { t.targets[0] = BlockId{999999}; });
    term(ir::Exit::try_call, [](auto &, auto &t) { t.signature = TypeId{1}; });
    term(ir::Exit::try_call, [](auto &, auto &t) { t.inputs.pop_back(); });
    term(ir::Exit::try_call,
         [](auto &, auto &t) { std::swap(t.targets[0], t.targets[1]); });
    term(ir::Exit::try_call, [](auto &f, auto &t) {
      // A failure block must never read the success parameter, even if its type
      // matches.
      auto &fail = f.blocks[t.targets[1].value];
      auto good = f.blocks[t.targets[0].value].parameters[0].value;
      if (!fail.instructions.empty())
        fail.instructions[0].inputs = {good};
    });
    term(ir::Exit::try_call, [](auto &f, auto &t) {
      f.blocks[t.targets[1].value].parameters[0].type = TypeId{1};
    });
    term(ir::Exit::try_call, [](auto &, auto &t) { t.origin.reset(); });
    term(ir::Exit::error_return,
         [](auto &, auto &t) { t.kind = ir::Exit::return_; });
    term(ir::Exit::error_return,
         [](auto &, auto &t) { t.inputs = {ValueId{999999}}; });
    auto instruction = [&](const std::string &name, ir::Op op, auto edit) {
      bad(name, [&](auto &m) {
        for (auto &f : m.functions)
          for (auto &b : f.blocks)
            for (size_t j = 0; j < b.instructions.size(); ++j)
              if (b.instructions[j].op == op) {
                edit(f, b, j);
                return true;
              }
        return false;
      });
    };
    instruction("missing-cleanup-end", ir::Op::cleanup_end,
                [](auto &, auto &b, size_t j) {
                  b.instructions.erase(b.instructions.begin() + j);
                });
    instruction("invalid-cleanup-registration", ir::Op::cleanup_begin,
                [](auto &, auto &b, size_t j) {
                  b.instructions[j].cleanup = HirNodeId{999999};
                });
    instruction("duplicate-cleanup", ir::Op::cleanup_begin,
                [](auto &, auto &b, size_t j) {
                  b.instructions.insert(b.instructions.begin() + j,
                                        b.instructions[j]);
                });
    instruction("missing-exit-begin", ir::Op::exit_begin,
                [](auto &, auto &b, size_t j) {
                  b.instructions.erase(b.instructions.begin() + j);
                });
    bad("saved-value-missing", [&](auto &m) {
      for (auto &f : m.functions)
        for (auto &b : f.blocks)
          for (auto &i : b.instructions)
            if (i.op == ir::Op::exit_begin && !i.inputs.empty()) {
              i.inputs.clear();
              return true;
            }
      return false;
    });
    bad("cleanup-order", [&](auto &m) {
      for (auto &f : m.functions)
        for (auto &b : f.blocks) {
          std::vector<size_t> markers;
          for (size_t j = 0; j < b.instructions.size(); ++j)
            if (b.instructions[j].op == ir::Op::cleanup_begin)
              markers.push_back(j);
          if (markers.size() >= 2) {
            std::swap(b.instructions[markers[0]].cleanup,
                      b.instructions[markers[1]].cleanup);
            return true;
          }
        }
      return false;
    });
    bad("omitted-exit-plan", [&](auto &m) {
      for (auto &f : m.functions)
        for (auto &b : f.blocks)
          if (b.terminator.exit) {
            b.terminator.exit.reset();
            return true;
          }
      return false;
    });
    bad("catch-uninitialized", [&](auto &m) {
      for (auto &f : m.functions)
        if (!f.handlers.empty()) {
          auto slot = f.handlers.front().slot;
          for (auto &b : f.blocks)
            for (auto it = b.instructions.begin(); it != b.instructions.end();
                 ++it)
              if (it->op == ir::Op::store && it->slot == slot) {
                b.instructions.erase(it);
                return true;
              }
        }
      return false;
    });
    bad("invalid-handler", [&](auto &m) {
      for (auto &f : m.functions)
        if (!f.handlers.empty()) {
          f.handlers[0].target.index = 999999;
          return true;
        }
      return false;
    });
    bad("erased-cleanup-obligation-and-origins", [&](auto &m) {
      for (auto &f : m.functions) {
        if (m.hir.module().resolved->symbols.at(f.symbol.value).name != "probe")
          continue;
        bool found = false;
        unsigned depth = 0;
        for (auto &block : f.blocks) {
          std::vector<ir::Instruction> kept;
          for (const auto &i : block.instructions) {
            if (i.op == ir::Op::cleanup_begin) {
              ++depth;
              found = true;
              continue;
            }
            if (i.op == ir::Op::cleanup_end) {
              --depth;
              continue;
            }
            if (i.op == ir::Op::exit_begin || depth)
              continue;
            kept.push_back(i);
          }
          block.instructions = std::move(kept);
          block.terminator.exit.reset();
          block.terminator.origin.reset();
        }
        if (found)
          return true;
      }
      return false;
    });
    bad("cleanup-call-erased-with-markers-retained", [&](auto &m) {
      unsigned depth = 0;
      for (auto &f : m.functions)
        for (auto &b : f.blocks)
          for (auto it = b.instructions.begin(); it != b.instructions.end();
               ++it) {
            if (it->op == ir::Op::cleanup_begin)
              ++depth;
            if (it->op == ir::Op::cleanup_end)
              --depth;
            if (depth && it->op == ir::Op::call &&
                m.hir.module().types.find(it->type)->kind ==
                    hir::TypeKind::Unit) {
              b.instructions.erase(it);
              return true;
            }
          }
      return false;
    });
    bad("cleanup-call-moved-to-other-inline-copy", [&](auto &m) {
      for (auto &f : m.functions)
        for (size_t a = 0; a < f.blocks.size(); ++a)
          for (size_t b = a + 1; b < f.blocks.size(); ++b)
            for (size_t x = 0; x < f.blocks[a].instructions.size(); ++x)
              for (size_t y = 0; y < f.blocks[b].instructions.size(); ++y) {
                auto i = f.blocks[a].instructions[x];
                const auto &j = f.blocks[b].instructions[y];
                if (i.op == ir::Op::call && j.op == ir::Op::call &&
                    i.origin == j.origin &&
                    m.hir.module().types.find(i.type)->kind ==
                        hir::TypeKind::Unit) {
                  // Keep the total number of calls, use the destination's
                  // dominating arguments, but execute twice in the wrong copy.
                  i.inputs = j.inputs;
                  f.blocks[a].instructions.erase(
                      f.blocks[a].instructions.begin() + x);
                  f.blocks[b].instructions.insert(
                      f.blocks[b].instructions.begin() + y, i);
                  return true;
                }
              }
      return false;
    });
    bad("live-unreachable-with-unfinished-cleanup", [&](auto &m) {
      for (auto &f : m.functions)
        for (auto &b : f.blocks)
          if (b.terminator.kind == ir::Exit::return_)
            for (size_t j = b.instructions.size(); j > 0; --j)
              if (b.instructions[j - 1].op == ir::Op::cleanup_end) {
                b.instructions.erase(b.instructions.begin() + j - 1);
                b.terminator.kind = ir::Exit::unreachable;
                b.terminator.inputs.clear();
                b.terminator.exit.reset();
                b.terminator.origin.reset();
                return true;
              }
      return false;
    });
    bad("enum-default-not-exhaustively-excluded", [&](auto &m) {
      for (auto &f : m.functions) {
        std::vector<ir::Instruction *> tests;
        for (auto &b : f.blocks)
          for (auto &i : b.instructions)
            if (i.op == ir::Op::enum_test)
              for (const auto &n : m.hir.module().nodes)
                if (n.kind() == hir::NodeKind::defer_) {
                  const auto &range = m.hir.module().nodes[n.children[0].value].source.span;
                  if (i.source.snapshot == range.snapshot &&
                      i.source.begin_byte >= range.begin_byte &&
                      i.source.end_byte <= range.end_byte)
                    tests.push_back(&i);
                }
        for (auto *a : tests)
          for (auto *b : tests)
            if (a->inputs == b->inputs && a->variant != b->variant) {
              a->variant = b->variant;
              return true;
            }
      }
      return false;
    });
    bad("cleanup-call-moved-to-impossible-enum-default", [&](auto &m) {
      for (auto &f : m.functions)
        for (auto &destination : f.blocks)
          if (destination.terminator.kind == ir::Exit::unreachable &&
              destination.instructions.empty())
            for (auto &source : f.blocks) {
              bool call = false, simple = true;
              for (const auto &i : source.instructions) {
                call |= i.op == ir::Op::call;
                simple &= i.op == ir::Op::function || i.op == ir::Op::address ||
                          i.op == ir::Op::constant || i.op == ir::Op::call;
              }
              if (call && simple) {
                destination.instructions = std::move(source.instructions);
                source.instructions.clear();
                return true;
              }
            }
      return false;
    });
    bad("handler-wrong-concrete-entry", [&](auto &m) {
      for (auto &f : m.functions)
        if (!f.handlers.empty()) {
          f.handlers[0].protected_entry = BlockId{999999};
          return true;
        }
      return false;
    });
    bad("handler-not-a-Try-entry", [&](auto &m) {
      for (auto &f : m.functions)
        if (!f.handlers.empty()) {
          f.handlers[0].protected_entry = BlockId{0};
          return true;
        }
      return false;
    });
    bad("handler-cross-inline-instance", [&](auto &m) {
      for (auto &f : m.functions)
        for (size_t a = 0; a < f.handlers.size(); ++a)
          for (size_t b = a + 1; b < f.handlers.size(); ++b) {
            auto &x = f.handlers[a], &y = f.handlers[b];
            if (x.target == y.target &&
                x.protected_entry != y.protected_entry) {
              std::swap(x.protected_entry, y.protected_entry);
              return true;
            }
          }
      return false;
    });
    SymbolId entry;
    for (auto &s : resolved.value->symbols)
      if (s.name == "main" && s.kind == modules::SymbolKind::function)
        entry = s.id;
    backend::native::Options options;
    options.optimization = unsigned(std::stoul(argv[3]));
    auto emitted = backend::native::emit(*cfg.value, entry, options);
    if (!emitted.success())
      for (auto &d : emitted.diagnostics)
        std::cerr << d.message << '\n';
    need(emitted.success(), "emit cleanup");
    write_file(out / "generated.o", std::string(emitted.value->object.begin(),
                                                emitted.value->object.end()));
    write_file(out / "before.ll", emitted.value->llvm_before_optimization);
    write_file(out / "after.ll", emitted.value->llvm_ir);
    write_file(out / "cleanup.nir", emitted.value->native_ir);
    write_file(out / "mutations.txt", mutations);
    llvm::LLVMContext context;
    llvm::SMDiagnostic error;
    auto m = llvm::parseAssemblyString(emitted.value->llvm_before_optimization,
                                       error, context);
    need(m && !llvm::verifyModule(*m), "parse actual generated LLVM");
    // Instrument the generated functions, never a C++ reimplementation or
    // rewritten source. Unbuffered write survives the runtime's fatal _Exit.
    auto i64 = llvm::Type::getInt64Ty(context);
    auto writer = m->getOrInsertFunction(
        "write",
        llvm::FunctionType::get(i64,
                                {llvm::Type::getInt32Ty(context),
                                 llvm::PointerType::getUnqual(context), i64},
                                false));
    auto record = [&](llvm::IRBuilder<> &b, uint64_t tag, llvm::Value *a,
                      llvm::Value *v) {
      auto layout = llvm::ArrayType::get(i64, 3);
      auto storage = b.CreateAlloca(layout);
      auto put = [&](unsigned n, llvm::Value *x) {
        b.CreateStore(b.CreateIntCast(x, i64, !x->getType()->isIntegerTy(1)),
                      b.CreateInBoundsGEP(layout, storage,
                                          {b.getInt32(0), b.getInt32(n)}));
      };
      put(0, b.getInt64(tag));
      put(1, a);
      put(2, v);
      b.CreateCall(writer, {b.getInt32(1), storage, b.getInt64(24)});
    };
    size_t observed = 0;
    uint64_t entry_error_type_id = 0;
    for (const auto &f : checked.value->module().functions) {
      const auto &sym =
          checked.value->module().resolved->symbols.at(f.symbol.value);
      auto fn = m->getFunction("lang.fn." + std::to_string(f.symbol.value));
      const auto &sig = *checked.value->module().types.find(f.signature);
      if (sym.name == "main" && sig.throws_type)
        entry_error_type_id = sig.throws_type->value;
      const auto &rt = *checked.value->module().types.find(sig.result);
      std::vector<llvm::ReturnInst *> returns;
      for (auto &b : *fn)
        if (auto r = llvm::dyn_cast<llvm::ReturnInst>(b.getTerminator()))
          returns.push_back(r);
      for (auto r : returns) {
        llvm::IRBuilder<> b(r);
        if (sym.name == "mark") {
          need((fn->arg_size() == 1 || fn->arg_size() == 2) &&
                   sig.parameters[0].mode == hir::ParamMode::inout,
               "mark observer type");
          auto log_type = llvm::IntegerType::get(
              context, hir::bit_width(*checked.value->module().types.find(
                                          sig.parameters[0].type),
                                      checked.value->config()));
          record(b, 1,
                 fn->arg_size() == 2 ? static_cast<llvm::Value *>(fn->getArg(1))
                                     : b.getInt32(1),
                 b.CreateLoad(log_type, fn->getArg(0)));
          ++observed;
        } else if (sym.name == "probe" && !sig.throws_type &&
                   sig.parameters.size() == 1 &&
                   sig.parameters[0].mode == hir::ParamMode::inout) {
          record(b, 2,
                 rt.kind == hir::TypeKind::Unit ? b.getInt64(0)
                                                : r->getReturnValue(),
                 b.CreateLoad(
                     llvm::IntegerType::get(
                         context,
                         hir::bit_width(*checked.value->module().types.find(
                                            sig.parameters[0].type),
                                        checked.value->config())),
                     fn->getArg(0)));
          ++observed;
        } else if (sym.name == "f" && sig.parameters.size() == 2 &&
                   sig.parameters[1].mode == hir::ParamMode::inout) {
          record(b, sig.throws_type ? 8 : 7,
                 rt.kind == hir::TypeKind::Unit && !sig.throws_type
                     ? b.getInt64(0)
                     : r->getReturnValue(),
                 b.CreateLoad(b.getInt32Ty(), fn->getArg(1)));
          ++observed;
        } else if (sym.name == "read_value") {
          record(b, 3, b.CreateLoad(b.getInt32Ty(), fn->getArg(1)),
                 r->getReturnValue());
          ++observed;
          // Only the success return initializes out_value.
          if (auto c = llvm::dyn_cast<llvm::ConstantInt>(r->getReturnValue());
              c && c->isOne())
            record(b, 6, b.CreateLoad(b.getInt32Ty(), fn->getArg(2)),
                   b.getInt64(0));
        } else if (sym.name == "main") {
          record(b, sig.throws_type ? 5 : 4, r->getReturnValue(),
                 b.getInt64(0));
          ++observed;
        }
      }
    }
    need(!llvm::verifyModule(*m), "verify instrumented LLVM");
    std::string observation;
    llvm::raw_string_ostream stream(observation);
    m->print(stream, nullptr);
    write_file(out / "observe.ll", observation);
    std::cout << "{\"status\":\"passed\",\"native_ir_rejected\":" << rejected
              << ",\"entry_error_type_id\":" << entry_error_type_id
              << ",\"observation_sites\":" << observed << "}\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
