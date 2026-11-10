#include "lang/backend/native.hpp"
#include "lang/sema/check.hpp"
#include <iostream>
#include <llvm/ADT/APFloat.h>
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
static const llvm::Constant *element(const llvm::Constant *value,
                                     unsigned index) {
  if (const auto *data = llvm::dyn_cast<llvm::ConstantDataSequential>(value))
    return data->getElementAsConstant(index);
  const auto *array = llvm::dyn_cast<llvm::ConstantArray>(value);
  need(array != nullptr, "constant Vec initializer is not an array");
  return array->getAggregateElement(index);
}
static void check_i32(const llvm::Constant *value, uint64_t count,
                      int64_t (*expected)(uint64_t)) {
  for (uint64_t i = 0; i < count; ++i) {
    const auto *integer = llvm::dyn_cast<llvm::ConstantInt>(element(value, i));
    need(integer != nullptr && integer->getBitWidth() == 32 &&
             integer->getSExtValue() == expected(i),
         "constant i32 Vec element/value order changed");
  }
}
int main(int argc, char **argv) {
  try {
    need(argc == 2, "Vec constant manifest required");
    SourceManager sources;
    auto project = modules::load_project({argv[1], {}, {}}, sources);
    need(project.success(), "load Vec constant source");
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

    for (unsigned optimization : {0u, 2u}) {
      backend::native::Options options;
      options.optimization = optimization;
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
      auto after = llvm::parseAssemblyString(emitted.value->llvm_ir,
                                             diagnostic, context);
      need(bool(after), "parse optimized emitted LLVM");
      need(!llvm::verifyModule(*after), "verify optimized emitted LLVM");
      if (optimization != 0) continue;

      std::vector<const llvm::GlobalVariable *> globals;
      unsigned ints = 0, zeros = 0, pairs = 0, bools = 0, f32s = 0, f64s = 0;
      for (const auto &global : module->globals()) {
        if (!global.getName().starts_with("vec.constants")) continue;
        need(global.isConstant() && global.hasPrivateLinkage() &&
                 global.getUnnamedAddr() !=
                     llvm::GlobalValue::UnnamedAddr::None,
             "constant Vec storage must stay private/read-only");
        const auto *array =
            llvm::dyn_cast<llvm::ArrayType>(global.getValueType());
        const auto *initializer = global.getInitializer();
        need(array != nullptr && initializer != nullptr,
             "constant Vec global has no array initializer");
        const uint64_t size = array->getNumElements();
        auto *type = array->getElementType();
        if (size == 256 && type->isIntegerTy(32)) {
          check_i32(initializer, size, [](uint64_t i) {
            return static_cast<int64_t>(i * 37);
          });
          ++ints;
        } else if (size == 128 && type->isIntegerTy(32)) {
          need(llvm::isa<llvm::ConstantAggregateZero>(initializer),
               "all-zero constant Vec did not use zeroinitializer");
          ++zeros;
        } else if (size == 3 && type->isIntegerTy(32)) {
          check_i32(initializer, size, [](uint64_t i) {
            return static_cast<int64_t>(i + 7);
          });
          ++pairs;
        } else if (size == 32 && type->isIntegerTy(1)) {
          for (uint64_t i = 0; i < size; ++i) {
            const auto *value =
                llvm::dyn_cast<llvm::ConstantInt>(element(initializer, i));
            need(value != nullptr && value->getBitWidth() == 1 &&
                     value->getZExtValue() == (i % 3 != 0),
                 "constant bool Vec element/value order changed");
          }
          ++bools;
        } else if (size == 32 && type->isFloatTy()) {
          for (uint64_t i = 0; i < size; ++i) {
            const auto *value =
                llvm::dyn_cast<llvm::ConstantFP>(element(initializer, i));
            need(value != nullptr &&
                     value->getValueAPF().bitwiseIsEqual(
                         llvm::APFloat(static_cast<float>(i) * 0.5f)),
                 "constant f32 Vec bits/order changed");
          }
          ++f32s;
        } else if (size == 32 && type->isDoubleTy()) {
          for (uint64_t i = 0; i < size; ++i) {
            const auto *value =
                llvm::dyn_cast<llvm::ConstantFP>(element(initializer, i));
            need(value != nullptr &&
                     value->getValueAPF().bitwiseIsEqual(llvm::APFloat(
                         static_cast<double>(i) * 0.25 + 0.125)),
                 "constant f64 Vec bits/order changed");
          }
          ++f64s;
        } else {
          need(false, "unexpected constant Vec shape");
        }
        globals.push_back(&global);
      }
      need(globals.size() == 7 && ints == 1 && zeros == 1 && pairs == 2 &&
               bools == 1 && f32s == 1 && f64s == 1,
           "expected seven read-only constant Vec arrays");

      std::set<const llvm::GlobalVariable *> used;
      size_t initializers = 0;
      for (const auto &function : *module)
        for (const auto &block : function)
          for (const auto &instruction : block) {
            const auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
            if (!call || !call->getCalledFunction() ||
                call->getCalledFunction()->getName() != "lang_vec_init_v1")
              continue;
            ++initializers;
            need(call->arg_size() == 2, "unexpected Vec initialization arity");
            const auto *gep =
                llvm::dyn_cast<llvm::GetElementPtrInst>(call->getArgOperand(1));
            need(gep != nullptr && gep->getNumIndices() == 2 &&
                     llvm::isa<llvm::ConstantInt>(gep->getOperand(1)) &&
                     llvm::cast<llvm::ConstantInt>(gep->getOperand(1))
                         ->isZero() &&
                     !llvm::isa<llvm::Constant>(gep->getOperand(2)),
                 "constant Vec initialization is not a compact indexed loop");
            const auto *global = llvm::dyn_cast<llvm::GlobalVariable>(
                gep->getPointerOperand()->stripPointerCasts());
            need(global != nullptr &&
                     global->getName().starts_with("vec.constants"),
                 "constant Vec initialization does not read its global array");
            used.insert(global);
          }
      need(initializers == 7,
           "expected one Vec initialization loop per constant list");
      need(used.size() == globals.size(),
           "a constant Vec array was not used by exactly one list");
    }
    std::cout << "passed: real load/check/lower/emit stores each integer, "
                 "bool and float constant list once in read-only storage and "
                 "initializes it from one compact loop; independent values, "
                 "zeroinitializer and per-list storage are preserved\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
