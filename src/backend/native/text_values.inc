  llvm::Value *value_address(TypeId id, llvm::Value *value) {
    auto *address = storage(lower_type(id));
    builder.CreateStore(value, address);
    return address;
  }
  llvm::Value *text_binary(const ir::Instruction &i, llvm::Value *a,
                            llvm::Value *b) {
    const auto input_type = value_types.at(i.inputs[0]);
    auto *left = value_address(input_type, a), *right = value_address(input_type, b);
    using B = hir::BinaryOp;
    if (i.binary == B::add) {
      auto *out = storage(lower_type(i.type));
      runtime("lang_text_concat_v1", builder.getVoidTy(), {out, left, right});
      return builder.CreateLoad(lower_type(i.type), out);
    }
    auto *order = runtime("lang_text_compare_v1", builder.getInt32Ty(), {left, right});
    auto *zero = builder.getInt32(0);
    switch (i.binary) {
    case B::equal: return builder.CreateICmpEQ(order, zero);
    case B::not_equal: return builder.CreateICmpNE(order, zero);
    case B::less: return builder.CreateICmpSLT(order, zero);
    case B::less_equal: return builder.CreateICmpSLE(order, zero);
    case B::greater: return builder.CreateICmpSGT(order, zero);
    case B::greater_equal: return builder.CreateICmpSGE(order, zero);
    default: fail("invalid verified text operator");
    }
    return nullptr;
  }
  llvm::Value *value_system_call(const ir::Instruction &i,
                                  llvm::ArrayRef<llvm::Value *> args) {
    using hir::SystemOperation;
    const auto sig = type(i.signature);
    auto pointer = [&](unsigned index) { return value_address(sig.parameters[index].type, args[index]); };
    switch (*i.system) {
    case SystemOperation::text_len: return builder.CreateExtractValue(args[0], {2});
    case SystemOperation::text_byte_len: return builder.CreateExtractValue(args[0], {1});
    case SystemOperation::text_bytes:
      return runtime("lang_text_bytes_v1", builder.getPtrTy(), {pointer(0)});
    case SystemOperation::text_from_bytes:
    case SystemOperation::text_from_rune: {
      const auto text = type(i.type).type_args.at(0);
      auto *out = storage(lower_type(text));
      auto *status = runtime(*i.system == SystemOperation::text_from_bytes
                                  ? "lang_text_from_bytes_v1" : "lang_text_from_rune_v1",
                             builder.getInt64Ty(), {out, args[0]});
      return stdlib_result(i.type, status,
          [&] { return builder.CreateLoad(lower_type(text), out); });
    }
    default: break;
    }
    auto element = [&] {
      if (type(i.type).kind == hir::TypeKind::Vec) return type(i.type).type_args.at(0);
      return type(sig.parameters.at(0).type).type_args.at(0);
    };
    switch (*i.system) {
    case SystemOperation::vec_empty:
    case SystemOperation::vec_make: {
      auto t = element();
      auto callback = [&](bool copy) -> llvm::Value * {
        return owning(t) ? static_cast<llvm::Value *>(value_op(t, copy))
                         : llvm::ConstantPointerNull::get(builder.getPtrTy());
      };
      auto *value = runtime("lang_vec_new_v1", builder.getPtrTy(),
                            {builder.getInt64(0), builder.getInt64(stride(t)), callback(true), callback(false)});
      if (*i.system == SystemOperation::vec_make)
        runtime("lang_vec_resize_v1", builder.getVoidTy(), {value, args[0], element_storage(t, args[1])});
      return value;
    }
    case SystemOperation::vec_len: return vec_field(args[0], 1, builder.getInt64Ty());
    case SystemOperation::vec_capacity: return vec_field(args[0], 8, builder.getInt64Ty());
    case SystemOperation::vec_reserve:
      runtime("lang_vec_reserve_v1", builder.getVoidTy(), {args[0], args[1]}); break;
    case SystemOperation::vec_push: {
      auto t = element();
      auto *copy = copy_value(t, args[1]);
      runtime("lang_vec_push_v1", builder.getVoidTy(), {args[0], element_storage(t, copy)}); break;
    }
    case SystemOperation::vec_pop: {
      auto t = element();
      auto *layout = lower_type(t);
      const bool empty = module.getDataLayout().getTypeAllocSize(layout).getFixedValue() == 0;
      auto *out = storage(empty ? builder.getInt8Ty() : layout);
      auto *present = runtime("lang_vec_pop_v1", builder.getInt1Ty(), {args[0], out});
      return stdlib_option(i.type, present,
          [&] { return builder.CreateLoad(layout, out); });
    }
    case SystemOperation::vec_resize:
      runtime("lang_vec_resize_v1", builder.getVoidTy(), {args[0], args[1], element_storage(element(), args[2])}); break;
    case SystemOperation::vec_clear:
      runtime("lang_vec_clear_v1", builder.getVoidTy(), {args[0]}); break;
    default: fail("invalid verified value operation");
    }
    return builder.getInt8(0);
  }
