#include "lang/backend/native.hpp"
#include "lang/sema/check.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/SourceMgr.h>
using namespace lang;
static void need(bool v, const std::string &m) { if (!v) throw std::runtime_error(m); }
static void write(const std::filesystem::path &p, const std::string &s) { std::ofstream(p) << s; }
int main(int argc, char **argv) {
  try {
    need(argc == 4, "manifest output optimization");
    std::filesystem::path out = argv[2]; std::filesystem::create_directories(out);
    SourceManager sources;
    auto p = modules::load_project({argv[1], {}, {}}, sources); need(p.success(), "load source");
    auto r = modules::resolve_project(std::make_shared<const modules::Project>(std::move(*p.value)));
    auto h = sema::check_project(r);
    if (!h.success()) for (const auto &d : h.diagnostics) std::cerr << d.message << '\n';
    need(h.success(), "checked and frozen HIR");
    SymbolId main;
    for (const auto &f : h.value->module().functions)
      if (h.value->module().resolved->symbols[f.symbol.value].name == "main") main = f.symbol;
    auto cfg = ir::lower(*h.value);
    if (!cfg.success()) for (const auto &d : cfg.diagnostics) std::cerr << d.message << '\n';
    need(cfg.success(), "verified NativeIR");
    size_t rejected = 0; std::string mutations;
    auto bad = [&](const std::string &name, auto edit) {
      auto m = cfg.value->module(); if (!edit(m)) return;
      auto result = ir::verify(m);
      need(!result.success() && !result.value && !result.diagnostics.empty() &&
           result.diagnostics[0].code == "E_IR_INVALID", "accepted corrupt IR: " + name);
      ++rejected; mutations += name + ": " + result.diagnostics[0].message + "\n";
    };
    auto erase = [&](ir::Op op, bool rewire) {
      bad("erase-actual-op-" + std::to_string(int(op)) + (rewire ? "-rewire" : "-metadata-retained"), [&](auto &m) {
        for (auto &f : m.functions) for (auto &b : f.blocks)
          for (size_t n = 0; n < b.instructions.size(); ++n) if (b.instructions[n].op == op) {
            auto i = b.instructions[n]; b.instructions.erase(b.instructions.begin() + n);
            if (rewire && i.result && !i.inputs.empty()) {
              for (auto &other : f.blocks) {
                for (auto &j : other.instructions) for (auto &v : j.inputs) if (v == *i.result) v = i.inputs[0];
                for (auto &v : other.terminator.inputs) if (v == *i.result) v = i.inputs[0];
              }
            }
            return true;
          }
        return false;
      });
    };
    erase(ir::Op::begin_borrow, false); erase(ir::Op::iteration_begin, false);
    erase(ir::Op::take, false); erase(ir::Op::destroy, false);
    erase(ir::Op::move, false); erase(ir::Op::copy, false); erase(ir::Op::copy, true);
    erase(ir::Op::end_borrow, false); erase(ir::Op::save_output, false); erase(ir::Op::iteration_end, false);
    bad("copy-changed-to-borrow-and-origin-recreated", [](auto &m) {
      for (auto &f : m.functions) for (auto &b : f.blocks) for (auto &i : b.instructions)
        if (i.op == ir::Op::copy) { i.op = ir::Op::borrow; i.origin.reset(); return true; }
      return false;
    });
    bad("one-copy-reused-by-two-owned-reads", [](auto &m) {
      for (auto &f : m.functions) {
        std::map<SymbolId, ValueId> first;
        for (auto &b : f.blocks) for (size_t p = 0; p < b.instructions.size(); ++p) {
          const auto i = b.instructions[p];
          if (i.op != ir::Op::copy || !i.origin || !i.result) continue;
          const auto &n = m.hir.module().nodes.at(i.origin->value);
          const auto *read = std::get_if<hir::Read>(&n.data);
          if (!read || !hir::classify_value(m.hir.module(), n.type).needs_drop) continue;
          if (!first.count(read->symbol)) { first.emplace(read->symbol, *i.result); continue; }
          auto replacement = first.at(read->symbol);
          b.instructions.erase(b.instructions.begin() + p);
          for (auto &block : f.blocks) {
            for (auto &use : block.instructions) for (auto &v : use.inputs)
              if (v == *i.result) v = replacement;
            for (auto &v : block.terminator.inputs) if (v == *i.result) v = replacement;
          }
          return true;
        }
      }
      return false;
    });
    bad("unused-owned-copy-result", [](auto &m) {
      for (auto &f : m.functions) {
        uint64_t next = 0;
        for (const auto &b : f.blocks) {
          for (const auto &p : b.parameters) next = std::max(next, p.value.value + 1);
          for (const auto &i : b.instructions) if (i.result) next = std::max(next, i.result->value + 1);
        }
        for (auto &b : f.blocks) for (size_t p = 0; p < b.instructions.size(); ++p) {
          auto copy = b.instructions[p];
          if (copy.op != ir::Op::copy || !hir::classify_value(m.hir.module(), copy.type).needs_drop) continue;
          copy.result = ValueId{next};
          b.instructions.insert(b.instructions.begin() + p + 1, copy);
          return true;
        }
      }
      return false;
    });
    bad("owning-store-erased-with-live-metadata-recreated", [](auto &m) {
      for (auto &f : m.functions) for (auto &b : f.blocks)
        for (auto it = b.instructions.begin(); it != b.instructions.end(); ++it)
          if (it->op == ir::Op::store && f.slots[it->slot.value].owner &&
              f.slots[it->slot.value].owner->kind == hir::OwnerKind::temporary) {
            auto owner = *f.slots[it->slot.value].owner;
            f.slots[it->slot.value].owner = hir::OwnerRef{owner.kind, owner.identity};
            b.instructions.erase(it); return true;
          }
      return false;
    });
    bad("move-argument-take-before-later-throwing-argument", [](auto &m) {
      for (auto &f : m.functions) for (auto &before : f.blocks)
        if (before.terminator.kind == ir::Exit::try_call)
          for (const auto &store : before.instructions)
            if (store.op == ir::Op::store && f.slots[store.slot.value].owner &&
                f.slots[store.slot.value].owner->kind == hir::OwnerKind::temporary)
              for (auto &after : f.blocks) if (&after != &before)
                for (auto it = after.instructions.begin(); it != after.instructions.end(); ++it)
                  if (it->op == ir::Op::take && it->slot == store.slot) {
                    auto take = *it; after.instructions.erase(it); before.instructions.push_back(take); return true;
                  }
      return false;
    });
    bad("live-flag-identity-erased", [](auto &m) {
      for (auto &f : m.functions) for (auto &s : f.slots) if (s.owner) { s.owner.reset(); return true; }
      return false;
    });
    bad("borrowed-parameter-made-owning", [](auto &m) {
      for (auto &f : m.functions) for (auto &s : f.slots)
        if (s.parameter && s.owner && !s.owning_parameter) { s.owning_parameter = true; return true; }
      return false;
    });
    // Computed owning switch subjects: the real result temporary, its single
    // initialization and the ExitPlan-ordered destroy. Every corruption below
    // must be rejected by the existing slot/live/action-order verification.
    auto subject_owner = [&](const ir::Module &m) -> std::optional<hir::OwnerRef> {
      const auto &h = m.hir.module();
      for (const auto &n : h.nodes)
        if (n.kind() == hir::NodeKind::switch_ &&
            h.nodes[n.children[0].value].kind() != hir::NodeKind::read &&
            hir::classify_value(h, h.nodes[n.children[0].value].type).needs_drop)
          return hir::OwnerRef{hir::OwnerKind::temporary, n.children[0].value};
      return {};
    };
    bad("switch-subject-destroy-deleted", [&](auto &m) {
      auto subject = subject_owner(m); if (!subject) return false;
      for (auto &f : m.functions) for (auto &b : f.blocks)
        for (auto it = b.instructions.begin(); it != b.instructions.end(); ++it)
          if (it->op == ir::Op::destroy && it->owner == subject) {
            b.instructions.erase(it); return true;
          }
      return false;
    });
    for (bool with_owner : {true, false})
      bad(with_owner ? "switch-subject-extra-drop-before-arm"
                     : "switch-subject-extra-drop-without-owner", [&](auto &m) {
        auto subject = subject_owner(m); if (!subject) return false;
        for (auto &f : m.functions) for (auto &b : f.blocks)
          for (size_t p = 0; p < b.instructions.size(); ++p) {
            const auto store = b.instructions[p];
            if (store.op != ir::Op::store ||
                f.slots[store.slot.value].owner != subject) continue;
            for (const auto &bb : f.blocks) for (const auto &i : bb.instructions)
              if (i.op == ir::Op::destroy && i.slot == store.slot) {
                auto extra = i;
                if (!with_owner) extra.owner.reset();
                b.instructions.insert(b.instructions.begin() + p + 1, extra);
                return true;
              }
          }
        return false;
      });
    bad("switch-subject-early-drop-before-arm", [&](auto &m) {
      auto subject = subject_owner(m); if (!subject) return false;
      for (auto &f : m.functions) for (auto &b : f.blocks)
        for (size_t p = 0; p < b.instructions.size(); ++p)
          if (b.instructions[p].op == ir::Op::store &&
              f.slots[b.instructions[p].slot.value].owner == subject)
            for (auto &bb : f.blocks)
              for (auto it = bb.instructions.begin(); it != bb.instructions.end(); ++it)
                if (it->op == ir::Op::destroy && it->slot == b.instructions[p].slot &&
                    it->owner == subject) {
                  auto d = *it; bb.instructions.erase(it);
                  b.instructions.insert(b.instructions.begin() + p + 1, d);
                  return true;
                }
      return false;
    });
    bad("switch-subject-destroy-wrong-owner", [&](auto &m) {
      auto subject = subject_owner(m); if (!subject) return false;
      for (auto &f : m.functions) for (auto &b : f.blocks) for (auto &i : b.instructions)
        if (i.op == ir::Op::destroy && i.owner == subject) {
          i.owner = hir::OwnerRef{hir::OwnerKind::temporary, i.owner->identity + 999999};
          return true;
        }
      return false;
    });
    bad("switch-subject-drop-before-arm-binding", [&](auto &m) {
      auto subject = subject_owner(m); if (!subject) return false;
      for (auto &f : m.functions) for (auto &b : f.blocks)
        for (size_t p = 1; p < b.instructions.size(); ++p)
          if (b.instructions[p].op == ir::Op::destroy && b.instructions[p].owner == subject &&
              b.instructions[p - 1].op == ir::Op::destroy &&
              b.instructions[p - 1].owner &&
              *b.instructions[p - 1].owner != subject) {
            std::swap(b.instructions[p - 1], b.instructions[p]);
            return true;
          }
      return false;
    });
    bad("switch-subject-store-erased", [&](auto &m) {
      auto subject = subject_owner(m); if (!subject) return false;
      for (auto &f : m.functions) for (auto &b : f.blocks)
        for (auto it = b.instructions.begin(); it != b.instructions.end(); ++it)
          if (it->op == ir::Op::store && f.slots[it->slot.value].owner == subject) {
            b.instructions.erase(it); return true;
          }
      return false;
    });
    write(out / "mutations.txt", mutations);
    backend::native::Options options; options.optimization = std::stoul(argv[3]);
    auto generated = backend::native::emit(*cfg.value, main, options);
    if (!generated.success()) for (const auto &d : generated.diagnostics) std::cerr << d.message << '\n';
    need(generated.success(), "LLVM/object emission");
    write(out / "native.txt", generated.value->native_ir);
    write(out / "generated.ll", generated.value->llvm_before_optimization);
    write(out / "optimized.ll", generated.value->llvm_ir);
    llvm::LLVMContext context; llvm::SMDiagnostic diagnostic;
    auto m = llvm::parseAssemblyString(generated.value->llvm_before_optimization, diagnostic, context);
    need(bool(m), "parse generated LLVM");
    auto *i64 = llvm::Type::getInt64Ty(context);
    auto observer = m->getOrInsertFunction("lang_test_value_v1",
        llvm::FunctionType::get(llvm::Type::getVoidTy(context), {i64,i64,i64}, false));
    size_t observations = 0;
    for (const auto &f : h.value->module().functions) {
      const auto &name = h.value->module().resolved->symbols[f.symbol.value].name;
      auto *fn = m->getFunction("lang.fn." + std::to_string(f.symbol.value));
      const auto &sig = *h.value->module().types.find(f.signature);
      std::vector<llvm::ReturnInst *> returns;
      for (auto &b : *fn) if (auto *ret = llvm::dyn_cast<llvm::ReturnInst>(b.getTerminator())) returns.push_back(ret);
      for (auto *ret : returns) {
        llvm::IRBuilder<> b(ret);
        if (name == "mark" && sig.parameters.size() == 2 && sig.parameters[0].mode == hir::ParamMode::inout) {
          // Inspect the generated LLVM function, and use its actual argument
          // types. No guessed C++ aggregate wrapper calls generated functions.
          need(fn->getArg(0)->getType()->isPointerTy() && fn->getArg(1)->getType()->isIntegerTy(), "mark ABI");
          b.CreateCall(observer, {b.getInt64(1), b.CreateIntCast(fn->getArg(1), i64, true), b.CreateLoad(i64, fn->getArg(0))}); ++observations;
        } else if (name == "probe" && !sig.throws_type && sig.parameters.size() == 1 &&
                   sig.parameters[0].mode == hir::ParamMode::inout && ret->getReturnValue()->getType()->isIntegerTy()) {
          need(fn->getArg(0)->getType()->isPointerTy(), "probe ABI");
          b.CreateCall(observer, {b.getInt64(2), b.CreateIntCast(ret->getReturnValue(), i64, true), b.CreateLoad(i64, fn->getArg(0))}); ++observations;
        }
      }
    }
    need(!llvm::verifyModule(*m), "instrumented LLVM verified");
    std::string text; llvm::raw_string_ostream stream(text); m->print(stream, nullptr);
    write(out / "observe.ll", text);
    for (auto &fn : *m) if (!fn.isDeclaration()) fn.addFnAttr(llvm::Attribute::SanitizeAddress);
    std::string sanitizer; llvm::raw_string_ostream sanit_stream(sanitizer); m->print(sanit_stream, nullptr);
    write(out / "sanitize.ll", sanitizer);
    std::cout << "{\"rejected\":" << rejected << ",\"observations\":" << observations << "}\n";
  } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
