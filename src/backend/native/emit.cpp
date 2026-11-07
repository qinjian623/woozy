#include "../../ir/layout.hpp"
#include "lang/backend/native.hpp"
#include <cmath>
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
  ir::detail::AggregateTypes aggregates;
  std::map<TypeId, llvm::Type *> layouts;
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
  llvm::StructType *vec_type = llvm::StructType::get(
      context,
      {builder.getInt64Ty(),
       builder.getInt64Ty(),
       builder.getInt64Ty(),
       builder.getInt64Ty(),
       builder.getInt64Ty(),
       builder.getPtrTy(),
       builder.getPtrTy(),
       builder.getPtrTy()});
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
    case hir::TypeKind::Vec:
      return builder.getPtrTy();
    case hir::TypeKind::String: {
      auto *layout = llvm::StructType::get(
          context,
          {builder.getPtrTy(), module.getDataLayout().getIntPtrType(context)});
      layouts.emplace(id, layout);
      return layout;
    }
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
    case hir::TypeKind::Vec:
      return "Vec[" + type_key(t.type_args[0]) + "]";
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
  llvm::FunctionType *signature(TypeId id) {
    const auto &t = type(id);
    std::vector<llvm::Type *> parameters;
    for (auto p : t.parameters)
      parameters.push_back(p.mode == hir::ParamMode::inout
                               ? builder.getPtrTy()
                               : lower_type(p.type));
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
#include "value_ops.inc"
  void emit_vec_check(llvm::Value *vec, llvm::Value *index, SourceSpan source) {
    auto *len_ptr = builder.CreateStructGEP(vec_type, vec, 1);
    auto *init_ptr = builder.CreateStructGEP(vec_type, vec, 2);
    auto *length = builder.CreateLoad(builder.getInt64Ty(), len_ptr);
    auto *init = builder.CreateLoad(builder.getInt64Ty(), init_ptr);
    auto *bad = builder.CreateOr(builder.CreateICmpUGE(index, length),
                                 builder.CreateICmpUGE(index, init));
    guard(bad, 4, source);
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
      runtime("lang_vec_end_v1", builder.getVoidTy(),
              {builder.CreateLoad(builder.getPtrTy(), slots.at(i.slot.value))}); break;
    case ir::Op::iteration_begin: {
      auto *vec = builder.CreateLoad(builder.getPtrTy(), slots.at(i.slot.value));
      auto *length = runtime("lang_vec_begin_v1", builder.getInt64Ty(), {vec});
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
      auto *vec = builder.CreateLoad(builder.getPtrTy(), slots.at(i.slot.value));
      auto *index = builder.CreateLoad(builder.getInt64Ty(), iteration_indices.at(i.slot.value));
      auto *address = runtime("lang_vec_at_v1", builder.getPtrTy(), {vec, index});
      v = copy_value(i.type, builder.CreateLoad(lower_type(i.type), address)); break;
    }
    case ir::Op::vec_length: {
      auto *len_ptr = builder.CreateStructGEP(vec_type, args[0], 1);
      v = builder.CreateLoad(builder.getInt64Ty(), len_ptr);
      break;
    }
    case ir::Op::vec_init: {
      auto element = type(i.type).type_args[0];
      v = runtime("lang_vec_new_v1", builder.getPtrTy(),
                  {builder.getInt64(args.size()),
                   builder.getInt64(stride(element)),
                   value_op(element, true), value_op(element, false)});
      for (auto *arg : args) {
        auto *address = element_storage(element, arg);
        runtime("lang_vec_init_v1", builder.getVoidTy(), {v, address});
      }
      break;
    }
    case ir::Op::vec_check: {
      auto *index = checked_index(args[1], value_types.at(i.inputs[1]), i.source, 4);
      emit_vec_check(args[0], index, i.source);
      break;
    }
    case ir::Op::vec_read: {
      auto *index = checked_index(args[1], value_types.at(i.inputs[1]), i.source, 4);
      auto element_type = type(value_types.at(i.inputs[0])).type_args[0];
      if (!owning(element_type)) {
        emit_vec_check(args[0], index, i.source);
        auto *data_ptr = builder.CreateStructGEP(vec_type, args[0], 5);
        auto *data = builder.CreateLoad(builder.getPtrTy(), data_ptr);
        auto *elem_addr = builder.CreateGEP(lower_type(element_type), data, index);
        v = builder.CreateLoad(lower_type(element_type), elem_addr);
      } else {
        auto *address = runtime("lang_vec_at_v1", builder.getPtrTy(), {args[0], index});
        v = copy_value(i.type, builder.CreateLoad(lower_type(i.type), address));
      }
      break;
    }
    case ir::Op::vec_set: {
      auto *index = checked_index(args[1], value_types.at(i.inputs[1]), i.source, 4);
      auto element_type = type(value_types.at(i.inputs[0])).type_args[0];
      if (!owning(element_type)) {
        emit_vec_check(args[0], index, i.source);
        auto *data_ptr = builder.CreateStructGEP(vec_type, args[0], 5);
        auto *data = builder.CreateLoad(builder.getPtrTy(), data_ptr);
        auto *elem_addr = builder.CreateGEP(lower_type(element_type), data, index);
        builder.CreateStore(args[2], elem_addr);
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
      auto *end = (i.bits & 2) ? checked_index(args[p], value_types.at(i.inputs[p]), i.source, 9)
                             : runtime("lang_vec_length_v1", builder.getInt64Ty(), {args[0]});
      v = runtime("lang_vec_slice_v1", builder.getPtrTy(), {args[0], start, end}); break;
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
      v = llvm::ConstantStruct::get(layout, {storage, length});
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
      if (active.at(i.slot.value)) builder.CreateStore(builder.getTrue(), active.at(i.slot.value));
      break;
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
    case ir::Op::replace_indirect:
      builder.CreateCall(value_op(value_types.at(i.inputs[0]), false), {args[0]});
      builder.CreateStore(args[1], args[0]);
      break;
    case ir::Op::store_indirect:
      builder.CreateStore(args[1], args[0]);
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
  std::map<size_t, llvm::Value *> iteration_lengths, iteration_indices;
  const ir::Function *current_function = nullptr;
  void function(const ir::Function &f) {
    current_function = &f;
    active.clear(); iteration_lengths.clear(); iteration_indices.clear();
    values.clear();
    value_types.clear();
    slots.clear();
    auto *fn = functions.at(f.symbol);
    const auto sig = type(f.signature);
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
      llvm::Value *flag = nullptr;
      if (slot.owner) {
        flag = builder.CreateAlloca(builder.getInt1Ty());
        builder.CreateStore(builder.getInt1(slot.owning_parameter), flag);
      }
      active.push_back(flag);
    }
    for (size_t s = 0; s < f.slots.size(); ++s) {
      const auto &slot = f.slots[s];
      if (slot.parameter && !slot.inout)
        builder.CreateStore(fn->getArg(*slot.parameter), slots[s]);
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
        auto *success = builder.CreateCall(signature(t.signature),
                                           values.at(t.inputs[0]), actual);
        builder.CreateCondBr(success, blocks[t.targets[0].value],
                             blocks[t.targets[1].value]);
        break;
      }
      case ir::Exit::return_:
        if (sig.throws_type) {
          if (type(sig.result).kind != hir::TypeKind::Unit)
            builder.CreateStore(values.at(t.inputs[0]),
                                fn->getArg(sig.parameters.size()));
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
    if (!sig.throws_type) {
      llvm::Value *value = builder.CreateCall(chosen->second);
      builder.CreateRet(result.kind == hir::TypeKind::Unit ? builder.getInt32(0)
                                                           : value);
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
      builder.CreateRet(value ? static_cast<llvm::Value *>(builder.CreateLoad(
                                    lower_type(sig.result), value))
                              : builder.getInt32(0));
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
      builder.CreateRet(builder.getInt32(1));
    }
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
    r.diagnostics.push_back(
        {"E_IR_INVALID", "codegen", "error", e.what(), {}, {}, {}, {}, {}, {}});
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
