#include "lang/backend/native.hpp"
#include "lang/sema/check.hpp"
#include <iostream>
#include <llvm/ADT/APFloat.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
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
      size_t copies = 0;
      for (const auto &function : *module)
        for (const auto &block : function)
          for (const auto &instruction : block) {
            if (const auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction))
              need(!call->getCalledFunction() ||
                       call->getCalledFunction()->getName() != "lang_vec_init_v1",
                   "constant Vec still initializes one element per runtime call");
            const auto *copy = llvm::dyn_cast<llvm::MemCpyInst>(&instruction);
            if (!copy) continue;
            const auto *global = llvm::dyn_cast<llvm::GlobalVariable>(
                copy->getRawSource()->stripPointerCasts());
            need(global && global->getName().starts_with("vec.constants"),
                 "bulk Vec copy does not read its constant table");
            auto *array = llvm::cast<llvm::ArrayType>(global->getValueType());
            const auto *length = llvm::dyn_cast<llvm::ConstantInt>(copy->getLength());
            need(length && length->getZExtValue() ==
                     module->getDataLayout().getTypeAllocSize(array).getFixedValue(),
                 "bulk Vec copy does not cover the complete table");
            const auto *target = llvm::dyn_cast<llvm::LoadInst>(copy->getRawDest());
            const auto *data = target ? llvm::dyn_cast<llvm::GetElementPtrInst>(
                                           target->getPointerOperand()) : nullptr;
            need(data && data->getNumIndices() == 2 &&
                     llvm::cast<llvm::ConstantInt>(data->getOperand(2))->getZExtValue() == 5,
                 "bulk Vec copy does not target its data pointer");
            const auto *created = llvm::dyn_cast<llvm::CallInst>(data->getPointerOperand());
            need(created && created->getCalledFunction() &&
                     created->getCalledFunction()->getName() == "lang_vec_new_v1" &&
                     llvm::cast<llvm::ConstantInt>(created->getArgOperand(0))->getZExtValue() == array->getNumElements() &&
                     llvm::cast<llvm::Constant>(created->getArgOperand(2))->isNullValue() &&
                     llvm::cast<llvm::Constant>(created->getArgOperand(3))->isNullValue(),
                 "bulk Vec copy is not a fresh trivial-element allocation");
            bool published = false;
            for (const auto &following : block) {
              const auto *store = llvm::dyn_cast<llvm::StoreInst>(&following);
              if (!store || !copy->comesBefore(store)) continue;
              const auto *field = llvm::dyn_cast<llvm::GetElementPtrInst>(store->getPointerOperand());
              const auto *count = llvm::dyn_cast<llvm::ConstantInt>(store->getValueOperand());
              if (field && field->getNumIndices() == 2 &&
                  field->getPointerOperand() == created && count &&
                  llvm::cast<llvm::ConstantInt>(field->getOperand(2))->getZExtValue() == 2 &&
                  count->getZExtValue() == array->getNumElements()) published = true;
            }
            need(published, "Vec initialized count is not published after its full copy");
            need(used.insert(global).second, "a constant table is copied twice");
            ++copies;
          }
      need(copies == 7 && used.size() == globals.size(),
           "expected one complete copy per constant Vec list");
    }
    std::cout << "passed: real load/check/lower/emit stores each integer, "
                 "bool and float constant list once in read-only storage and "
                 "initializes it with one full copy; independent values, "
                 "zeroinitializer and per-list storage are preserved\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
