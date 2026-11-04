// Test-only observation: call generated functions with their actual LLVM types.
#include "lang/backend/native.hpp"
#include "lang/sema/check.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/SourceMgr.h>
#include <sstream>
using namespace lang;
void need(bool ok, const std::string &s) {
  if (!ok)
    throw std::runtime_error(s);
}
void write(const std::filesystem::path &p, const std::string &s) {
  std::ofstream f(p, std::ios::binary);
  f.write(s.data(), s.size());
  need(bool(f), "write artifact");
}
int main(int argc, char **argv) {
  try {
    need(argc == 5, "manifest out optimization requests required");
    std::filesystem::path out = argv[2];
    std::filesystem::create_directories(out);
    SourceManager sources;
    auto project = modules::load_project({argv[1], {}, {}}, sources);
    need(project.success(), "load");
    project.value->backend = "native";
    auto resolved = modules::resolve_project(
        std::make_shared<const modules::Project>(std::move(*project.value)));
    auto checked = sema::check_project(resolved);
    if (!checked.success())
      for (auto &d : checked.diagnostics)
        std::cerr << d.code << ": " << d.message << '\n';
    need(checked.success(), "check real project");
    auto lower = ir::lower(*checked.value);
    need(lower.success(), "lower verified HIR");
    size_t rejected = 0;
    // Keep the existing CFG verifier checks, then separately corrupt floating
    // instructions without changing the verified HIR. Every mutation must fail.
    const auto original = lower.value->module();
    for (bool unary : {true, false}) {
      bool found = false;
      size_t fi = 0, bi = 0, ii = 0;
      for (size_t f = 0; f < original.functions.size() && !found; ++f)
        for (size_t b = 0; b < original.functions[f].blocks.size() && !found;
             ++b)
          for (size_t i = 0;
               i < original.functions[f].blocks[b].instructions.size(); ++i) {
            const auto &v = original.functions[f].blocks[b].instructions[i];
            if (v.op == (unary ? ir::Op::unary : ir::Op::binary) &&
                original.hir.module().types.find(v.type)->kind ==
                    hir::TypeKind::Float) {
              found = true;
              fi = f;
              bi = b;
              ii = i;
              break;
            }
          }
      if (!found)
        continue;
      auto corrupt = [&](auto change) {
        auto m = original;
        auto &f = m.functions[fi];
        auto &b = f.blocks[bi];
        change(m, f, b, b.instructions[ii]);
        auto r = ir::verify(m);
        need(!r.success() && !r.value && r.failure == FailureKind::internal &&
                 r.diagnostics.at(0).code == "E_IR_INVALID",
             "bad floating IR accepted");
        ++rejected;
      };
      if (unary) {
        for (auto op : {hir::UnaryOp::bit_not, hir::UnaryOp::logical_not})
          corrupt([&](auto &, auto &, auto &, auto &i) { i.unary = op; });
      } else {
        for (auto op : {hir::BinaryOp::remainder, hir::BinaryOp::bit_and,
                        hir::BinaryOp::bit_or, hir::BinaryOp::bit_xor,
                        hir::BinaryOp::shift_left, hir::BinaryOp::shift_right})
          corrupt([&](auto &, auto &, auto &, auto &i) { i.binary = op; });
        corrupt([](auto &, auto &, auto &, auto &i) {
          i.binary = hir::BinaryOp::equal;
        }); // float result, not bool
        for (auto kind : {hir::TypeKind::Float, hir::TypeKind::SignedInt}) {
          const auto width =
              original.hir.module()
                  .types
                  .find(original.functions[fi].blocks[bi].instructions[ii].type)
                  ->bits;
          TypeId other;
          for (uint64_t n = 1; n <= original.hir.module().types.size(); ++n) {
            auto *t = original.hir.module().types.find(TypeId{n});
            if (t && t->kind == kind &&
                (kind != hir::TypeKind::Float || t->bits != width)) {
              other = TypeId{n};
              break;
            }
          }
          // A single-width source need not intern the other precision.
          // The dynamic project contains both and exercises both mutations.
          if (other.value == 0)
            continue;
          corrupt([&](auto &m, auto &, auto &b, auto &i) {
            ValueId next{0};
            for (auto &f : m.functions)
              for (auto &block : f.blocks) {
                for (auto &p : block.parameters)
                  next.value = std::max(next.value, p.value.value + 1);
                for (auto &v : block.instructions)
                  if (v.result)
                    next.value = std::max(next.value, v.result->value + 1);
              }
            i.inputs[1] = next;
            ir::Instruction c;
            c.op = ir::Op::constant;
            c.type = other;
            c.result = next;
            c.source = i.source;
            b.instructions.insert(b.instructions.begin(), c);
          });
        }
      }
      corrupt([](auto &, auto &, auto &, auto &i) { i.result.reset(); });
      corrupt([](auto &, auto &, auto &, auto &i) { i.inputs.pop_back(); });
    }
    SymbolId entry;
    std::map<std::string, SymbolId> names;
    for (const auto &s : resolved.value->symbols)
      if (s.kind == modules::SymbolKind::function) {
        names.emplace(s.name, s.id);
        if (s.name == "main")
          entry = s.id;
      }
    backend::native::Options options;
    options.optimization = unsigned(std::stoul(argv[3]));
    auto emitted = backend::native::emit(*lower.value, entry, options);
    need(emitted.success(), "emit");
    write(out / "generated.o", std::string(emitted.value->object.begin(),
                                           emitted.value->object.end()));
    write(out / "before.ll", emitted.value->llvm_before_optimization);
    write(out / "after.ll", emitted.value->llvm_ir);
    write(out / "generated.nir", emitted.value->native_ir);
    llvm::LLVMContext context;
    llvm::SMDiagnostic error;
    auto check_math = [&](const std::string &ir) {
      auto m = llvm::parseAssemblyString(ir, error, context);
      need(m && !llvm::verifyModule(*m), "verify emitted LLVM");
      for (auto &f : *m) {
        need(!f.getName().starts_with("llvm.fma") &&
                 !f.getName().starts_with("llvm.fmuladd"),
             "unexpected FMA");
        for (auto &b : f)
          for (auto &i : b)
            if (llvm::isa<llvm::FPMathOperator>(i))
              need(!i.getFastMathFlags().any(), "fast/contract flag");
      }
      return m;
    };
    check_math(emitted.value->llvm_ir);
    auto m = check_math(emitted.value->llvm_before_optimization);
    auto *main = m->getFunction("main");
    main->deleteBody();
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(context, "observe", main));
    auto writer = m->getOrInsertFunction(
        "write", llvm::FunctionType::get(
                     b.getInt64Ty(),
                     {b.getInt32Ty(), b.getPtrTy(), b.getInt64Ty()}, false));
    // Observe the SSE floating environment in the actual execution process.
    // This test target, like the production emitter, is Linux x86_64 only.
    auto *mxcsr = b.CreateAlloca(b.getInt32Ty());
    auto *read_mxcsr = llvm::InlineAsm::get(
        llvm::FunctionType::get(b.getVoidTy(), {b.getPtrTy()}, false),
        "stmxcsr ($0)", "r,~{memory}", true);
    b.CreateCall(read_mxcsr, {mxcsr});
    b.CreateCall(writer, {b.getInt32(1), mxcsr, b.getInt64(4)});
    std::ifstream requests(argv[4]);
    need(bool(requests), "open requests");
    std::string line;
    size_t count = 0;
    while (std::getline(requests, line)) {
      std::istringstream row(line);
      std::string name, result, bits;
      row >> name >> result;
      need(names.contains(name), "missing function " + name);
      auto *f =
          m->getFunction("lang.fn." + std::to_string(names.at(name).value));
      need(f, "missing generated function");
      auto *ret = f->getReturnType();
      unsigned width = result == "f32"    ? 32
                       : result == "f64"  ? 64
                       : result == "bool" ? 1
                                          : 0;
      need(width && (width == 1    ? ret->isIntegerTy(1)
                     : width == 32 ? ret->isFloatTy()
                                   : ret->isDoubleTy()),
           "actual return type differs from request");
      std::vector<llvm::Value *> args;
      while (row >> bits) {
        need(args.size() < f->arg_size(), "extra argument");
        auto *t = f->getFunctionType()->getParamType(args.size());
        unsigned w = t->isFloatTy() ? 32 : t->isDoubleTy() ? 64 : 0;
        need(w && bits.size() == w / 4,
             "actual argument precision differs from request");
        auto *raw = llvm::ConstantInt::get(context, llvm::APInt(w, bits, 16));
        args.push_back(llvm::ConstantExpr::getBitCast(raw, t));
      }
      need(args.size() == f->arg_size(), "argument count");
      auto *actual = b.CreateCall(f->getFunctionType(), f, args);
      llvm::Value *raw = width == 1
                             ? b.CreateZExt(actual, b.getInt8Ty())
                             : b.CreateBitCast(actual, b.getIntNTy(width));
      auto *slot = b.CreateAlloca(raw->getType());
      b.CreateStore(raw, slot);
      b.CreateCall(writer, {b.getInt32(1), slot,
                            b.getInt64(width == 1 ? 1 : width / 8)});
      ++count;
    }
    b.CreateRet(b.getInt32(0));
    need(!llvm::verifyModule(*m), "observation module invalid");
    std::string text;
    llvm::raw_string_ostream stream(text);
    m->print(stream, nullptr);
    write(out / "observe.ll", text);
    std::string metadata =
        "{\"status\":\"passed\",\"probes\":" + std::to_string(count) +
        ",\"bad_ir_rejected\":" + std::to_string(rejected) + "}\n";
    write(out / "metadata.json", metadata);
    std::cout << metadata;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
