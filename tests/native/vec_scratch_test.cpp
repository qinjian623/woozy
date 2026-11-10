#include "lang/backend/native.hpp"
#include "lang/sema/check.hpp"
#include <iostream>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/SourceMgr.h>
#include <set>
#include <stdexcept>
#include <vector>
using namespace lang;
static void need(bool ok, const std::string &message) {
  if (!ok) throw std::runtime_error(message);
}
struct VecGroup {
  const llvm::CallInst *created = nullptr;
  std::vector<const llvm::Value *> scratch;
};
int main(int argc, char **argv) {
  try {
    need(argc == 2, "Vec scratch manifest required");
    SourceManager sources;
    auto project = modules::load_project({argv[1], {}, {}}, sources);
    need(project.success(), "load Vec scratch source");
    auto resolved = modules::resolve_project(
        std::make_shared<const modules::Project>(std::move(*project.value)));
    auto checked = sema::check_project(resolved);
    need(checked.success(), "real check");
    auto lowered = ir::lower(*checked.value);
    if (!lowered.success())
      for (const auto &d : lowered.diagnostics) std::cerr << d.message << '\n';
    need(lowered.success(), "real NativeIR verification");
    SymbolId entry;
    for (const auto &s : resolved.value->symbols)
      if (s.name == "main" && s.kind == modules::SymbolKind::function)
        entry = s.id;
    backend::native::Options options;
    options.optimization = 0;
    auto emitted = backend::native::emit(*lowered.value, entry, options);
    if (!emitted.success())
      for (const auto &d : emitted.diagnostics) std::cerr << d.message << '\n';
    need(emitted.success(), "real LLVM emission");

    llvm::LLVMContext context;
    llvm::SMDiagnostic diagnostic;
    auto module = llvm::parseAssemblyString(
        emitted.value->llvm_before_optimization, diagnostic, context);
    need(bool(module), "parse unoptimized emitted LLVM");
    need(!llvm::verifyModule(*module), "verify unoptimized emitted LLVM");

    std::vector<VecGroup> groups;
    for (const auto &function : *module)
      for (const auto &block : function)
        for (const auto &instruction : block) {
          const auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
          if (!call) continue;
          const auto *callee = call->getCalledFunction();
          if (!callee) continue;
          if (callee->getName() == "lang_vec_new_v1") {
            groups.push_back(VecGroup{call, {}});
            continue;
          }
          if (callee->getName() != "lang_vec_init_v1") continue;
          need(!groups.empty() && groups.back().created,
               "Vec element initialization without its constructor");
          need(call->arg_size() == 2, "unexpected Vec initialization arity");
          need(call->getArgOperand(0) == groups.back().created,
               "Vec initialization is not attached to its constructor");
          auto *scratch = call->getArgOperand(1)->stripPointerCasts();
          need(llvm::isa<llvm::AllocaInst>(scratch),
               "Vec element initialization does not use scratch storage");
          groups.back().scratch.push_back(scratch);
        }

    need(groups.size() == 5, "expected five Vec constructors in fixture");
    const std::vector<size_t> expected{3, 2, 2, 2, 0};
    std::set<const llvm::Value *> distinct;
    for (size_t g = 0; g < groups.size(); ++g) {
      need(groups[g].scratch.size() == expected[g],
           "unexpected initialized element count for Vec " + std::to_string(g));
      if (groups[g].scratch.empty()) continue;
      auto *scratch = groups[g].scratch.front();
      for (auto *address : groups[g].scratch)
        need(address == scratch,
             "one Vec literal uses more than one element scratch");
      need(distinct.insert(scratch).second,
           "different Vec literals share one element scratch");
    }
    auto *zero_scratch =
        llvm::cast<llvm::AllocaInst>(groups[2].scratch.front());
    need(zero_scratch->getAllocatedType() ==
             llvm::Type::getInt8Ty(context),
         "zero-sized Vec elements must use an i8 copy-byte scratch");
    auto *unit_scratch =
        llvm::cast<llvm::AllocaInst>(groups[3].scratch.front());
    need(unit_scratch->getAllocatedType() ==
             llvm::Type::getInt8Ty(context),
         "Unit Vec elements must use an i8 copy-byte scratch");
    bool zero_initialized = false;
    for (const auto &function : *module)
      for (const auto &block : function) {
        for (const auto &instruction : block) {
          if (const auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction))
            if (store->getPointerOperand()->stripPointerCasts() == zero_scratch)
              if (const auto *byte =
                      llvm::dyn_cast<llvm::ConstantInt>(store->getValueOperand()))
                zero_initialized = byte->getType()->isIntegerTy(8) &&
                                   byte->isZero();
          if (const auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction))
            if (const auto *callee = call->getCalledFunction())
              if (callee->getName() == "lang_vec_init_v1" &&
                  call->getArgOperand(1)->stripPointerCasts() == zero_scratch) {
                need(zero_initialized,
                     "zero-sized Vec copies from an uninitialized scratch");
                break;
              }
        }
      }
    std::cout << "passed: real load/check/lower/emit gives each Vec literal "
                 "one scratch alloca; distinct Vec literals keep distinct "
                 "scratch storage; empty literal initializes no element\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
