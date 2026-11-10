#include "lang/backend/native.hpp"
#include "lang/sema/check.hpp"
#include <iostream>
#include <llvm/ADT/APFloat.h>
#include <llvm/ADT/APInt.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/SourceMgr.h>
#include <map>
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
static void check_float_bits(const llvm::Constant *initializer,
                             unsigned width) {
  const uint32_t f32_bits[] = {0x80000000u, 0x00000000u, 0xbfc00000u,
                               0x3fc00000u, 0xc0100000u, 0x40100000u,
                               0xc0600000u, 0x40600000u};
  const uint64_t f64_bits[] = {0x8000000000000000ull, 0x0000000000000000ull,
                               0xbff8000000000000ull, 0x3ff8000000000000ull,
                               0xc002000000000000ull, 0x4002000000000000ull,
                               0xc00c000000000000ull, 0x400c000000000000ull};
  for (unsigned i = 0; i < 8; ++i) {
    const auto *value =
        llvm::dyn_cast<llvm::ConstantFP>(element(initializer, i));
    need(value != nullptr, "constant float Vec element is not ConstantFP");
    const auto actual = value->getValueAPF().bitcastToAPInt();
    const auto expected =
        width == 32 ? llvm::APInt(32, f32_bits[i], false)
                    : llvm::APInt(64, f64_bits[i], false);
    need(actual.getBitWidth() == width && actual == expected,
         "constant float Vec sign, negative zero or value order changed");
  }
}
static const llvm::CallInst *dynamic_constructor(const llvm::MemCpyInst *copy) {
  const auto *dest =
      llvm::dyn_cast<llvm::GetElementPtrInst>(copy->getRawDest());
  if (!dest || dest->getNumIndices() != 1) return nullptr;
  const auto *load = llvm::dyn_cast<llvm::LoadInst>(
      dest->getPointerOperand()->stripPointerCasts());
  const auto *field = load
                          ? llvm::dyn_cast<llvm::GetElementPtrInst>(
                                load->getPointerOperand())
                          : nullptr;
  if (!field || field->getNumIndices() != 2 ||
      llvm::cast<llvm::ConstantInt>(field->getOperand(2))->getZExtValue() != 5)
    return nullptr;
  return llvm::dyn_cast<llvm::CallInst>(field->getPointerOperand());
}
int main(int argc, char **argv) {
  try {
    need(argc == 2, "Vec negative constant manifest required");
    SourceManager sources;
    auto project = modules::load_project({argv[1], {}, {}}, sources);
    need(project.success(), "load Vec negative constant source");
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

      std::set<const llvm::GlobalVariable *> globals;
      bool saw_f32 = false, saw_f64 = false;
      for (const auto &global : module->globals()) {
        if (!global.getName().starts_with("vec.constants")) continue;
        need(global.isConstant() && global.hasPrivateLinkage() &&
                 global.getUnnamedAddr() !=
                     llvm::GlobalValue::UnnamedAddr::None,
             "negative constant Vec storage must stay private/read-only");
        const auto *array =
            llvm::dyn_cast<llvm::ArrayType>(global.getValueType());
        const auto *initializer = global.getInitializer();
        need(array != nullptr && initializer != nullptr &&
                 array->getNumElements() == 8,
             "negative constant Vec global has the wrong array shape");
        if (array->getElementType()->isFloatTy()) {
          check_float_bits(initializer, 32);
          saw_f32 = true;
        } else if (array->getElementType()->isDoubleTy()) {
          check_float_bits(initializer, 64);
          saw_f64 = true;
        } else {
          need(false, "unexpected negative constant Vec element type");
        }
        globals.insert(&global);
      }
      need(globals.size() == 2 && saw_f32 && saw_f64,
           "expected one read-only f32 and one f64 negative constant array");

      std::map<const llvm::CallInst *, const llvm::MemCpyInst *> bulk;
      std::vector<const llvm::CallInst *> vectors;
      std::map<const llvm::CallInst *,
               std::vector<const llvm::MemCpyInst *>>
          dynamic_copies;
      for (const auto &function : *module)
        for (const auto &block : function)
          for (const auto &instruction : block) {
            if (const auto *copy = llvm::dyn_cast<llvm::MemCpyInst>(&instruction)) {
              const auto *source = llvm::dyn_cast<llvm::GlobalVariable>(
                  copy->getRawSource()->stripPointerCasts());
              if (source && source->getName().starts_with("vec.constants")) {
                const auto *target = llvm::dyn_cast<llvm::LoadInst>(copy->getRawDest());
                const auto *field = target ? llvm::dyn_cast<llvm::GetElementPtrInst>(target->getPointerOperand()) : nullptr;
                const auto *created = field ? llvm::dyn_cast<llvm::CallInst>(field->getPointerOperand()) : nullptr;
                need(created && bulk.emplace(created, copy).second,
                     "negative constant Vec has no unique bulk copy");
              } else if (const auto *created = dynamic_constructor(copy)) {
                dynamic_copies[created].push_back(copy);
              }
            }
            const auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
            if (!call || !call->getCalledFunction()) continue;
            const auto name = call->getCalledFunction()->getName();
            if (name == "lang_vec_new_v1") {
              vectors.push_back(call);
            } else if (name == "lang_vec_init_v1") {
              need(false, "dynamic Vec still calls the runtime initializer");
            }
          }
      need(vectors.size() == 4, "expected four Vec constructors in fixture");

      std::set<const llvm::GlobalVariable *> used;
      std::set<size_t> group_sizes;
      size_t scratch_initializers = 0;
      for (const auto *created : vectors) {
        if (auto found = bulk.find(created); found != bulk.end()) {
          const auto *copy = found->second;
          const auto *global = llvm::dyn_cast<llvm::GlobalVariable>(
              copy->getRawSource()->stripPointerCasts());
          need(global && globals.count(global), "negative constant copy uses the wrong table");
          const auto *bytes = llvm::dyn_cast<llvm::ConstantInt>(copy->getLength());
          need(bytes && bytes->getZExtValue() ==
                   module->getDataLayout().getTypeAllocSize(global->getValueType()).getFixedValue(),
               "negative constant copy does not cover all eight values");
          bool published = false;
          for (const auto &instruction : *copy->getParent()) {
            const auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
            if (!store || !copy->comesBefore(store)) continue;
            const auto *field = llvm::dyn_cast<llvm::GetElementPtrInst>(store->getPointerOperand());
            const auto *count = llvm::dyn_cast<llvm::ConstantInt>(store->getValueOperand());
            if (field && field->getNumIndices() == 2 && field->getPointerOperand() == created &&
                llvm::cast<llvm::ConstantInt>(field->getOperand(2))->getZExtValue() == 2 &&
                count && count->getZExtValue() == 8) published = true;
          }
          need(published, "negative constant Vec is not fully initialized after copy");
          need(dynamic_copies.find(created) == dynamic_copies.end(),
               "constant Vec also uses direct element copies");
          used.insert(global);
          continue;
        }
        auto found = dynamic_copies.find(created);
        need(found != dynamic_copies.end() && !found->second.empty(),
             "dynamic Vec constructor has no element copies");
        const auto &copies = found->second;
        group_sizes.insert(copies.size());
        scratch_initializers += copies.size();
        const llvm::AllocaInst *scratch = nullptr;
        for (size_t n = 0; n < copies.size(); ++n) {
          const auto *copy = copies[n];
          const auto *source = llvm::dyn_cast<llvm::AllocaInst>(
              copy->getRawSource()->stripPointerCasts());
          need(source != nullptr, "dynamic Vec initialization must use scratch");
          if (!scratch) scratch = source;
          need(source == scratch,
               "one dynamic Vec literal uses more than one scratch");
          const auto *length = llvm::dyn_cast<llvm::ConstantInt>(copy->getLength());
          const auto *dest =
              llvm::dyn_cast<llvm::GetElementPtrInst>(copy->getRawDest());
          const auto *offset = dest ? llvm::dyn_cast<llvm::ConstantInt>(dest->getOperand(1)) : nullptr;
          need(length && offset && offset->getZExtValue() ==
                                 n * length->getZExtValue(),
               "dynamic element copy is not at its prefix offset");
          const auto *next = n + 1 < copies.size() ? copies[n + 1] : nullptr;
          bool published = false;
          for (const auto &instruction : *copy->getParent()) {
            if (!copy->comesBefore(&instruction)) continue;
            if (next && !instruction.comesBefore(next)) continue;
            const auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
            if (!store) continue;
            const auto *field = llvm::dyn_cast<llvm::GetElementPtrInst>(store->getPointerOperand());
            const auto *count = llvm::dyn_cast<llvm::ConstantInt>(store->getValueOperand());
            if (field && field->getNumIndices() == 2 &&
                field->getPointerOperand() == created &&
                llvm::cast<llvm::ConstantInt>(field->getOperand(2))->getZExtValue() == 2 &&
                count && count->getZExtValue() == n + 1)
              published = true;
          }
          need(published,
               "dynamic initialized count is not published after its copy");
        }
      }
      need(group_sizes == std::set<size_t>{3, 4} && bulk.size() == 2 && used.size() == 2,
           "negative constants did not use two bulk copies");
      need(scratch_initializers == 7,
           "dynamic negative or side-effect Vec elements changed their path");
    }
    std::cout << "passed: real load/check/lower/emit keeps signed f32/f64 "
                 "constants including -0.0 in private read-only arrays and "
                 "complete copies; dynamic negative and side-effect lists copy "
                 "from scratch without runtime initialization\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
