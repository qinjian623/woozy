#include "lang/backend/native.hpp"
#include <functional>
#include <llvm/ADT/APFloat.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <mutex>
#include <set>

static_assert(LLVM_VERSION_MAJOR == 22 && LLVM_VERSION_MINOR == 1 &&
              LLVM_VERSION_PATCH == 8);
namespace lang::backend::native {
namespace {
bool signed_(const hir::Type &t) {
  return t.kind == hir::TypeKind::SignedInt || t.kind == hir::TypeKind::Isize;
}
struct Failure {
  std::string code, message;
  FailureKind kind;
};
void fail(std::string message) {
  throw Failure{"E_IR_INVALID", std::move(message), FailureKind::internal};
}
void unsupported(std::string message) {
  throw Failure{"E_CAPABILITY_UNAVAILABLE", std::move(message),
                FailureKind::unsupported};
}
std::string text_ir(const llvm::Module &m) {
  std::string s;
  llvm::raw_string_ostream stream(s);
  m.print(stream, nullptr);
  return s;
}
void verify_llvm(const llvm::Module &m) {
  std::string error;
  llvm::raw_string_ostream stream(error);
  if (llvm::verifyModule(m, &stream))
    fail("LLVM verifier: " + stream.str());
}
class Emitter {
  const ir::Module &input;
  const hir::TypeContext &types;
  llvm::LLVMContext context;
  llvm::Module module{"lang.native.scalars", context};
  // Keep guards and dangerous operations in their actual blocks at O0, even
  // for literal operands. ConstantFolder would create poison in dead arms.
  llvm::IRBuilder<llvm::NoFolder> builder{context};
  std::map<SymbolId, llvm::Function *> functions;
  std::unique_ptr<llvm::TargetMachine> machine;
  std::map<ValueId, llvm::Value *> values;
  std::map<ValueId, TypeId> value_types;
  std::vector<llvm::Value *> slots;
  const hir::Type &type(TypeId id) const { return *types.find(id); }
  llvm::Type *lower_type(TypeId id) {
    const auto &t = type(id);
    switch (t.kind) {
    case hir::TypeKind::Unit:
      return builder.getInt8Ty();
    case hir::TypeKind::Bool:
      return builder.getInt1Ty();
    case hir::TypeKind::SignedInt:
    case hir::TypeKind::UnsignedInt:
    case hir::TypeKind::Isize:
    case hir::TypeKind::Usize:
      return builder.getIntNTy(hir::bit_width(t, input.hir.config()));
    case hir::TypeKind::Float:
      return t.bits == 32 ? builder.getFloatTy() : builder.getDoubleTy();
    case hir::TypeKind::Function:
      return builder.getPtrTy();
    default:
      unsupported("native scalar type is not implemented");
    }
    return nullptr;
  }
  llvm::FunctionType *signature(TypeId id) {
    const auto &t = type(id);
    std::vector<llvm::Type *> parameters;
    for (auto p : t.parameters)
      parameters.push_back(p.mode == hir::ParamMode::inout
                               ? builder.getPtrTy()
                               : lower_type(p.type));
    return llvm::FunctionType::get(lower_type(t.result), parameters, false);
  }
  void guard(llvm::Value *bad, uint32_t cause, SourceSpan s) {
    auto *fn = builder.GetInsertBlock()->getParent();
    auto *panic = llvm::BasicBlock::Create(context, "panic", fn);
    auto *ok = llvm::BasicBlock::Create(context, "checked", fn);
    builder.CreateCondBr(bad, panic, ok);
    builder.SetInsertPoint(panic);
    auto *panic_type =
        llvm::FunctionType::get(builder.getVoidTy(),
                                {builder.getInt32Ty(), builder.getInt64Ty(),
                                 builder.getInt64Ty(), builder.getInt64Ty()},
                                false);
    auto panic_fn =
        module.getOrInsertFunction("lang_native_panic_v1", panic_type);
    // The runtime really terminates and performs no exception unwinding.
    auto *decl = llvm::cast<llvm::Function>(panic_fn.getCallee());
    decl->addFnAttr(llvm::Attribute::NoReturn);
    decl->addFnAttr(llvm::Attribute::NoUnwind);
    builder.CreateCall(panic_fn, {builder.getInt32(cause),
                                  builder.getInt64(s.snapshot.value),
                                  builder.getInt64(s.begin_byte),
                                  builder.getInt64(s.end_byte)});
    builder.CreateUnreachable();
    builder.SetInsertPoint(ok);
  }
  llvm::Value *binary(const ir::Instruction &i, llvm::Value *a,
                      llvm::Value *b) {
    const auto &left = type(value_types.at(i.inputs[0]));
    bool sign = signed_(left);
    using B = hir::BinaryOp;
    switch (i.binary) {
    case B::add:
      return builder.CreateAdd(a, b);
    case B::subtract:
      return builder.CreateSub(a, b);
    case B::multiply:
      return builder.CreateMul(a, b);
    case B::bit_and:
      return builder.CreateAnd(a, b);
    case B::bit_or:
      return builder.CreateOr(a, b);
    case B::bit_xor:
      return builder.CreateXor(a, b);
    case B::equal:
      return builder.CreateICmpEQ(a, b);
    case B::not_equal:
      return builder.CreateICmpNE(a, b);
    case B::less:
      return sign ? builder.CreateICmpSLT(a, b) : builder.CreateICmpULT(a, b);
    case B::less_equal:
      return sign ? builder.CreateICmpSLE(a, b) : builder.CreateICmpULE(a, b);
    case B::greater:
      return sign ? builder.CreateICmpSGT(a, b) : builder.CreateICmpUGT(a, b);
    case B::greater_equal:
      return sign ? builder.CreateICmpSGE(a, b) : builder.CreateICmpUGE(a, b);
    case B::divide:
    case B::remainder: {
      guard(builder.CreateICmpEQ(b, llvm::ConstantInt::get(b->getType(), 0)), 1,
            i.source);
      bool rem = i.binary == B::remainder;
      if (!sign)
        return rem ? builder.CreateURem(a, b) : builder.CreateUDiv(a, b);
      unsigned width = a->getType()->getIntegerBitWidth();
      auto *minimum = llvm::ConstantInt::get(
          context, llvm::APInt::getSignedMinValue(width));
      auto *minus_one =
          llvm::ConstantInt::get(context, llvm::APInt::getAllOnes(width));
      auto *special = builder.CreateAnd(builder.CreateICmpEQ(a, minimum),
                                        builder.CreateICmpEQ(b, minus_one));
      auto *fn = builder.GetInsertBlock()->getParent();
      auto *overflow = llvm::BasicBlock::Create(context, "division.wrap", fn);
      auto *normal = llvm::BasicBlock::Create(context, "division.normal", fn);
      auto *join = llvm::BasicBlock::Create(context, "division.result", fn);
      builder.CreateCondBr(special, overflow, normal);
      builder.SetInsertPoint(overflow);
      auto *wrapped = rem ? llvm::ConstantInt::get(a->getType(), 0) : minimum;
      builder.CreateBr(join);
      builder.SetInsertPoint(normal);
      auto *result = rem ? builder.CreateSRem(a, b) : builder.CreateSDiv(a, b);
      builder.CreateBr(join);
      builder.SetInsertPoint(join);
      auto *phi = builder.CreatePHI(a->getType(), 2);
      phi->addIncoming(wrapped, overflow);
      phi->addIncoming(result, normal);
      return phi;
    }
    case B::shift_left:
    case B::shift_right: {
      const auto &rhs = type(value_types.at(i.inputs[1]));
      unsigned width = a->getType()->getIntegerBitWidth();
      // Widen the RHS before comparing. Comparing width=64 in e.g. i8 is
      // safe here too, but no narrowed LHS or sign-discarding cast is used.
      auto *amount =
          builder.CreateIntCast(b, builder.getIntNTy(65), signed_(rhs));
      auto *bad = builder.CreateICmpUGE(
          amount, llvm::ConstantInt::get(amount->getType(), width));
      if (signed_(rhs))
        bad = builder.CreateOr(
            bad, builder.CreateICmpSLT(
                     amount, llvm::ConstantInt::get(amount->getType(), 0)));
      guard(bad, 2, i.source);
      auto *checked = builder.CreateIntCast(amount, a->getType(), false);
      if (i.binary == B::shift_left)
        return builder.CreateShl(a, checked);
      return sign ? builder.CreateAShr(a, checked)
                  : builder.CreateLShr(a, checked);
    }
    }
    fail("unknown binary operation");
    return nullptr;
  }
  llvm::Value *convert(const ir::Instruction &i, llvm::Value *v) {
    const auto &from = type(value_types.at(i.inputs[0])), &to = type(i.type);
    // i65 can represent every source integer and every destination bound.
    auto *wide_type = builder.getIntNTy(65);
    auto *wide = builder.CreateIntCast(v, wide_type, signed_(from));
    unsigned width = hir::bit_width(to, input.hir.config());
    auto low = signed_(to) ? llvm::APInt::getSignedMinValue(width).sext(65)
                           : llvm::APInt(65, 0);
    auto high = signed_(to) ? llvm::APInt::getSignedMaxValue(width).zext(65)
                            : llvm::APInt::getMaxValue(width).zext(65);
    auto *bad = builder.CreateOr(
        builder.CreateICmpSLT(wide, llvm::ConstantInt::get(context, low)),
        builder.CreateICmpSGT(wide, llvm::ConstantInt::get(context, high)));
    guard(bad, 3, i.source);
    return builder.CreateIntCast(v, lower_type(i.type), signed_(from));
  }
  void instruction(const ir::Instruction &i) {
    std::vector<llvm::Value *> args;
    for (auto v : i.inputs)
      args.push_back(values.at(v));
    llvm::Value *v = nullptr;
    switch (i.op) {
    case ir::Op::constant: {
      const auto &t = type(i.type);
      if (t.kind == hir::TypeKind::Float) {
        llvm::APFloat bits(t.bits == 32 ? llvm::APFloat::IEEEsingle()
                                        : llvm::APFloat::IEEEdouble(),
                           llvm::APInt(t.bits, i.bits));
        v = llvm::ConstantFP::get(context, bits);
      } else
        v = llvm::ConstantInt::get(lower_type(i.type), i.bits);
      break;
    }
    case ir::Op::function:
      v = functions.at(i.function);
      break;
    case ir::Op::load:
      v = builder.CreateLoad(lower_type(i.type), slots.at(i.slot.value));
      break;
    case ir::Op::address:
      v = slots.at(i.slot.value);
      break;
    case ir::Op::store:
      builder.CreateStore(args[0], slots.at(i.slot.value));
      break;
    case ir::Op::unary:
      v = i.unary == hir::UnaryOp::negate ? builder.CreateNeg(args[0])
                                          : builder.CreateNot(args[0]);
      break;
    case ir::Op::binary:
      v = binary(i, args[0], args[1]);
      break;
    case ir::Op::convert:
      v = convert(i, args[0]);
      break;
    case ir::Op::call: {
      llvm::SmallVector<llvm::Value *, 8> actual(args.begin() + 1, args.end());
      v = builder.CreateCall(signature(i.signature), args[0], actual);
      break;
    }
    }
    if (i.result) {
      values.emplace(*i.result, v);
      value_types.emplace(*i.result, i.type);
    }
  }
  void function(const ir::Function &f) {
    values.clear();
    value_types.clear();
    slots.clear();
    auto *fn = functions.at(f.symbol);
    std::vector<llvm::BasicBlock *> blocks;
    for (size_t b = 0; b < f.blocks.size(); ++b)
      blocks.push_back(
          llvm::BasicBlock::Create(context, "b" + std::to_string(b), fn));
    builder.SetInsertPoint(blocks[0]);
    for (const auto &slot : f.slots) {
      llvm::Value *storage;
      if (slot.inout)
        storage = fn->getArg(*slot.parameter);
      else
        storage = builder.CreateAlloca(lower_type(slot.type));
      slots.push_back(storage);
    }
    for (size_t s = 0; s < f.slots.size(); ++s) {
      const auto &slot = f.slots[s];
      if (slot.parameter && !slot.inout)
        builder.CreateStore(fn->getArg(*slot.parameter), slots[s]);
    }
    // Dominating definitions precede uses, including nested short-circuit CFGs
    // whose numeric block IDs are not topologically ordered.
    std::set<size_t> visited;
    std::vector<size_t> order;
    std::function<void(size_t, std::vector<size_t> &)> dfs = [&](size_t b,
                                                                 auto &post) {
      if (!visited.insert(b).second)
        return;
      for (auto d : f.blocks[b].terminator.targets)
        dfs(d.value, post);
      post.push_back(b);
    };
    for (size_t b = 0; b < f.blocks.size(); ++b)
      if (!visited.contains(b)) {
        std::vector<size_t> post;
        dfs(b, post);
        order.insert(order.end(), post.rbegin(), post.rend());
      }
    for (auto b : order) {
      builder.SetInsertPoint(blocks[b]);
      for (const auto &i : f.blocks[b].instructions)
        instruction(i);
      const auto &t = f.blocks[b].terminator;
      switch (t.kind) {
      case ir::Exit::branch:
        builder.CreateBr(blocks[t.targets[0].value]);
        break;
      case ir::Exit::conditional:
        builder.CreateCondBr(values.at(t.inputs[0]), blocks[t.targets[0].value],
                             blocks[t.targets[1].value]);
        break;
      case ir::Exit::return_:
        builder.CreateRet(values.at(t.inputs[0]));
        break;
      case ir::Exit::unreachable:
        builder.CreateUnreachable();
        break;
      default:
        fail("unverified terminator reached emitter");
      }
    }
    std::string error;
    llvm::raw_string_ostream stream(error);
    if (llvm::verifyFunction(*fn, &stream))
      fail("LLVM function verifier: " + stream.str());
  }

public:
  Emitter(const ir::Module &m, const Options &options)
      : input(m), types(m.hir.module().types) {
    static std::once_flag initialized;
    std::call_once(initialized, [] {
      llvm::InitializeNativeTarget();
      llvm::InitializeNativeTargetAsmPrinter();
    });
    std::string error;
    auto triple = llvm::Triple(options.triple);
    auto *target = llvm::TargetRegistry::lookupTarget(triple, error);
    if (!target)
      throw Failure{"E_TOOLCHAIN_MISSING", error, FailureKind::environment};
    machine.reset(target->createTargetMachine(
        triple, options.cpu, options.features, llvm::TargetOptions{},
        llvm::Reloc::PIC_, llvm::CodeModel::Small,
        options.optimization == 2 ? llvm::CodeGenOptLevel::Default
                                  : llvm::CodeGenOptLevel::None));
    if (!machine)
      throw Failure{"E_TOOLCHAIN_MISSING", "cannot create x86_64 TargetMachine",
                    FailureKind::environment};
    module.setTargetTriple(triple);
    module.setDataLayout(machine->createDataLayout());
    if (module.getDataLayout().getPointerSizeInBits() != 64)
      fail("x86_64 DataLayout is not 64-bit");
  }
  Output run(SymbolId entry, const Options &options) {
    // Reject future HIR capabilities before generating any LLVM instruction.
    for (uint64_t id = 1; id <= types.size(); ++id) {
      const auto &t = type(TypeId{id});
      if (t.kind == hir::TypeKind::Function) {
        if (t.throws_type)
          unsupported("typed throws is not implemented by native scalars");
        for (auto p : t.parameters)
          if (p.mode == hir::ParamMode::move)
            unsupported("move is not implemented by native scalars");
      } else if (t.kind != hir::TypeKind::Error)
        (void)lower_type(TypeId{id});
    }
    for (const auto &f : input.functions)
      functions.emplace(
          f.symbol,
          llvm::Function::Create(
              signature(f.signature), llvm::GlobalValue::InternalLinkage,
              "lang.fn." + std::to_string(f.symbol.value), module));
    auto chosen = functions.find(entry);
    if (chosen == functions.end())
      fail("entry SymbolId is not a verified function");
    TypeId selected;
    for (auto &f : input.functions)
      if (f.symbol == entry)
        selected = f.signature;
    const auto &sig = type(selected);
    const auto &result = type(sig.result);
    if (!sig.parameters.empty() ||
        !(result.kind == hir::TypeKind::Unit ||
          (result.kind == hir::TypeKind::SignedInt && result.bits == 32)))
      unsupported(
          "executable entry requires no parameters and i32 or Unit result");
    for (auto &f : input.functions)
      function(f);
    auto *main = llvm::Function::Create(
        llvm::FunctionType::get(builder.getInt32Ty(), {}, false),
        llvm::GlobalValue::ExternalLinkage, "main", module);
    builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", main));
    llvm::Value *value = builder.CreateCall(chosen->second);
    builder.CreateRet(result.kind == hir::TypeKind::Unit ? builder.getInt32(0)
                                                         : value);
    verify_llvm(module);
    Output out;
    out.native_ir = ir::dump(input);
    out.llvm_before_optimization = text_ir(module);
    if (options.optimization == 2) {
      llvm::LoopAnalysisManager loops;
      llvm::FunctionAnalysisManager functions;
      llvm::CGSCCAnalysisManager cgscc;
      llvm::ModuleAnalysisManager modules;
      llvm::PassBuilder passes(machine.get());
      passes.registerModuleAnalyses(modules);
      passes.registerCGSCCAnalyses(cgscc);
      passes.registerFunctionAnalyses(functions);
      passes.registerLoopAnalyses(loops);
      passes.crossRegisterProxies(loops, functions, cgscc, modules);
      auto pipeline =
          passes.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
      pipeline.run(module, modules);
    }
    verify_llvm(module);
    out.llvm_ir = text_ir(module);
    out.triple = module.getTargetTriple().str();
    out.data_layout = module.getDataLayoutStr();
    llvm::SmallVector<char, 0> object;
    llvm::raw_svector_ostream stream(object);
    llvm::legacy::PassManager codegen;
    if (machine->addPassesToEmitFile(codegen, stream, nullptr,
                                     llvm::CodeGenFileType::ObjectFile, false))
      throw Failure{"E_TOOLCHAIN_MISSING", "TargetMachine cannot emit object",
                    FailureKind::environment};
    codegen.run(module);
    out.object.assign(object.begin(), object.end());
    return out;
  }
};
} // namespace
PhaseResult<Output> emit(const ir::VerifiedModule &input, SymbolId entry,
                         const Options &options) {
  PhaseResult<Output> r;
  try {
    if (options.triple != "x86_64-unknown-linux-gnu" ||
        options.cpu != "generic" || !options.features.empty() ||
        (options.optimization != 0 && options.optimization != 2))
      unsupported("native scalars supports Linux x86_64, generic CPU, no extra "
                  "features, O0/O2 only");
    Emitter emitter(input.module(), options);
    r.value = emitter.run(entry, options);
    r.state = PhaseState::complete;
  } catch (const Failure &e) {
    r.failure = e.kind;
    r.diagnostics.push_back(
        {e.code, "codegen", "error", e.message, {}, {}, {}, {}, {}, {}});
  } catch (const std::exception &e) {
    r.diagnostics.push_back(
        {"E_IR_INVALID", "codegen", "error", e.what(), {}, {}, {}, {}, {}, {}});
  }
  return r;
}
PhaseResult<Output> emit(const hir::VerifiedHirModule &input, SymbolId entry,
                         const Options &options) {
  auto cfg = ir::lower(input);
  if (!cfg.success()) {
    PhaseResult<Output> r;
    r.state = cfg.state;
    r.failure = cfg.failure;
    r.diagnostics = std::move(cfg.diagnostics);
    return r;
  }
  return emit(*cfg.value, entry, options);
}
} // namespace lang::backend::native
