#include "../../hir/foreign.hpp"
#include "../../hir/ownership.hpp"
#include "../../ir/layout.hpp"
#include "lang/backend/native.hpp"
#include "c_ffi.hpp"
#include <cmath>
#include <functional>
#include <llvm/ADT/APFloat.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/MDBuilder.h>
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
  ir::detail::AggregateTypes aggregates;
  std::map<TypeId, llvm::Type *> layouts;
  llvm::LLVMContext context;
  llvm::Module module{"lang.native.scalars", context};
  // Keep guards and dangerous operations in their actual blocks at O0, even
  // for literal operands. ConstantFolder would create poison in dead arms.
  llvm::IRBuilder<llvm::NoFolder> builder{context};
  std::map<SymbolId, llvm::Function *> functions;
  std::map<SymbolId, TypeId> function_signatures;
  std::map<SymbolId, const hir::HirFunction *> hir_functions;
  std::map<uint32_t, llvm::StructType *> c_record_layouts;
  std::map<ValueId, SymbolId> function_value_symbols;
  // Function values that carry an environment (or a bound receiver) cannot be
  // used as C callbacks. Op::function records them so the C lowering rejects
  // the value instead of silently passing its thunk.
  std::set<ValueId> dynamic_function_values;
  std::map<SymbolId, llvm::Function *> function_thunks;
  std::map<std::string, llvm::StructType *> env_record_types;
  std::map<std::pair<std::string, bool>, llvm::Function *> env_record_ops;
  std::map<std::tuple<uint64_t, uint64_t, uint64_t>, llvm::Function *>
      bound_adapters;
  std::string out_c_adapter_source;
  std::map<std::pair<uint64_t, uint32_t>, llvm::Function *> callback_bridges;
  std::set<uint32_t> used_c_callables, used_c_resources;
  std::set<std::pair<uint64_t, uint32_t>> used_c_callbacks;
  std::unique_ptr<llvm::TargetMachine> machine;
  std::map<ValueId, llvm::Value *> values;
  std::map<ValueId, TypeId> value_types;
  std::vector<llvm::Value *> slots;
  std::optional<SymbolId> gc_entry_symbol;
  llvm::StructType *vec_type = llvm::StructType::get(
      context,
      {builder.getInt64Ty(),
       builder.getInt64Ty(),
       builder.getInt64Ty(),
       builder.getInt64Ty(),
       builder.getInt64Ty(),
       builder.getPtrTy(),
       builder.getPtrTy(),
       builder.getPtrTy(),
       builder.getInt64Ty(),
       // LangVec.weak_control: the private canonical Weak slot appended after
       // capacity, at byte offset 72. The runtime owns every construction and
       // detach; generated code only takes its address for downgrade.
       builder.getPtrTy()});
  // LangFn is exactly {code, env}. Every stored/indirect code pointer has the
  // env-first C ABI; a noncapturing value stores a cached thunk and a null env.
  llvm::StructType *fn_type = llvm::StructType::get(
      context, {builder.getPtrTy(), builder.getPtrTy()});
  // LangWeak is exactly {control, code}. The reviewed Weak source operations
  // project the canonical target slot below; the 16-byte value layout and its
  // value ops are unchanged.
  llvm::StructType *weak_type = llvm::StructType::get(
      context, {builder.getPtrTy(), builder.getPtrTy()});
  // LangHandle prefix {refs, kind, destroy, weak_control}. Handles stay opaque
  // pointers in lowered values; this layout only addresses the canonical Weak
  // slot at byte offset 24.
  llvm::StructType *handle_type = llvm::StructType::get(
      context, {builder.getInt64Ty(), builder.getInt64Ty(),
                builder.getPtrTy(), builder.getPtrTy()});
  // Source Slice[T] is an inline 24-byte {owner, start, length} descriptor.
  // The low-level runtime owns retain/pin/copy/drop; this is not a heap node.
  llvm::StructType *slice_type = llvm::StructType::get(
      context,
      {builder.getPtrTy(), builder.getInt64Ty(), builder.getInt64Ty()});
  hir::Type type(TypeId id) const { return *aggregates.types.find(id); }
  llvm::Type *lower_type(TypeId id) {
    if (auto found = layouts.find(id); found != layouts.end())
      return found->second;
    const auto t = type(id);
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
      return fn_type;
    case hir::TypeKind::Weak:
      return weak_type;
    case hir::TypeKind::Vec:
    case hir::TypeKind::ThreadHandle:
    case hir::TypeKind::MutexHandle:
    case hir::TypeKind::CondvarHandle:
    case hir::TypeKind::AtomicI64Handle:
    case hir::TypeKind::IpcHandle:
      return builder.getPtrTy();
    case hir::TypeKind::String: {
      auto *layout = llvm::StructType::get(
          context,
          {builder.getPtrTy(), builder.getInt64Ty(), builder.getInt64Ty(), builder.getPtrTy()});
      layouts.emplace(id, layout);
      return layout;
    }
    case hir::TypeKind::Slice:
      return slice_type;
    case hir::TypeKind::CForeignPointer:
    case hir::TypeKind::CForeignBuffer:
    case hir::TypeKind::CForeignRecord:
    case hir::TypeKind::CForeignResource:
    case hir::TypeKind::CForeignCallback:
    case hir::TypeKind::CForeignOpaque:
      return c_lower_type(id);
    case hir::TypeKind::NominalStruct:
    case hir::TypeKind::NominalEnum: {
      auto *layout = llvm::StructType::create(
          context, "value." + std::to_string(id.value));
      layouts.emplace(id, layout);
      std::vector<llvm::Type *> fields;
      if (t.kind == hir::TypeKind::NominalStruct) {
        for (auto f : aggregates.fields(id))
          fields.push_back(lower_type(f.type));
      } else {
        fields.push_back(builder.getInt32Ty());
        for (const auto &v : aggregates.declaration(id).variants) {
          std::vector<llvm::Type *> payload;
          for (auto f : aggregates.fields(id, v.symbol))
            payload.push_back(lower_type(f.type));
          fields.push_back(llvm::StructType::get(context, payload));
        }
      }
      layout->setBody(fields);
      return layout;
    }
    default:
      unsupported("native scalar type is not implemented");
    }
    return nullptr;
  }
  llvm::Type *c_ref_llvm_type(const modules::CTypeRef &ref) {
    using modules::CTypeRef;
    if (ref.kind == CTypeRef::Kind::scalar) {
      if (ref.scalar == "Unit" || ref.scalar == "bool") return builder.getInt8Ty();
      if (ref.scalar == "i8") return builder.getInt8Ty();
      if (ref.scalar == "u8") return builder.getInt8Ty();
      if (ref.scalar == "i16") return builder.getInt16Ty();
      if (ref.scalar == "u16") return builder.getInt16Ty();
      if (ref.scalar == "i32") return builder.getInt32Ty();
      if (ref.scalar == "u32") return builder.getInt32Ty();
      if (ref.scalar == "i64" || ref.scalar == "u64" ||
          ref.scalar == "isize" || ref.scalar == "usize")
        return builder.getInt64Ty();
      if (ref.scalar == "f32") return builder.getFloatTy();
      if (ref.scalar == "f64") return builder.getDoubleTy();
      unsupported("unknown C scalar storage type");
    }
    if (ref.kind == CTypeRef::Kind::pointer || ref.kind == CTypeRef::Kind::buffer)
      return ref.kind == CTypeRef::Kind::pointer
                 ? static_cast<llvm::Type *>(builder.getPtrTy())
                 : llvm::StructType::get(context,
                                         {builder.getPtrTy(), builder.getInt64Ty()});
    const auto &d = input.hir.module().resolved->project->c_type(ref.named);
    if (d.kind == modules::CDeclKind::record) {
      if (auto it = c_record_layouts.find(d.id); it != c_record_layouts.end())
        return it->second;
      auto *layout = llvm::StructType::create(context,
          "c.record." + std::to_string(d.id));
      c_record_layouts.emplace(d.id, layout);
      std::vector<llvm::Type *> fields;
      for (const auto &field : d.fields)
        fields.push_back(c_ref_llvm_type(field.type));
      layout->setBody(fields);
      return layout;
    }
    return builder.getPtrTy();
  }
  llvm::Type *c_lower_type(TypeId id) {
    const auto t = type(id);
    if (t.kind == hir::TypeKind::CForeignRecord) {
      modules::CTypeRef ref;
      ref.kind = modules::CTypeRef::Kind::named;
      ref.named = t.c_type;
      return c_ref_llvm_type(ref);
    }
    switch (t.kind) {
    case hir::TypeKind::Unit:
    case hir::TypeKind::Bool:
      return builder.getInt8Ty();
    case hir::TypeKind::SignedInt:
    case hir::TypeKind::UnsignedInt:
    case hir::TypeKind::Isize:
    case hir::TypeKind::Usize:
      return builder.getIntNTy(hir::bit_width(t, input.hir.config()));
    case hir::TypeKind::Float:
      return t.bits == 32 ? builder.getFloatTy() : builder.getDoubleTy();
    case hir::TypeKind::CForeignPointer:
    case hir::TypeKind::CForeignResource:
    case hir::TypeKind::CForeignCallback:
    case hir::TypeKind::CForeignOpaque:
      return builder.getPtrTy();
    case hir::TypeKind::CForeignBuffer:
      return llvm::StructType::get(context,
                                   {builder.getPtrTy(), builder.getInt64Ty()});
    default:
      unsupported("non-C type in C storage position");
    }
    return nullptr;
  }
  std::string type_key(TypeId id) const {
    const auto t = type(id);
    switch (t.kind) {
    case hir::TypeKind::Unit:
      return "Unit";
    case hir::TypeKind::Bool:
      return "bool";
    case hir::TypeKind::String:
      return "string";
    case hir::TypeKind::Isize:
      return "isize";
    case hir::TypeKind::Usize:
      return "usize";
    case hir::TypeKind::SignedInt:
      return "i" + std::to_string(t.bits);
    case hir::TypeKind::UnsignedInt:
      return "u" + std::to_string(t.bits);
    case hir::TypeKind::Float:
      return "f" + std::to_string(t.bits);
    case hir::TypeKind::NominalStruct:
    case hir::TypeKind::NominalEnum: {
      const auto &resolved = *input.hir.module().resolved;
      const auto &symbol = resolved.symbols.at(t.declaration.value);
      std::string key =
          resolved.project->modules.at(symbol.module.value).logical_path + "." +
          symbol.name;
      if (!t.type_args.empty()) {
        key += "[";
        for (size_t a = 0; a < t.type_args.size(); ++a) {
          if (a)
            key += ",";
          key += type_key(t.type_args[a]);
        }
        key += "]";
      }
      return key;
    }
    case hir::TypeKind::ThreadHandle: return "std/thread.Thread";
    case hir::TypeKind::MutexHandle: return "std/sync.Mutex";
    case hir::TypeKind::CondvarHandle: return "std/sync.Condvar";
    case hir::TypeKind::AtomicI64Handle: return "std/sync.AtomicI64";
    case hir::TypeKind::IpcHandle: return "std/ipc.Handle";
    case hir::TypeKind::Vec:
      return "Vec[" + type_key(t.type_args[0]) + "]";
    case hir::TypeKind::Slice:
      return "Slice[" + type_key(t.type_args[0]) + "]";
    case hir::TypeKind::Weak:
      return "Weak[" + type_key(t.type_args.at(0)) + "]";
    case hir::TypeKind::Function: {
      std::string key = "func(";
      for (size_t a = 0; a < t.parameters.size(); ++a) {
        if (a)
          key += ",";
        if (t.parameters[a].mode == hir::ParamMode::inout)
          key += "inout ";
        key += type_key(t.parameters[a].type);
      }
      key += ")->(" + type_key(t.result) + ")";
      if (t.throws_type)
        key += " throws(" + type_key(*t.throws_type) + ")";
      return key;
    }
    default:
      fail("non-concrete entry error type");
    }
    return {};
  }
  llvm::FunctionType *signature(TypeId id, bool env_first = false,
                               bool skip_first_parameter = false) {
    const auto &t = type(id);
    std::vector<llvm::Type *> parameters;
    if (env_first)
      parameters.push_back(builder.getPtrTy());
    for (size_t p = skip_first_parameter ? 1 : 0; p < t.parameters.size();
         ++p)
      parameters.push_back(t.parameters[p].mode == hir::ParamMode::inout
                               ? builder.getPtrTy()
                               : lower_type(t.parameters[p].type));
    if (t.throws_type) {
      if (type(t.result).kind != hir::TypeKind::Unit)
        parameters.push_back(builder.getPtrTy());
      parameters.push_back(builder.getPtrTy());
    }
    return llvm::FunctionType::get(t.throws_type ? builder.getInt1Ty()
                                                 : lower_type(t.result),
                                   parameters, false);
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
    if (left.kind == hir::TypeKind::String) return text_binary(i, a, b);
    if (left.kind == hir::TypeKind::Float) {
      // These operations round in their own LLVM float/double type. No
      // widening, fast-math flags, contraction, or integer zero guard.
      switch (i.binary) {
      case B::add: return builder.CreateFAdd(a, b);
      case B::subtract: return builder.CreateFSub(a, b);
      case B::multiply: return builder.CreateFMul(a, b);
      case B::divide: return builder.CreateFDiv(a, b);
      case B::equal: return builder.CreateFCmpOEQ(a, b);
      case B::not_equal: return builder.CreateFCmpUNE(a, b);
      case B::less: return builder.CreateFCmpOLT(a, b);
      case B::less_equal: return builder.CreateFCmpOLE(a, b);
      case B::greater: return builder.CreateFCmpOGT(a, b);
      case B::greater_equal: return builder.CreateFCmpOGE(a, b);
      default: fail("invalid verified floating operation");
      }
    }
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
    if (from.kind == hir::TypeKind::Float &&
        to.kind == hir::TypeKind::Float) {
      // Identity keeps the LLVM value unchanged. Binary32 -> binary64 uses
      // fpext and is exact, including subnormals. Binary64 -> binary32 uses
      // fptrunc, which is IEEE round-to-nearest ties-to-even under the
      // checked RNE/FTZ-off environment; finite overflow becomes a signed
      // infinity and no guard or Panic is involved.
      if (from.bits == to.bits)
        return v;
      if (from.bits == 32)
        return builder.CreateFPExt(v, lower_type(i.type));
      return builder.CreateFPTrunc(v, lower_type(i.type));
    }
    if (from.kind == hir::TypeKind::Float) {
      // Truncate into an i64 first only under bounds that every f32/f64 can
      // represent exactly; fptosi/fptoui are then never given a value whose
      // truncation is unrepresentable. NaN is unordered, so ULE/UGE both fire.
      const unsigned width = hir::bit_width(to, input.hir.config());
      auto *float_type = builder.getFloatTy();
      if (from.bits == 64)
        float_type = builder.getDoubleTy();
      auto limit = [&](long exponent) {
        return llvm::ConstantFP::get(float_type,
                                     std::ldexp(1.0, int(exponent)));
      };
      llvm::Value *bad = nullptr;
      if (signed_(to)) {
        auto *minus_two63 =
            llvm::ConstantFP::get(float_type, -std::ldexp(1.0, 63));
        bad = builder.CreateOr(builder.CreateFCmpULT(v, minus_two63),
                               builder.CreateFCmpUGE(v, limit(63)));
      } else {
        // -1.0 itself truncates to -1, which cannot be unsigned; values in
        // (-1,0) truncate to 0 and stay valid. 2^64 is exactly representable.
        auto *minus_one = llvm::ConstantFP::get(float_type, -1.0);
        bad = builder.CreateOr(builder.CreateFCmpULE(v, minus_one),
                               builder.CreateFCmpUGE(v, limit(64)));
      }
      guard(bad, 3, i.source);
      v = signed_(to) ? builder.CreateFPToSI(v, builder.getInt64Ty())
                      : builder.CreateFPToUI(v, builder.getInt64Ty());
      if (width < 64) {
        llvm::Value *narrow = nullptr;
        if (signed_(to)) {
          auto lo = llvm::APInt::getSignedMinValue(width).sext(64);
          auto hi = llvm::APInt::getSignedMaxValue(width).sext(64);
          narrow = builder.CreateOr(
              builder.CreateICmpSLT(v, llvm::ConstantInt::get(context, lo)),
              builder.CreateICmpSGT(v, llvm::ConstantInt::get(context, hi)));
        } else {
          auto hi = llvm::APInt::getMaxValue(width).zext(64);
          narrow = builder.CreateICmpUGT(v, llvm::ConstantInt::get(context, hi));
        }
        guard(narrow, 3, i.source);
      }
      return builder.CreateIntCast(v, lower_type(i.type), signed_(to));
    }
    if (to.kind == hir::TypeKind::Float) {
      // One direct conversion to the destination width keeps the source
      // integer exact; LLVM's default is round-to-nearest ties-to-even and no
      // fast-math or contraction flag is attached.
      auto *target = lower_type(i.type);
      return signed_(from) ? builder.CreateSIToFP(v, target)
                           : builder.CreateUIToFP(v, target);
    }
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
  llvm::MDNode *vec_header_tag = nullptr, *vec_data_tag = nullptr;
  // A Vec's descriptor and element storage never overlap. Different Vec
  // payloads may alias, so all payload accesses keep the same tag.
  void vec_memory(llvm::Instruction *instruction, bool header) {
    if (!vec_header_tag) {
      llvm::MDBuilder metadata(context);
      auto *root = metadata.createTBAARoot("lang.vec.storage");
      auto *descriptor = metadata.createTBAAScalarTypeNode("descriptor", root);
      auto *data = metadata.createTBAAScalarTypeNode("elements", root);
      vec_header_tag = metadata.createTBAAStructTagNode(descriptor, descriptor, 0);
      vec_data_tag = metadata.createTBAAStructTagNode(data, data, 0);
    }
    instruction->setMetadata(llvm::LLVMContext::MD_tbaa,
                             header ? vec_header_tag : vec_data_tag);
  }
  llvm::LoadInst *vec_field(llvm::Value *vec, unsigned field, llvm::Type *type) {
    auto *value = builder.CreateLoad(type, builder.CreateStructGEP(vec_type, vec, field));
    vec_memory(value, true);
    return value;
  }
#include "value_ops.inc"
#include "concurrency.inc"
#include "system_extra.inc"
#include "process_ops.inc"
#include "text_values.inc"
#include "weak_ops.inc"
#include "gc_ops.inc"
  std::string id_list(const std::vector<TypeId> &fields) const {
    std::string key;
    for (auto id : fields) {
      key += std::to_string(id.value);
      key.push_back(',');
    }
    return key;
  }
  // A nonempty Fn environment is exactly one row of an anonymous record held
  // in a LangVec. The record has no public TypeId, so its recursive copy/drop
  // helpers are synthesized from the field TypeIds in declaration order.
  llvm::StructType *env_record_type(const std::vector<TypeId> &fields) {
    const auto key = id_list(fields);
    if (auto found = env_record_types.find(key);
        found != env_record_types.end())
      return found->second;
    std::vector<llvm::Type *> elements;
    for (auto field : fields)
      elements.push_back(lower_type(field));
    auto *layout = llvm::StructType::get(context, elements);
    env_record_types.emplace(key, layout);
    return layout;
  }
  llvm::Function *env_record_op(const std::vector<TypeId> &fields,
                                bool copy) {
    const auto key = std::pair{id_list(fields), copy};
    if (auto found = env_record_ops.find(key); found != env_record_ops.end())
      return found->second;
    auto *layout = env_record_type(fields);
    auto *fn = llvm::Function::Create(
        llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()},
                                false),
        llvm::GlobalValue::InternalLinkage,
        std::string(copy ? "lang.env.copy." : "lang.env.drop.") +
            std::to_string(env_record_ops.size()),
        module);
    env_record_ops.emplace(key, fn);
    auto ip = builder.saveIP();
    builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", fn));
    auto *address = fn->getArg(0);
    for (size_t n = 0; n < fields.size(); ++n) {
      const size_t p = copy ? n : fields.size() - 1 - n;
      if (owning(fields[p]))
        builder.CreateCall(value_op(fields[p], copy),
                           {builder.CreateStructGEP(layout, address, p)});
    }
    builder.CreateRetVoid();
    builder.restoreIP(ip);
    return fn;
  }
  // Install already-owned child values as one Vec row. The children were
  // copied or moved by NativeIR; storing their bytes transfers ownership once.
  llvm::Value *make_environment(const std::vector<TypeId> &fields,
                                llvm::ArrayRef<llvm::Value *> children) {
    if (fields.size() != children.size())
      fail("function environment child count differs from schema");
    auto *layout = env_record_type(fields);
    auto *row = storage(layout);
    for (size_t n = 0; n < fields.size(); ++n)
      builder.CreateStore(children[n],
                          builder.CreateStructGEP(layout, row, n));
    const uint64_t stride = std::max<uint64_t>(
        1, module.getDataLayout().getTypeAllocSize(layout).getFixedValue());
    auto *env = runtime("lang_vec_new_v1", builder.getPtrTy(),
                        {builder.getInt64(1), builder.getInt64(stride),
                         env_record_op(fields, true),
                         env_record_op(fields, false)});
    auto *data = vec_field(env, 5, builder.getPtrTy());
    const uint64_t bytes =
        module.getDataLayout().getTypeAllocSize(layout).getFixedValue();
    if (bytes)
      vec_memory(builder.CreateMemCpy(data, llvm::MaybeAlign(1), row,
                                      llvm::MaybeAlign(1), bytes),
                 false);
    vec_memory(builder.CreateStore(builder.getInt64(1),
                                   builder.CreateStructGEP(vec_type, env, 2)),
               true);
    gc_register_fields(env, fields);
    return env;
  }
  // Cache one env-ignoring thunk per noncapturing function. Stored/indirect
  // code always has the env-first ABI, including throws outputs.
  llvm::Function *function_thunk(SymbolId symbol) {
    if (auto found = function_thunks.find(symbol);
        found != function_thunks.end())
      return found->second;
    auto signature_id = function_signatures.at(symbol);
    auto target = functions.at(symbol);
    auto *thunk = llvm::Function::Create(
        signature(signature_id, true), llvm::GlobalValue::InternalLinkage,
        "lang.thunk." + std::to_string(symbol.value), module);
    function_thunks.emplace(symbol, thunk);
    auto ip = builder.saveIP();
    builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", thunk));
    std::vector<llvm::Value *> actual;
    for (unsigned a = 1; a < thunk->arg_size(); ++a)
      actual.push_back(thunk->getArg(a));
    builder.CreateRet(builder.CreateCall(target, actual));
    builder.restoreIP(ip);
    return thunk;
  }
  llvm::Function *bound_adapter(const hir::FunctionValue &value) {
    if (!value.bound) fail("bound adapter without receiver schema");
    const auto &bound = *value.bound;
    if (bound.mode == hir::ParamMode::inout)
      fail("inout Self cannot be stored in a bound function value");
    auto signature_id = function_signatures.at(value.symbol);
    const auto &method = type(signature_id);
    if (method.parameters.empty() || method.parameters[0].type != bound.type ||
        method.parameters[0].mode != bound.mode)
      fail("bound receiver differs from the method Self parameter");
    // Both ordinary and move Self require an ordinary-copyable receiver. The
    // per-call copy for move Self must not consume the environment field.
    if (owning(bound.type)) (void)value_op(bound.type, true);
    const auto key = std::tuple<uint64_t, uint64_t, uint64_t>{
        value.symbol.value, bound.type.value,
        static_cast<uint64_t>(bound.mode)};
    if (auto found = bound_adapters.find(key); found != bound_adapters.end())
      return found->second;
    auto *adapter = llvm::Function::Create(
        signature(signature_id, true, true), llvm::GlobalValue::InternalLinkage,
        "lang.bound." + std::to_string(value.symbol.value) + "." +
            std::to_string(bound.type.value),
        module);
    bound_adapters.emplace(key, adapter);
    auto ip = builder.saveIP();
    builder.SetInsertPoint(
        llvm::BasicBlock::Create(context, "entry", adapter));
    auto *env = adapter->getArg(0);
    auto *row = runtime("lang_vec_at_v1", builder.getPtrTy(),
                        {env, builder.getInt64(0)});
    auto *layout = env_record_type({bound.type});
    auto *address = builder.CreateStructGEP(layout, row, 0);
    llvm::Value *receiver =
        builder.CreateLoad(lower_type(bound.type), address);
    if (bound.mode == hir::ParamMode::move && owning(bound.type)) {
      auto *copy = storage(lower_type(bound.type));
      builder.CreateStore(receiver, copy);
      builder.CreateCall(value_op(bound.type, true), {copy});
      receiver = builder.CreateLoad(lower_type(bound.type), copy);
    }
    std::vector<llvm::Value *> actual{receiver};
    for (unsigned a = 1; a < method.parameters.size(); ++a)
      actual.push_back(adapter->getArg(a));
    for (unsigned a = method.parameters.size(); a < adapter->arg_size(); ++a)
      actual.push_back(adapter->getArg(a));
    builder.CreateRet(builder.CreateCall(functions.at(value.symbol), actual));
    builder.restoreIP(ip);
    return adapter;
  }
  const hir::FunctionValue *function_value_schema(const ir::Instruction &i) {
    if (!i.origin || i.origin->value >= input.hir.module().nodes.size())
      return nullptr;
    const auto &node = input.hir.module().nodes[i.origin->value];
    if (auto *value = std::get_if<hir::FunctionValue>(&node.data))
      return value;
    if (auto *literal = std::get_if<hir::ConstantLiteral>(&node.data))
      return std::get_if<hir::FunctionValue>(&literal->value.data);
    return nullptr;
  }
  llvm::Value *function_value(const ir::Instruction &i,
                              llvm::ArrayRef<llvm::Value *> args) {
    const auto *schema = function_value_schema(i);
    const bool constant_origin =
        i.origin && i.origin->value < input.hir.module().nodes.size() &&
        std::holds_alternative<hir::ConstantLiteral>(
            input.hir.module().nodes[i.origin->value].data);
    const SymbolId symbol = schema ? schema->symbol : i.function;
    if (symbol != i.function)
      fail("function value symbol differs from its HIR schema");
    if (schema && schema->bound && !schema->captures.empty())
      fail("function value has both captures and a bound receiver");
    const bool dynamic =
        schema && (!schema->captures.empty() || schema->bound.has_value());
    if (dynamic && constant_origin)
      fail("constant FunctionValue cannot capture or bind a receiver");
    if (dynamic && args.empty())
      fail("capturing or bound function value has no environment inputs");
    llvm::Value *value = llvm::Constant::getNullValue(fn_type);
    if (dynamic) {
      std::vector<TypeId> fields;
      if (schema->bound) {
        if (args.size() != 1)
          fail("bound function value has the wrong receiver child count");
        if (value_types.at(i.inputs[0]) != schema->bound->type)
          fail("bound receiver child type differs from its schema");
        fields.push_back(schema->bound->type);
      } else {
        if (args.size() != schema->captures.size())
          fail("capture child count differs from the function schema");
        const auto hf = hir_functions.find(symbol);
        if (hf == hir_functions.end() ||
            hf->second->captures.size() != schema->captures.size())
          fail("capture schema differs from the lambda entry");
        for (size_t c = 0; c < schema->captures.size(); ++c) {
          if (value_types.at(i.inputs[c]) != schema->captures[c].type)
            fail("capture child type differs from the function schema");
          if (hf->second->captures[c].origin != schema->captures[c].origin ||
              hf->second->captures[c].type != schema->captures[c].type)
            fail("capture origin/type differs from the lambda entry");
          fields.push_back(schema->captures[c].type);
        }
      }
      if (schema->bound) {
        const auto hf = hir_functions.find(symbol);
        if (hf != hir_functions.end() && !hf->second->captures.empty())
          fail("bound method entry unexpectedly captures locals");
      }
      llvm::Function *code = schema->bound
                                 ? bound_adapter(*schema)
                                 : functions.at(symbol);
      value = builder.CreateInsertValue(value, code, {0});
      value = builder.CreateInsertValue(
          value, make_environment(fields, args), {1});
    } else {
      if (!args.empty()) fail("noncapturing function value has children");
      const auto found = hir_functions.find(symbol);
      if (found != hir_functions.end() && !found->second->captures.empty())
        fail("capturing function value is missing its HIR capture schema");
      value = llvm::ConstantStruct::get(
          fn_type, {function_thunk(symbol),
                    llvm::ConstantPointerNull::get(builder.getPtrTy())});
    }
    if (i.result) {
      function_value_symbols[*i.result] = symbol;
      if (dynamic) dynamic_function_values.insert(*i.result);
    }
    return value;
  }
  const hir::Call &call_origin(const std::optional<HirNodeId> &origin,
                               SourceSpan source) const {
    if (!origin || origin->value >= input.hir.module().nodes.size())
      fail("call instruction has no HIR origin");
    const auto &node = input.hir.module().nodes[origin->value];
    const auto *call = std::get_if<hir::Call>(&node.data);
    if (!call)
      fail("call instruction origin is not a HIR call");
    (void)source;
    return *call;
  }
  llvm::Value *indirect_call_operand(llvm::Value *callee,
                                     llvm::ArrayRef<llvm::Value *> arguments,
                                     TypeId signature_id, SourceSpan source) {
    auto *code = builder.CreateExtractValue(callee, {0});
    auto *env = builder.CreateExtractValue(callee, {1});
    std::vector<llvm::Value *> actual{env};
    actual.insert(actual.end(), arguments.begin(), arguments.end());
    return builder.CreateCall(signature(signature_id, true), code, actual);
  }
  void emit_vec_check(llvm::Value *vec, llvm::Value *index, SourceSpan source) {
    auto *length = vec_field(vec, 1, builder.getInt64Ty());
    auto *init = vec_field(vec, 2, builder.getInt64Ty());
    auto *bad = builder.CreateOr(builder.CreateICmpUGE(index, length),
                                 builder.CreateICmpUGE(index, init));
    guard(bad, 4, source);
  }
  llvm::Value *system_call(const ir::Instruction &i,
                           llvm::ArrayRef<llvm::Value *> args) {
    using hir::SystemOperation;
    if (!i.system) fail("system operation missing");
    if (auto *value = process_system_call(i, args)) return value;
    if (auto *value = extra_system_call(i, args)) return value;
    if (auto *value = weak_system_call(i, args)) return value;
    for (const auto &spec : hir::value_system_operations)
      if (spec.operation == *i.system) return value_system_call(i, args);
    auto call = [&](llvm::StringRef name, llvm::Type *result,
                    llvm::ArrayRef<llvm::Value *> actual) -> llvm::Value * {
      llvm::SmallVector<llvm::Type *, 8> types;
      for (auto *value : actual) types.push_back(value->getType());
      auto target = module.getOrInsertFunction(
          name, llvm::FunctionType::get(result, types, false));
      return builder.CreateCall(target, actual);
    };
    auto text_pointer = [&](unsigned index) {
      return builder.CreateExtractValue(args[index], {0});
    };
    auto text_length = [&](unsigned index) {
      return builder.CreateExtractValue(args[index], {1});
    };
    auto *i64 = builder.getInt64Ty();
    switch (*i.system) {
    case SystemOperation::ffi_buffer:
    case SystemOperation::ffi_buffer_mut: {
      llvm::Value *value = llvm::Constant::getNullValue(lower_type(i.type));
      value = builder.CreateInsertValue(value, args[0], {0});
      return builder.CreateInsertValue(value, args[1], {1});
    }
    case SystemOperation::ffi_buffer_len:
    case SystemOperation::ffi_buffer_len_mut:
      return builder.CreateExtractValue(args[0], {1});
    case SystemOperation::slice_view:
    case SystemOperation::slice_sub: {
      auto *out = storage(lower_type(i.type));
      auto *source_address =
          value_address(type(i.signature).parameters[0].type, args[0]);
      if (*i.system == SystemOperation::slice_view)
        runtime("lang_slice_new_v1", builder.getVoidTy(),
                {out, args[0], args[1], args[2]});
      else
        runtime("lang_slice_sub_v1", builder.getVoidTy(),
                {out, source_address, args[1], args[2]});
      return builder.CreateLoad(lower_type(i.type), out);
    }
    case SystemOperation::slice_len:
      return runtime("lang_slice_length_v1", builder.getInt64Ty(),
                     {value_address(type(i.signature).parameters[0].type, args[0])});
    case SystemOperation::slice_to_vec: {
      auto *owner = builder.CreateExtractValue(args[0], {0});
      auto *start = builder.CreateExtractValue(args[0], {1});
      auto *length = builder.CreateExtractValue(args[0], {2});
      auto *end = builder.CreateAdd(start, length);
      auto *result = runtime("lang_vec_slice_v1", builder.getPtrTy(),
                             {owner, start, end});
      gc_register_vec(result, type(i.type).type_args[0]);
      return result;
    }
    case SystemOperation::read:
      return call("lang_os_read_vec_v1", i64, {args[0], args[1], args[2]});
    case SystemOperation::write:
      return call("lang_os_write_vec_v1", i64,
                  {args[0], args[1], args[2], args[3]});
    case SystemOperation::write_text:
      return call("lang_os_write_text_v1", i64,
                  {args[0], text_pointer(1), text_length(1), args[2], args[3]});
    case SystemOperation::open:
      return call("lang_os_open_v1", i64,
                  {text_pointer(0), text_length(0), args[1], args[2]});
    case SystemOperation::close:
      return call("lang_os_close_v1", i64, args);
    case SystemOperation::seek:
      return call("lang_os_seek_v1", i64, args);
    case SystemOperation::unlink:
      return call("lang_os_unlink_v1", i64, {text_pointer(0), text_length(0)});
    case SystemOperation::mkdir:
      return call("lang_os_mkdir_v1", i64,
                  {text_pointer(0), text_length(0), args[1]});
    case SystemOperation::rmdir:
      return call("lang_os_rmdir_v1", i64, {text_pointer(0), text_length(0)});
    case SystemOperation::getpid:
      return call("lang_os_getpid_v1", i64, {});
    case SystemOperation::exit:
      call("lang_os_exit_v1", builder.getVoidTy(), args);
      return builder.getInt8(0); // Unit; the process does not return here.
    case SystemOperation::vec_length:
      return call("lang_vec_length_v1", i64, args);
    case SystemOperation::text_length:
      return text_length(0);
    case SystemOperation::thread_spawn:
      return thread_spawn(i, args);
    case SystemOperation::thread_error:
      return call("lang_thread_error_v1", i64, args);
    case SystemOperation::thread_wait:
      return call("lang_thread_wait_v1", i64, args);
    case SystemOperation::thread_wait_for:
      return call("lang_thread_wait_for_v1", i64, args);
    case SystemOperation::thread_result:
      return call("lang_thread_result_v1", i64, args);
    case SystemOperation::mutex_new:
      return call("lang_mutex_new_v1", builder.getPtrTy(), args);
    case SystemOperation::condvar_new:
      return call("lang_condvar_new_v1", builder.getPtrTy(), args);
    case SystemOperation::atomic_i64_new:
      return call("lang_atomic_i64_new_v1", builder.getPtrTy(), args);
    case SystemOperation::atomic_i64_load:
      return call("lang_atomic_i64_load_v1", i64, args);
    case SystemOperation::atomic_i64_fetch_add:
      return call("lang_atomic_i64_fetch_add_v1", i64, args);
    case SystemOperation::atomic_i64_compare_exchange:
      return call("lang_atomic_i64_compare_exchange_v1", builder.getInt1Ty(), args);
    case SystemOperation::condvar_wait_for:
      return call("lang_condvar_wait_for_v1", builder.getInt1Ty(), args);
    case SystemOperation::mutex_lock:
    case SystemOperation::mutex_unlock:
    case SystemOperation::condvar_wait:
    case SystemOperation::condvar_notify_one:
    case SystemOperation::condvar_notify_all:
    case SystemOperation::atomic_i64_store: {
      const char *name = nullptr;
      switch (*i.system) {
      case SystemOperation::mutex_lock: name = "lang_mutex_lock_v1"; break;
      case SystemOperation::mutex_unlock: name = "lang_mutex_unlock_v1"; break;
      case SystemOperation::condvar_wait: name = "lang_condvar_wait_v1"; break;
      case SystemOperation::condvar_notify_one: name = "lang_condvar_notify_one_v1"; break;
      case SystemOperation::condvar_notify_all: name = "lang_condvar_notify_all_v1"; break;
      case SystemOperation::atomic_i64_store: name = "lang_atomic_i64_store_v1"; break;
      default: fail("invalid synchronization operation");
      }
      call(name, builder.getVoidTy(), args);
      return builder.getInt8(0);
    }
    default: break;
    }
    fail("unknown system operation");
    return nullptr;
  }
  llvm::Value *to_c_storage(TypeId t, llvm::Value *value) {
    if (type(t).kind == hir::TypeKind::Bool && value->getType()->isIntegerTy(1))
      return builder.CreateZExt(value, builder.getInt8Ty());
    return value;
  }
  llvm::Value *from_c_storage(TypeId t, llvm::Value *value) {
    if (type(t).kind == hir::TypeKind::Bool)
      return builder.CreateTrunc(value, builder.getInt1Ty());
    return value;
  }
  llvm::Function *callback_bridge(SymbolId symbol, uint32_t c_type) {
    auto key = std::pair{symbol.value, c_type};
    if (auto it = callback_bridges.find(key); it != callback_bridges.end())
      return it->second;
    const auto &d = input.hir.module().resolved->project->c_type(c_type);
    auto *bridge = llvm::Function::Create(
        llvm::FunctionType::get(builder.getVoidTy(),
                                {builder.getPtrTy(), builder.getPtrTy()}, false),
        llvm::GlobalValue::ExternalLinkage,
        cffi::callback_bridge_symbol(symbol.value, c_type), module);
    bridge->addFnAttr(llvm::Attribute::NoUnwind);
    callback_bridges.emplace(key, bridge);
    auto ip = builder.saveIP();
    builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", bridge));
    auto *target = functions.at(symbol);
    std::vector<llvm::Value *> call_args;
    for (size_t a = 0; a < d.parameters.size(); ++a) {
      auto *slot = builder.CreateGEP(builder.getPtrTy(), bridge->getArg(1),
                                     builder.getInt64(a));
      auto *address = builder.CreateLoad(builder.getPtrTy(), slot);
      llvm::Value *value =
          builder.CreateLoad(c_ref_llvm_type(d.parameters[a].type), address);
      if (target->getFunctionType()->getParamType(a)->isIntegerTy(1) &&
          value->getType()->isIntegerTy(8))
        value = builder.CreateTrunc(value, builder.getInt1Ty());
      call_args.push_back(value);
    }
    auto *call = builder.CreateCall(target, call_args);
    const bool unit = d.result.kind == modules::CTypeRef::Kind::scalar &&
                      d.result.scalar == "Unit";
    if (!unit) {
      llvm::Value *result = call;
      auto *storage = c_ref_llvm_type(d.result);
      if (storage->isIntegerTy(8) && result->getType()->isIntegerTy(1))
        result = builder.CreateZExt(result, builder.getInt8Ty());
      builder.CreateStore(result, bridge->getArg(0));
    }
    builder.CreateRetVoid();
    builder.restoreIP(ip);
    return bridge;
  }
  llvm::Function *callback_trampoline(SymbolId symbol, uint32_t c_type) {
    used_c_callbacks.emplace(symbol.value, c_type);
    // The Clang-compiled C trampoline is the real C function pointer. The
    // declaration type here is irrelevant to the call ABI; only its address is
    // stored into the adapter argument slot.
    auto callee = module.getOrInsertFunction(
        cffi::callback_trampoline_symbol(symbol.value, c_type),
        llvm::FunctionType::get(builder.getVoidTy(), {}, false));
    return llvm::cast<llvm::Function>(callee.getCallee());
  }
  llvm::Value *c_foreign_call(const ir::Instruction &i,
                              llvm::ArrayRef<llvm::Value *> args) {
    if (!i.c_callable) fail("C call missing callable metadata");
    const auto id = *i.c_callable;
    if (id < 1 || id > input.hir.module().resolved->project->c_callables.size())
      fail("C callable index out of range");
    const auto &sig = type(i.signature);
    used_c_callables.insert(id);
    auto note_resource = [&](TypeId t) {
      const auto &value = type(t);
      if (value.kind == hir::TypeKind::CForeignResource)
        used_c_resources.insert(value.c_type);
    };
    note_resource(sig.result);
    for (auto p : sig.parameters) note_resource(p.type);
    llvm::AllocaInst *out = nullptr;
    if (type(sig.result).kind != hir::TypeKind::Unit)
      out = builder.CreateAlloca(c_lower_type(sig.result), nullptr, "c.out");
    std::vector<llvm::Value *> actual;
    for (size_t a = 0; a < sig.parameters.size(); ++a) {
      const auto parameter_type = sig.parameters[a].type;
      auto *slot = builder.CreateAlloca(c_lower_type(parameter_type), nullptr,
                                        "c.arg");
      if (type(parameter_type).kind == hir::TypeKind::CForeignCallback) {
        auto found = function_value_symbols.find(i.inputs[a]);
        if (found == function_value_symbols.end())
          fail("C callback argument has no function identity");
        if (dynamic_function_values.contains(i.inputs[a]))
          fail("C callback cannot be a capturing or bound function value");
        auto *trampoline = callback_trampoline(
            found->second, type(parameter_type).c_type);
        (void)callback_bridge(found->second, type(parameter_type).c_type);
        builder.CreateStore(trampoline, slot);
      } else {
        builder.CreateStore(to_c_storage(parameter_type, args[a]), slot);
      }
      actual.push_back(slot);
    }
    std::vector<llvm::Type *> parameter_types;
    parameter_types.push_back(builder.getPtrTy());
    for (size_t a = 0; a < sig.parameters.size(); ++a)
      parameter_types.push_back(builder.getPtrTy());
    auto adapter = module.getOrInsertFunction(
        cffi::adapter_symbol(id),
        llvm::FunctionType::get(builder.getVoidTy(), parameter_types, false));
    std::vector<llvm::Value *> call_args;
    call_args.push_back(out ? static_cast<llvm::Value *>(out)
                            : llvm::ConstantPointerNull::get(builder.getPtrTy()));
    call_args.insert(call_args.end(), actual.begin(), actual.end());
    // An external call can block or synchronously re-enter Woozy. All graph
    // writes (argument stores above) are complete and no runtime lock is held.
    auto *safe = gc_enter_safe();
    builder.CreateCall(adapter, call_args);
    gc_leave_safe(safe);
    if (!out) return builder.getInt8(0);
    return from_c_storage(sig.result,
                          builder.CreateLoad(c_lower_type(sig.result), out));
  }
  void instruction(const ir::Instruction &i) {
    std::vector<llvm::Value *> args;
    for (auto v : i.inputs)
      args.push_back(values.at(v));
    llvm::Value *v = nullptr;
    switch (i.op) {
    case ir::Op::copy:
      v = copy_value(i.type, args[0]); break;
    case ir::Op::borrow:
      v = args[0]; break;
    case ir::Op::move:
      v = builder.CreateLoad(lower_type(i.type), slots.at(i.slot.value));
      if (active.at(i.slot.value)) builder.CreateStore(builder.getFalse(), active.at(i.slot.value));
      break;
    case ir::Op::take:
      builder.CreateStore(builder.getFalse(), active.at(i.slot.value)); break;
    case ir::Op::destroy:
      destroy_slot(i.slot.value, current_function->slots.at(i.slot.value).type); break;
    case ir::Op::iteration_end:
      if (type(current_function->slots.at(i.slot.value).type).kind == hir::TypeKind::Vec)
        runtime("lang_vec_end_v1", builder.getVoidTy(),
                {builder.CreateLoad(builder.getPtrTy(), slots.at(i.slot.value))});
      // A Slice already owns an iteration pin for its whole lifetime; the
      // loop adds no second pin and therefore has nothing to end here.
      break;
    case ir::Op::iteration_begin: {
      auto collection_type = current_function->slots.at(i.slot.value).type;
      auto *collection = builder.CreateLoad(lower_type(collection_type), slots.at(i.slot.value));
      auto *length = type(collection_type).kind == hir::TypeKind::String
                         ? builder.CreateExtractValue(collection, {2})
                         : type(collection_type).kind == hir::TypeKind::Slice
                               ? runtime("lang_slice_length_v1", builder.getInt64Ty(),
                                         {slots.at(i.slot.value)})
                               : runtime("lang_vec_begin_v1", builder.getInt64Ty(), {collection});
      builder.CreateStore(length, iteration_lengths.at(i.slot.value));
      builder.CreateStore(builder.getInt64(0), iteration_indices.at(i.slot.value)); break;
    }
    case ir::Op::iteration_test:
      v = builder.CreateICmpULT(builder.CreateLoad(builder.getInt64Ty(), iteration_indices.at(i.slot.value)),
                               builder.CreateLoad(builder.getInt64Ty(), iteration_lengths.at(i.slot.value))); break;
    case ir::Op::iteration_next: {
      auto *index = iteration_indices.at(i.slot.value);
      builder.CreateStore(builder.CreateAdd(builder.CreateLoad(builder.getInt64Ty(), index), builder.getInt64(1)), index); break;
    }
    case ir::Op::iteration_read: {
      auto *index = builder.CreateLoad(builder.getInt64Ty(), iteration_indices.at(i.slot.value));
      if (type(current_function->slots.at(i.slot.value).type).kind == hir::TypeKind::String) {
        v = runtime("lang_text_at_v1", builder.getInt32Ty(), {slots.at(i.slot.value), index});
      } else if (type(current_function->slots.at(i.slot.value).type).kind == hir::TypeKind::Slice) {
        auto *address = runtime("lang_slice_at_v1", builder.getPtrTy(),
                                {slots.at(i.slot.value), index});
        v = copy_value(i.type, builder.CreateLoad(lower_type(i.type), address));
      } else {
        auto *vec = builder.CreateLoad(builder.getPtrTy(), slots.at(i.slot.value));
        auto *address = runtime("lang_vec_at_v1", builder.getPtrTy(), {vec, index});
        v = copy_value(i.type, builder.CreateLoad(lower_type(i.type), address));
      }
      break;
    }
    case ir::Op::vec_length: {
      v = type(value_types.at(i.inputs[0])).kind == hir::TypeKind::String
              ? builder.CreateExtractValue(args[0], {2})
              : vec_field(args[0], 1, builder.getInt64Ty());
      break;
    }
    case ir::Op::vec_init: {
      auto element = type(i.type).type_args[0];
      llvm::Value *copy = owning(element)
                              ? static_cast<llvm::Value *>(value_op(element, true))
                              : llvm::ConstantPointerNull::get(builder.getPtrTy());
      llvm::Value *drop = owning(element)
                              ? static_cast<llvm::Value *>(value_op(element, false))
                              : llvm::ConstantPointerNull::get(builder.getPtrTy());
      v = runtime("lang_vec_new_v1", builder.getPtrTy(),
                  {builder.getInt64(args.size()),
                   builder.getInt64(stride(element)), copy, drop});
      bool constants = !args.empty() && constant_vec_element(element);
      llvm::SmallVector<llvm::Constant *, 16> constant_args;
      for (auto *arg : args) {
        auto *constant = constant_vec_value(arg);
        if (!constant) {
          constants = false;
          break;
        }
        constant_args.push_back(constant);
      }
      if (constants) {
        constant_vec_init(v, element, constant_args);
        gc_register_vec(v, element);
        break;
      }
      // The constructor's storage is fresh and every argument has already
      // been evaluated, so copy each element directly and publish the
      // initialized prefix one element at a time. The scratch byte for Unit
      // and other zero-sized elements stays zero as before. Ownership is
      // already arranged by NativeIR; no extra retain or drop belongs here.
      auto *data = vec_field(v, 5, builder.getPtrTy());
      const uint64_t element_stride = stride(element);
      llvm::AllocaInst *scratch = nullptr;
      // Publish the initialized prefix as one graph-stable interval. The
      // arguments are already owned values and no copy/drop callback runs in
      // this loop. The Vec is not registered until the prefix is complete.
      auto *critical = gc_enter_critical();
      for (size_t n = 0; n < args.size(); ++n) {
        auto *address = element_storage(element, args[n], scratch);
        auto *target = builder.CreateGEP(
            builder.getInt8Ty(), data,
            builder.getInt64(static_cast<uint64_t>(n) * element_stride));
        vec_memory(builder.CreateMemCpy(target, llvm::MaybeAlign(1), address,
                                        llvm::MaybeAlign(1), element_stride),
                   false);
        vec_memory(builder.CreateStore(
                       builder.getInt64(n + 1),
                       builder.CreateStructGEP(vec_type, v, 2)),
                   true);
      }
      gc_leave_critical(critical);
      gc_register_vec(v, element);
      break;
    }
    case ir::Op::vec_check: {
      auto *index = checked_index(args[1], value_types.at(i.inputs[1]), i.source, 4);
      if (type(value_types.at(i.inputs[0])).kind == hir::TypeKind::Slice) {
        auto *address = value_address(value_types.at(i.inputs[0]), args[0]);
        auto *length = runtime("lang_slice_length_v1", builder.getInt64Ty(), {address});
        guard(builder.CreateICmpUGE(index, length), 4, i.source);
      } else emit_vec_check(args[0], index, i.source);
      break;
    }
    case ir::Op::vec_read: {
      auto *index = checked_index(args[1], value_types.at(i.inputs[1]), i.source, 4);
      auto receiver_type = value_types.at(i.inputs[0]);
      if (type(receiver_type).kind == hir::TypeKind::String) {
        guard(builder.CreateICmpUGE(index, builder.CreateExtractValue(args[0], {2})), 4, i.source);
        v = runtime("lang_text_at_v1", builder.getInt32Ty(), {value_address(receiver_type, args[0]), index});
        break;
      }
      auto element_type = type(receiver_type).type_args[0];
      if (type(receiver_type).kind == hir::TypeKind::Slice) {
        auto *slice_address = value_address(receiver_type, args[0]);
        auto *address = runtime("lang_slice_at_v1", builder.getPtrTy(),
                                {slice_address, index});
        auto *loaded = builder.CreateLoad(lower_type(element_type), address);
        v = owning(element_type) ? copy_value(i.type, loaded) : loaded;
      } else if (!owning(element_type)) {
        emit_vec_check(args[0], index, i.source);
        auto *data = vec_field(args[0], 5, builder.getPtrTy());
        auto *elem_addr = builder.CreateGEP(lower_type(element_type), data, index);
        auto *loaded = builder.CreateLoad(lower_type(element_type), elem_addr);
        vec_memory(loaded, false);
        v = loaded;
      } else {
        auto *address = runtime("lang_vec_at_v1", builder.getPtrTy(), {args[0], index});
        v = copy_value(i.type, builder.CreateLoad(lower_type(i.type), address));
      }
      break;
    }
    case ir::Op::vec_element_address: {
      // The preceding verified vec_check fixed the panic location. Reuse the
      // current Vec/Slice bounds-checking accessor for the address.
      auto *index = checked_index(args[1], value_types.at(i.inputs[1]), i.source, 4);
      auto receiver = value_types.at(i.inputs[0]);
      if (type(receiver).kind == hir::TypeKind::Slice) {
        auto *slice_address = value_address(receiver, args[0]);
        v = runtime("lang_slice_at_v1", builder.getPtrTy(), {slice_address, index});
      } else v = runtime("lang_vec_at_v1", builder.getPtrTy(), {args[0], index});
      break;
    }
    case ir::Op::vec_set: {
      auto *index = checked_index(args[1], value_types.at(i.inputs[1]), i.source, 4);
      auto receiver_type = value_types.at(i.inputs[0]);
      auto element_type = type(receiver_type).type_args[0];
      if (type(receiver_type).kind == hir::TypeKind::Slice) {
        auto *slice_address = value_address(receiver_type, args[0]);
        if (!owning(element_type)) {
          auto *target = runtime("lang_slice_at_v1", builder.getPtrTy(),
                                 {slice_address, index});
          builder.CreateStore(args[2], target);
        } else {
          // Translate the relative index to the owner's absolute slot. The
          // verified runtime setter drops the replaced element exactly once.
          auto *owner = builder.CreateExtractValue(args[0], {0});
          auto *start = builder.CreateExtractValue(args[0], {1});
          auto *absolute = builder.CreateAdd(start, index);
          auto *address = element_storage(value_types.at(i.inputs[2]), args[2]);
          runtime("lang_vec_set_v1", builder.getVoidTy(),
                  {owner, absolute, address});
        }
      } else if (!owning(element_type)) {
        emit_vec_check(args[0], index, i.source);
        auto *data = vec_field(args[0], 5, builder.getPtrTy());
        auto *elem_addr = builder.CreateGEP(lower_type(element_type), data, index);
        vec_memory(builder.CreateStore(args[2], elem_addr), false);
      } else {
        auto *address = element_storage(value_types.at(i.inputs[2]), args[2]);
        runtime("lang_vec_set_v1", builder.getVoidTy(), {args[0], index, address});
      }
      break;
    }
    case ir::Op::vec_slice: {
      size_t p = 1;
      auto *start = (i.bits & 1) ? checked_index(args[p], value_types.at(i.inputs[p]), i.source, 9)
                               : static_cast<llvm::Value *>(builder.getInt64(0));
      if (i.bits & 1) ++p;
      auto *end = (i.bits & 2)
                      ? checked_index(args[p], value_types.at(i.inputs[p]), i.source, 9)
                      : type(i.type).kind == hir::TypeKind::String
                            ? builder.CreateExtractValue(args[0], {2})
                            : type(i.type).kind == hir::TypeKind::Slice
                                  ? static_cast<llvm::Value *>(builder.getInt64(0))
                                  : runtime("lang_vec_length_v1", builder.getInt64Ty(), {args[0]});
      if (type(i.type).kind == hir::TypeKind::String) {
        guard(builder.CreateOr(builder.CreateICmpUGT(start, end),
                                builder.CreateICmpUGT(end, builder.CreateExtractValue(args[0], {2}))), 9, i.source);
        auto *out = storage(lower_type(i.type));
        runtime("lang_text_slice_v1", builder.getVoidTy(), {out, value_address(i.type, args[0]), start, end});
        v = builder.CreateLoad(lower_type(i.type), out);
      } else if (type(i.type).kind == hir::TypeKind::Slice) {
        auto *source_address = value_address(value_types.at(i.inputs[0]), args[0]);
        if (!(i.bits & 2))
          end = runtime("lang_slice_length_v1", builder.getInt64Ty(), {source_address});
        auto *out = storage(lower_type(i.type));
        runtime("lang_slice_sub_v1", builder.getVoidTy(),
                {out, source_address, start, end});
        v = builder.CreateLoad(lower_type(i.type), out);
      } else {
        v = runtime("lang_vec_slice_v1", builder.getPtrTy(), {args[0], start, end});
        gc_register_vec(v, type(i.type).type_args[0]);
      }
      break;
    }
    case ir::Op::begin_borrow:
    case ir::Op::ownership_begin:
    case ir::Op::ownership_end:
    case ir::Op::end_borrow:
    case ir::Op::save_output:
    case ir::Op::exit_begin:
    case ir::Op::cleanup_begin:
    case ir::Op::cleanup_end:
      break; // verified compile-time obligations; no runtime bookkeeping
    case ir::Op::string_literal: {
      // No added terminator. Even the empty value has static storage; the
      // logical value is the pointer plus the exact target-sized byte count.
      auto *bytes = llvm::ConstantDataArray::getString(
          context, llvm::StringRef(i.utf8.data(), i.utf8.size()), false);
      auto *storage = new llvm::GlobalVariable(
          module, bytes->getType(), true, llvm::GlobalValue::PrivateLinkage,
          bytes, "string.bytes");
      storage->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
      storage->setAlignment(llvm::Align(1));
      auto *layout = llvm::cast<llvm::StructType>(lower_type(i.type));
      auto *length = llvm::ConstantInt::get(
          llvm::cast<llvm::IntegerType>(layout->getElementType(1)),
          i.utf8.size());
      uint64_t runes = 0;
      for (unsigned char byte : i.utf8) if ((byte & 0xc0) != 0x80) ++runes;
      v = llvm::ConstantStruct::get(layout,
          {storage, length, builder.getInt64(runes), llvm::ConstantPointerNull::get(builder.getPtrTy())});
      break;
    }
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
      v = function_value(i, args);
      break;
    case ir::Op::load:
      v = builder.CreateLoad(lower_type(i.type), slots.at(i.slot.value));
      break;
    case ir::Op::address:
      v = slots.at(i.slot.value);
      break;
    case ir::Op::store: {
      const auto slot_type = current_function->slots.at(i.slot.value).type;
      if (owning(slot_type)) {
        auto *critical = gc_enter_critical();
        builder.CreateStore(args[0], slots.at(i.slot.value));
        gc_leave_critical(critical);
      } else {
        builder.CreateStore(args[0], slots.at(i.slot.value));
      }
      if (active.at(i.slot.value)) builder.CreateStore(builder.getTrue(), active.at(i.slot.value));
      break;
    }
    case ir::Op::struct_init:
    case ir::Op::enum_init: {
      v = llvm::Constant::getNullValue(lower_type(i.type));
      if (i.op == ir::Op::struct_init) {
        for (unsigned a = 0; a < args.size(); ++a)
          v = builder.CreateInsertValue(v, args[a], {a});
      } else {
        auto index =
            static_cast<unsigned>(aggregates.variant_index(i.type, i.variant));
        v = builder.CreateInsertValue(v, builder.getInt32(index), {0});
        for (unsigned a = 0; a < args.size(); ++a)
          v = builder.CreateInsertValue(v, args[a], {index + 1, a});
      }
      break;
    }
    case ir::Op::field_read:
    case ir::Op::field_address: {
      auto receiver = value_types.at(i.inputs[0]);
      auto index =
          static_cast<unsigned>(aggregates.field_index(receiver, i.field));
      if (i.op == ir::Op::field_read)
        v = builder.CreateExtractValue(args[0], {index});
      else
        v = builder.CreateStructGEP(lower_type(receiver), args[0], index);
      break;
    }
    case ir::Op::load_indirect:
      v = builder.CreateLoad(lower_type(i.type), args[0]);
      break;
    case ir::Op::replace_indirect: {
      const auto value_type = value_types.at(i.inputs[0]);
      if (owning(value_type)) {
        // Move the old owner aside, publish the complete new field in one
        // graph-stable interval, then drop the old owner outside it. A
        // reentrant destructor must never observe a half-replaced field.
        auto *old = storage(lower_type(value_type));
        builder.CreateStore(
            builder.CreateLoad(lower_type(value_type), args[0]), old);
        auto *critical = gc_enter_critical();
        builder.CreateStore(args[1], args[0]);
        gc_leave_critical(critical);
        builder.CreateCall(value_op(value_type, false), {old});
      } else {
        builder.CreateStore(args[1], args[0]);
      }
      break;
    }
    case ir::Op::store_indirect:
      if (owning(value_types.at(i.inputs[0]))) {
        auto *critical = gc_enter_critical();
        builder.CreateStore(args[1], args[0]);
        gc_leave_critical(critical);
      } else {
        builder.CreateStore(args[1], args[0]);
      }
      break;
    case ir::Op::enum_test: {
      auto index =
          aggregates.variant_index(value_types.at(i.inputs[0]), i.variant);
      v = builder.CreateICmpEQ(builder.CreateExtractValue(args[0], {0}),
                               builder.getInt32(index));
      break;
    }
    case ir::Op::enum_payload: {
      auto receiver = value_types.at(i.inputs[0]);
      auto variant =
          static_cast<unsigned>(aggregates.variant_index(receiver, i.variant));
      auto field = static_cast<unsigned>(
          aggregates.field_index(receiver, i.field, i.variant));
      v = builder.CreateExtractValue(args[0], {variant + 1, field});
      break;
    }
    case ir::Op::unary:
      // fneg flips the sign bit, including +/-0; subtraction from +0 does not.
      v = i.unary == hir::UnaryOp::negate
              ? (type(i.type).kind == hir::TypeKind::Float
                     ? builder.CreateFNeg(args[0])
                     : builder.CreateNeg(args[0]))
              : builder.CreateNot(args[0]);
      break;
    case ir::Op::binary:
      v = binary(i, args[0], args[1]);
      break;
    case ir::Op::convert:
      v = convert(i, args[0]);
      break;
    case ir::Op::system_call:
      v = system_call(i, args);
      break;
    case ir::Op::foreign_call:
      v = c_foreign_call(i, args);
      break;
    case ir::Op::c_field_read: {
      v = from_c_storage(i.type, builder.CreateExtractValue(args[0], {i.c_field}));
      break;
    }
    case ir::Op::c_ptr_read:
    case ir::Op::c_buffer_read: {
      const auto receiver = type(value_types.at(i.inputs[0]));
      const auto element = receiver.type_args.at(0);
      const auto index_type = type(value_types.at(i.inputs[1]));
      auto *index = builder.CreateIntCast(args[1], builder.getInt64Ty(),
                                          signed_(index_type));
      llvm::Value *address = nullptr;
      if (i.op == ir::Op::c_ptr_read) {
        address = builder.CreateGEP(c_lower_type(element), args[0], index);
      } else {
        auto *data = builder.CreateExtractValue(args[0], {0});
        auto *length = builder.CreateExtractValue(args[0], {1});
        guard(builder.CreateICmpUGE(index, length), 4, i.source);
        address = builder.CreateGEP(c_lower_type(element), data, index);
      }
      v = from_c_storage(element,
                         builder.CreateLoad(c_lower_type(element), address));
      break;
    }
    case ir::Op::c_ptr_write:
    case ir::Op::c_buffer_write: {
      const auto receiver = type(value_types.at(i.inputs[0]));
      const auto element = receiver.type_args.at(0);
      const auto index_type = type(value_types.at(i.inputs[1]));
      auto *index = builder.CreateIntCast(args[1], builder.getInt64Ty(),
                                          signed_(index_type));
      llvm::Value *address = nullptr;
      if (i.op == ir::Op::c_ptr_write) {
        address = builder.CreateGEP(c_lower_type(element), args[0], index);
      } else {
        auto *data = builder.CreateExtractValue(args[0], {0});
        auto *length = builder.CreateExtractValue(args[0], {1});
        guard(builder.CreateICmpUGE(index, length), 4, i.source);
        address = builder.CreateGEP(c_lower_type(element), data, index);
      }
      builder.CreateStore(to_c_storage(element, args[2]), address);
      break;
    }
    case ir::Op::call: {
      const auto &call = call_origin(i.origin, i.source);
      std::vector<llvm::Value *> actual(args.begin() + 1, args.end());
      if (call.kind == hir::CalleeKind::direct && call.direct_symbol) {
        auto target = functions.at(*call.direct_symbol);
        const auto hf = hir_functions.find(*call.direct_symbol);
        if (hf != hir_functions.end() && !hf->second->captures.empty())
          fail("direct call targets a capturing lambda entry");
        v = builder.CreateCall(target, actual);
      } else if (call.kind == hir::CalleeKind::function_value) {
        v = indirect_call_operand(args[0], actual, i.signature, i.source);
      } else {
        fail("ordinary call origin is neither direct nor function_value");
      }
      break;
    }
    }
    if (i.result) {
      values.emplace(*i.result, v);
      value_types.emplace(*i.result, i.type);
    }
  }
  std::map<size_t, llvm::Value *> iteration_lengths, iteration_indices;
  const ir::Function *current_function = nullptr;
  void function(const ir::Function &f) {
    current_function = &f;
    active.clear(); iteration_lengths.clear(); iteration_indices.clear();
    values.clear();
    value_types.clear();
    function_value_symbols.clear();
    dynamic_function_values.clear();
    slots.clear();
    auto *fn = functions.at(f.symbol);
    const auto sig = type(f.signature);
    const auto hf = hir_functions.find(f.symbol);
    const bool capturing =
        hf != hir_functions.end() && !hf->second->captures.empty();
    const unsigned parameter_offset = capturing ? 1 : 0;
    std::vector<llvm::BasicBlock *> blocks;
    for (size_t b = 0; b < f.blocks.size(); ++b)
      blocks.push_back(
          llvm::BasicBlock::Create(context, "b" + std::to_string(b), fn));
    const auto [poll_headers, loop_exits] = gc_loop_safepoints(f);
    const bool gc_entry = gc_entry_symbol && *gc_entry_symbol == f.symbol;
    builder.SetInsertPoint(blocks[0]);
    for (const auto &slot : f.slots) {
      llvm::Value *storage;
      if (slot.inout)
        storage = fn->getArg(*slot.parameter + parameter_offset);
      else
        storage = builder.CreateAlloca(lower_type(slot.type));
      slots.push_back(storage);
      llvm::Value *flag = nullptr;
      if (slot.owner) {
        flag = builder.CreateAlloca(builder.getInt1Ty());
        builder.CreateStore(builder.getInt1(slot.owning_parameter), flag);
      }
      active.push_back(flag);
    }
    // A lambda entry receives borrowed capture slots from the single env row.
    // Loading a field into its slot does not copy or take ownership, and the
    // slot is not dropped when the lambda exits.
    if (capturing) {
      std::vector<TypeId> fields;
      for (const auto &capture : hf->second->captures)
        fields.push_back(capture.type);
      auto *layout = env_record_type(fields);
      auto *row = runtime("lang_vec_at_v1", builder.getPtrTy(),
                          {fn->getArg(0), builder.getInt64(0)});
      for (size_t c = 0; c < hf->second->captures.size(); ++c) {
        const auto &capture = hf->second->captures[c];
        std::optional<size_t> target;
        for (size_t s = 0; s < f.slots.size(); ++s)
          if (f.slots[s].binding == capture.local) {
            target = s;
            break;
          }
        if (!target || f.slots[*target].type != capture.type)
          fail("capture local has no matching initialized slot");
        if (f.slots[*target].parameter || f.slots[*target].inout ||
            f.slots[*target].owner)
          fail("capture local is marked as an owning parameter or local");
        auto *value = builder.CreateLoad(
            lower_type(capture.type),
            builder.CreateStructGEP(layout, row, c));
        builder.CreateStore(value, slots[*target]);
      }
    }
    for (size_t s = 0; s < f.slots.size(); ++s) {
      const auto &slot = f.slots[s];
      if (slot.parameter && !slot.inout)
        builder.CreateStore(fn->getArg(*slot.parameter + parameter_offset),
                            slots[s]);
    }
    for (const auto &b : f.blocks) for (const auto &i : b.instructions)
      if (i.op == ir::Op::iteration_begin) {
        iteration_lengths.emplace(i.slot.value, builder.CreateAlloca(builder.getInt64Ty()));
        iteration_indices.emplace(i.slot.value, builder.CreateAlloca(builder.getInt64Ty()));
      }
    struct TryStorage {
      llvm::Value *value = nullptr, *error = nullptr;
    };
    std::map<size_t, TryStorage> outputs;
    std::map<size_t, llvm::Value *> incoming;
    for (size_t b = 0; b < f.blocks.size(); ++b) {
      const auto &t = f.blocks[b].terminator;
      if (t.kind != ir::Exit::try_call)
        continue;
      const auto cs = type(t.signature);
      TryStorage storage;
      if (type(cs.result).kind != hir::TypeKind::Unit)
        storage.value =
            builder.CreateAlloca(lower_type(cs.result), nullptr, "try.value");
      storage.error = builder.CreateAlloca(lower_type(*cs.throws_type), nullptr,
                                           "try.error");
      outputs.emplace(b, storage);
      incoming.emplace(t.targets[0].value, storage.value);
      incoming.emplace(t.targets[1].value, storage.error);
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
      for (auto p : f.blocks[b].parameters) {
        auto *address = incoming.at(b);
        values.emplace(p.value,
                       address ? static_cast<llvm::Value *>(builder.CreateLoad(
                                     lower_type(p.type), address))
                               : llvm::ConstantInt::get(lower_type(p.type), 0));
        value_types.emplace(p.value, p.type);
      }
      // Stable safepoint: function entry, loop headers and loop exits. The runtime
      // controller checks a pending world request and then graph pressure;
      // with neither pending it performs only atomic loads/branches.
      if (b == 0 || poll_headers.contains(b)) gc_poll();
      if (gc_entry && loop_exits.contains(b)) gc_collect_loop_exit();
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
      case ir::Exit::try_call: {
        llvm::SmallVector<llvm::Value *, 8> actual;
        for (size_t a = 1; a < t.inputs.size(); ++a)
          actual.push_back(values.at(t.inputs[a]));
        const auto &storage = outputs.at(b);
        if (storage.value)
          actual.push_back(storage.value);
        actual.push_back(storage.error);
        const auto &call = call_origin(t.origin, t.source);
        if (call.kind == hir::CalleeKind::direct && call.direct_symbol) {
          auto target = functions.at(*call.direct_symbol);
          const auto hf = hir_functions.find(*call.direct_symbol);
          if (hf != hir_functions.end() && !hf->second->captures.empty())
            fail("direct throwing call targets a capturing lambda entry");
          auto *success = builder.CreateCall(target, actual);
          builder.CreateCondBr(success, blocks[t.targets[0].value],
                               blocks[t.targets[1].value]);
        } else if (call.kind == hir::CalleeKind::function_value) {
          std::vector<llvm::Value *> method_args(actual.begin(), actual.end());
          auto *callee = values.at(t.inputs[0]);
          auto *code = builder.CreateExtractValue(callee, {0});
          auto *env = builder.CreateExtractValue(callee, {1});
          method_args.insert(method_args.begin(), env);
          auto *success = builder.CreateCall(signature(t.signature, true),
                                             code, method_args);
          builder.CreateCondBr(success, blocks[t.targets[0].value],
                               blocks[t.targets[1].value]);
        } else {
          fail("throwing call origin is neither direct nor function_value");
        }
        break;
      }
      case ir::Exit::return_:
        if (sig.throws_type) {
          if (type(sig.result).kind != hir::TypeKind::Unit)
            builder.CreateStore(values.at(t.inputs[0]),
                                fn->getArg(sig.parameters.size() +
                                           parameter_offset));
          builder.CreateRet(builder.getTrue());
        } else
          builder.CreateRet(values.at(t.inputs[0]));
        break;
      case ir::Exit::error_return:
        builder.CreateStore(values.at(t.inputs[0]),
                            fn->getArg(fn->arg_size() - 1));
        builder.CreateRet(builder.getFalse());
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
      : input(m), aggregates(m.hir.module()) {
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
    llvm::TargetOptions target_options;
    target_options.AllowFPOpFusion = llvm::FPOpFusion::Strict;
    machine.reset(target->createTargetMachine(
        triple, options.cpu, options.features, target_options,
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
    // Only concrete CFG functions and their used types reach LLVM. The shared
    // context also contains legal, non-executable generic declarations.
    for (const auto &h : input.hir.module().functions)
      hir_functions.emplace(h.symbol, &h);
    for (const auto &f : input.functions) {
      function_signatures.emplace(f.symbol, f.signature);
      const auto hf = hir_functions.find(f.symbol);
      const bool capturing =
          hf != hir_functions.end() && !hf->second->captures.empty();
      functions.emplace(
          f.symbol,
          llvm::Function::Create(
              signature(f.signature, capturing),
              llvm::GlobalValue::InternalLinkage,
              "lang.fn." + std::to_string(f.symbol.value), module));
    }
    auto chosen = functions.find(entry);
    if (chosen == functions.end())
      fail("entry SymbolId is not a verified function");
    {
      const auto hf = hir_functions.find(entry);
      if (hf != hir_functions.end() && !hf->second->captures.empty())
        fail("capturing lambda cannot be the executable entry");
    }
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
    gc_entry_symbol = entry;
    for (auto &f : input.functions)
      function(f);
    auto *main = llvm::Function::Create(
        llvm::FunctionType::get(builder.getInt32Ty(), {}, false),
        llvm::GlobalValue::ExternalLinkage, "main", module);
    builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", main));
    // Outermost lifecycle wrapper: enable the integration guards before any
    // source code runs, and finish only after all wrapper cleanup. Panic and
    // an explicit source process exit do not reach the finish path.
    auto init = module.getOrInsertFunction(
        "lang_gc_program_init_v1",
        llvm::FunctionType::get(builder.getInt64Ty(), {}, false));
    auto finish = module.getOrInsertFunction(
        "lang_gc_program_finish_v1",
        llvm::FunctionType::get(builder.getInt64Ty(), {}, false));
    auto *init_status = builder.CreateCall(init, {});
    auto *init_failed =
        llvm::BasicBlock::Create(context, "main.gc.init_failed", main);
    auto *run = llvm::BasicBlock::Create(context, "main.gc.run", main);
    builder.CreateCondBr(builder.CreateICmpNE(init_status, builder.getInt64(0)),
                         init_failed, run);
    builder.SetInsertPoint(init_failed);
    {
      auto report = module.getOrInsertFunction(
          "lang_native_typed_error_v1",
          llvm::FunctionType::get(builder.getVoidTy(),
                                  {builder.getPtrTy(), builder.getInt64Ty()},
                                  false));
      constexpr llvm::StringLiteral key = "lang-gc-program-init";
      builder.CreateCall(report, {builder.CreateGlobalString(key),
                                  builder.getInt64(key.size())});
      builder.CreateRet(builder.getInt32(1));
    }
    builder.SetInsertPoint(run);
    if (!sig.throws_type) {
      llvm::Value *value = builder.CreateCall(chosen->second);
      llvm::Value *result_value =
          result.kind == hir::TypeKind::Unit ? builder.getInt32(0) : value;
      builder.CreateCall(finish, {});
      builder.CreateRet(result_value);
    } else {
      llvm::SmallVector<llvm::Value *, 2> outputs;
      llvm::Value *value = nullptr;
      if (result.kind != hir::TypeKind::Unit) {
        value = builder.CreateAlloca(lower_type(sig.result));
        outputs.push_back(value);
      }
      outputs.push_back(builder.CreateAlloca(lower_type(*sig.throws_type)));
      auto *success = builder.CreateCall(chosen->second, outputs);
      auto *ok = llvm::BasicBlock::Create(context, "main.success", main);
      auto *error = llvm::BasicBlock::Create(context, "main.error", main);
      builder.CreateCondBr(success, ok, error);
      builder.SetInsertPoint(ok);
      llvm::Value *result_value =
          value ? static_cast<llvm::Value *>(builder.CreateLoad(
                      lower_type(sig.result), value))
                : builder.getInt32(0);
      builder.CreateCall(finish, {});
      builder.CreateRet(result_value);
      builder.SetInsertPoint(error);
      auto report = module.getOrInsertFunction(
          "lang_native_typed_error_v1",
          llvm::FunctionType::get(builder.getVoidTy(),
                                  {builder.getPtrTy(), builder.getInt64Ty()},
                                  false));
      const auto key = type_key(*sig.throws_type);
      builder.CreateCall(report, {builder.CreateGlobalString(key),
                                  builder.getInt64(key.size())});
      if (owning(*sig.throws_type)) builder.CreateCall(value_op(*sig.throws_type, false), {outputs.back()});
      builder.CreateCall(finish, {});
      builder.CreateRet(builder.getInt32(1));
    }
    if (!used_c_callables.empty())
      out_c_adapter_source = cffi::adapter_source(
          *input.hir.module().resolved->project, used_c_callables,
          used_c_resources, used_c_callbacks);
    verify_llvm(module);
    Output out;
    out.native_ir = ir::dump(input);
    out.c_adapter_source = std::move(out_c_adapter_source);
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
  auto capabilities =
      hir::check_backend_capabilities(input.module().hir.module());
  if (!capabilities.success()) {
    r.state = capabilities.state;
    r.failure = capabilities.failure;
    r.diagnostics = std::move(capabilities.diagnostics);
    return r;
  }
  try {
    if (input.module().hir.config().backend != hir::Backend::native ||
        input.module().hir.config().word_bits != 64)
      unsupported("native scalars require native/64 HIR");
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
    r.failure = FailureKind::internal;
    r.diagnostics.push_back(
        {"E_INTERNAL_ERROR", "codegen", "error", e.what(), {}, {}, {}, {}, {}, {}});
  }
  return r;
}
PhaseResult<Output> emit(const hir::VerifiedHirModule &input, SymbolId entry,
                         const Options &options) {
  auto capabilities = hir::check_backend_capabilities(input.module());
  if (!capabilities.success()) {
    PhaseResult<Output> r;
    r.state = capabilities.state;
    r.failure = capabilities.failure;
    r.diagnostics = std::move(capabilities.diagnostics);
    return r;
  }
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
