#include "lang/backend/native.hpp"
#include "lang/sema/check.hpp"
#include <iostream>
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
struct VecGroup {
  const llvm::CallInst *created = nullptr;
  std::vector<const llvm::MemCpyInst *> copies;
};
// A dynamic Vec construction copies from scratch to the constructor's data
// pointer. Constant tables copy from a global straight into the field load and
// therefore do not match this shape.
static const llvm::CallInst *dynamic_constructor(const llvm::MemCpyInst *copy) {
  const auto *dest =
      llvm::dyn_cast<llvm::GetElementPtrInst>(copy->getRawDest());
  if (!dest || dest->getNumIndices() != 1) return nullptr;
  const auto *load = llvm::dyn_cast<llvm::LoadInst>(
      dest->getPointerOperand()->stripPointerCasts());
  if (!load) return nullptr;
  const auto *field = llvm::dyn_cast<llvm::GetElementPtrInst>(load->getPointerOperand());
  if (!field || field->getNumIndices() != 2 ||
      llvm::cast<llvm::ConstantInt>(field->getOperand(2))->getZExtValue() != 5)
    return nullptr;
  return llvm::dyn_cast<llvm::CallInst>(field->getPointerOperand());
}
static const llvm::AllocaInst *element_scratch(const llvm::MemCpyInst *copy) {
  return llvm::dyn_cast<llvm::AllocaInst>(
      copy->getRawSource()->stripPointerCasts());
}
static bool publishes(const llvm::MemCpyInst *copy,
                      const llvm::CallInst *created, size_t n,
                      const llvm::MemCpyInst *next) {
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
      return true;
  }
  return false;
}
struct Signature {
  std::vector<size_t> counts;
  std::vector<std::vector<std::pair<uint64_t, uint64_t>>> copies;
  bool operator==(const Signature &other) const {
    return counts == other.counts && copies == other.copies;
  }
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

    Signature signatures[2];
    bool have_o0 = false;
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

      std::vector<VecGroup> groups;
      std::map<const llvm::CallInst *, size_t> group_of;
      for (const auto &function : *module)
        for (const auto &block : function)
          for (const auto &instruction : block) {
            if (const auto *copy = llvm::dyn_cast<llvm::MemCpyInst>(&instruction)) {
              const auto *created = dynamic_constructor(copy);
              if (created) {
                auto found = group_of.find(created);
                need(found != group_of.end(),
                     "element copy is not attached to its constructor");
                groups[found->second].copies.push_back(copy);
              }
              continue;
            }
            if (const auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction)) {
              const auto *callee = call->getCalledFunction();
              need(!callee || callee->getName() != "lang_vec_init_v1",
                   "dynamic Vec initialization still calls the runtime");
              if (callee && callee->getName() == "lang_vec_new_v1") {
                group_of.emplace(call, groups.size());
                groups.push_back(VecGroup{call, {}});
              }
            }
          }
      for (const auto &function : *after)
        for (const auto &block : function)
          for (const auto &instruction : block)
            if (const auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction))
              need(!call->getCalledFunction() ||
                       call->getCalledFunction()->getName() != "lang_vec_init_v1",
                   "optimized dynamic Vec initialization calls the runtime");

      need(groups.size() == 11, "expected eleven Vec constructors in fixture");
      const std::vector<size_t> expected{3, 2, 2, 2, 2, 1, 2, 1, 2, 2, 0};
      std::set<const llvm::Value *> distinct;
      for (size_t g = 0; g < groups.size(); ++g) {
        const auto &copies = groups[g].copies;
        need(copies.size() == expected[g],
             "unexpected initialized element count for Vec " + std::to_string(g) +
                 ": expected " + std::to_string(expected[g]) + ", got " +
                 std::to_string(copies.size()));
        signatures[optimization / 2].counts.push_back(copies.size());
        signatures[optimization / 2].copies.emplace_back();
        if (copies.empty()) continue;
        const auto *scratch = element_scratch(copies.front());
        need(scratch != nullptr,
             "dynamic Vec copy does not use scratch storage");
        for (size_t n = 0; n < copies.size(); ++n) {
          const auto *copy = copies[n];
          need(element_scratch(copy) && element_scratch(copy) == scratch,
               "one Vec literal uses more than one element scratch");
          const auto *length = llvm::dyn_cast<llvm::ConstantInt>(copy->getLength());
          need(length && length->getZExtValue() ==
                             module->getDataLayout().getTypeAllocSize(
                                 scratch->getAllocatedType()).getFixedValue(),
               "element copy does not cover one element");
          const auto *dest =
              llvm::dyn_cast<llvm::GetElementPtrInst>(copy->getRawDest());
          const auto *offset = dest ? llvm::dyn_cast<llvm::ConstantInt>(
                                          dest->getOperand(1))
                                    : nullptr;
          need(offset && offset->getZExtValue() ==
                             n * length->getZExtValue(),
               "element copy is not at its prefix offset");
          need(copy->getMetadata(llvm::LLVMContext::MD_tbaa) != nullptr,
               "element copy has no payload TBAA tag");
          const auto *next = n + 1 < copies.size() ? copies[n + 1] : nullptr;
          need(publishes(copy, groups[g].created, n, next),
               "initialized count is not published after its element copy");
          signatures[optimization / 2].copies.back().push_back(
              {length->getZExtValue(), offset->getZExtValue()});
        }
        need(distinct.insert(scratch).second,
             "different Vec literals share one element scratch");
      }
      auto *zero_scratch = element_scratch(groups[2].copies.front());
      need(zero_scratch && zero_scratch->getAllocatedType() ==
                               llvm::Type::getInt8Ty(context),
           "zero-sized Vec elements must use an i8 copy-byte scratch");
      auto *unit_scratch = element_scratch(groups[3].copies.front());
      need(unit_scratch && unit_scratch->getAllocatedType() ==
                               llvm::Type::getInt8Ty(context),
           "Unit Vec elements must use an i8 copy-byte scratch");
      bool zero_initialized = false;
      for (const auto &instruction : *groups[2].copies.front()->getParent()) {
        if (!instruction.comesBefore(groups[2].copies.front())) continue;
        const auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
        if (!store || store->getPointerOperand()->stripPointerCasts() != zero_scratch)
          continue;
        const auto *byte = llvm::dyn_cast<llvm::ConstantInt>(store->getValueOperand());
        zero_initialized = byte && byte->getType()->isIntegerTy(8) && byte->isZero();
      }
      need(zero_initialized,
           "zero-sized Vec copies from an uninitialized scratch");
      have_o0 = have_o0 || optimization == 0;
    }
    need(have_o0 && signatures[0] == signatures[1],
         "direct Vec initialization differs between O0 and O2");
    std::cout << "passed: dynamic Vec literals copy one element per direct "
                 "memcpy, publish initialized prefixes, keep one scratch per "
                 "literal, and match at O0/O2\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
