#include "lang/backend/native.hpp"
#include "lang/sema/check.hpp"
#include <iostream>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/SourceMgr.h>
#include <set>
using namespace lang;
void require(bool b, const char *s) {
  if (!b)
    throw std::runtime_error(s);
}
int main(int argc, char **argv) {
  try {
    require(argc == 3, "scalar and aggregate float manifests required");
    for (int input = 1; input < argc; ++input) {
    SourceManager sources;
    auto project = modules::load_project({argv[input], {}, {}}, sources);
    require(project.success(), "load");
    auto resolved = modules::resolve_project(
        std::make_shared<const modules::Project>(std::move(*project.value)));
    auto checked = sema::check_project(resolved);
    require(checked.success(), "real check");
    SymbolId entry;
    for (auto &s : resolved.value->symbols)
      if (s.name == "main" && s.kind == modules::SymbolKind::function)
        entry = s.id;
    for (unsigned level : {0u, 2u}) {
      backend::native::Options options;
      options.optimization = level;
      auto emitted = backend::native::emit(*checked.value, entry, options);
      require(emitted.success(), "emit float");
      llvm::LLVMContext context;
      llvm::SMDiagnostic error;
      auto m = llvm::parseAssemblyString(
          emitted.value->llvm_before_optimization, error, context);
      require(m && !llvm::verifyModule(*m), "parse emitted LLVM");
      std::set<std::pair<unsigned, uint64_t>> constants;
      for (auto &f : *m)
        for (auto &b : f)
          for (auto &i : b)
            for (auto &operand : i.operands())
              if (auto *v = llvm::dyn_cast<llvm::ConstantFP>(operand.get())) {
                const auto &number = v->getValueAPF();
                constants.emplace(number.bitcastToAPInt().getBitWidth(),
                                  number.bitcastToAPInt().getZExtValue());
              }
      require(constants.contains({32, UINT64_C(0x3f800000)}) &&
                  constants.contains({64, UINT64_C(0x3ff0000000000000)}) &&
                  constants.contains({32, 1}),
              "IEEE ties-to-even/subnormal bit pattern changed");
      for (auto &f : *m)
        for (auto &b : f)
          for (auto &i : b)
            require(!llvm::isa<llvm::FPMathOperator>(i) ||
                        !i.getFastMathFlags().any(),
                    "unexpected fast-math");
    }
    }
    // These are valid C++ API constructions of invalid LLVM, with no parser
    // involved. This independently exercises the exact linked verifyModule.
    llvm::LLVMContext context;
    llvm::Module bad("invalid", context);
    llvm::IRBuilder<> b(context);
    auto *f = llvm::Function::Create(
        llvm::FunctionType::get(b.getInt32Ty(), {}, false),
        llvm::GlobalValue::ExternalLinkage, "broken", bad);
    auto *block = llvm::BasicBlock::Create(context, "entry", f);
    b.SetInsertPoint(block);
    std::string diagnostics;
    llvm::raw_string_ostream stream(diagnostics);
    require(llvm::verifyModule(bad, &stream),
            "LLVM accepted missing terminator");
    b.CreateRet(b.getInt1(true));
    require(llvm::verifyModule(bad, &stream),
            "LLVM accepted wrong return type");
    std::cout
        << "passed: source float IEEE bits at O0/O2; no fast-math; actual LLVM "
           "verifier rejects missing terminator and wrong return type\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
